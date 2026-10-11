/* Lazy directory-name indexes for iOS asset lookups.
 * Copyright 2026 Madeira fork contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Included by file.c after get_dir_case_sensitivity().
 */
#ifndef __WINE_IOS_DIRECTORY_INDEX_H
#define __WINE_IOS_DIRECTORY_INDEX_H

#ifndef IOS_INDEX_TEST
#include <sys/event.h>
#endif

#define IOS_INDEX_SLOTS 32
#define IOS_INDEX_BYTES 65536
#define IOS_INDEX_BUCKETS 256

struct ios_index_record
{
    unsigned int next, hash, length, bytes;
    /* WCHAR name[length], char native[bytes + 1], padding to 4 bytes. */
};

struct ios_index_dir
{
    struct stat stamp;
    unsigned char *data;
    unsigned int used;
    uint64_t age;
    int fd, dirty, overflow;
};

static struct ios_index_dir ios_index_dirs[IOS_INDEX_SLOTS];
static pthread_mutex_t ios_index_mutex = PTHREAD_MUTEX_INITIALIZER;
static int ios_index_queue = -1, ios_index_enabled = -1;
static uint64_t ios_index_age;

static BOOL ios_index_same(const struct stat *a, const struct stat *b)
{
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
           a->st_mode == b->st_mode && a->st_uid == b->st_uid && a->st_gid == b->st_gid &&
           a->st_mtimespec.tv_sec == b->st_mtimespec.tv_sec &&
           a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec &&
           a->st_ctimespec.tv_sec == b->st_ctimespec.tv_sec &&
           a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec;
}

static void ios_index_drop(struct ios_index_dir *d)
{
    if (d->age) close(d->fd); /* closing also removes the vnode filter */
    free(d->data);
    memset(d, 0, sizeof(*d));
}

/* Slots are static: a queued event for an evicted descriptor can only cause a
 * conservative invalidation, never reference freed memory. Do not free here:
 * the caller may be constructing one of these entries. */
static BOOL ios_index_poll(void)
{
    struct kevent events[IOS_INDEX_SLOTS];
    const struct timespec zero = {0, 0};
    int i, n = kevent(ios_index_queue, NULL, 0, events, IOS_INDEX_SLOTS, &zero);
    if (n < 0)
    {
        for (i = 0; i < IOS_INDEX_SLOTS; i++) ios_index_dirs[i].dirty = 1;
        return FALSE;
    }
    for (i = 0; i < n; i++)
    {
        struct ios_index_dir *d = events[i].udata;
        if (d) d->dirty = 1;
    }
    return TRUE;
}

static unsigned int ios_index_hash(const WCHAR *name, int length)
{
    unsigned int hash = 2166136261u;
    int i;
    for (i = 0; i < length; i++) hash = (hash ^ towupper(name[i])) * 16777619u;
    return hash;
}

static const char *ios_index_find(struct ios_index_dir *d, const WCHAR *name, int length)
{
    unsigned int hash = ios_index_hash(name, length), at, best = 0;
    for (at = ((unsigned int *)d->data)[hash % IOS_INDEX_BUCKETS]; at;)
    {
        struct ios_index_record *r = (void *)(d->data + at);
        const WCHAR *wide = (const WCHAR *)(r + 1);
        /* Preserve the first readdir spelling when case-folded names collide. */
        if (r->hash == hash && r->length == (unsigned int)length && !wcsnicmp(wide, name, length) && (!best || at < best))
            best = at;
        at = r->next;
    }
    if (best)
    {
        struct ios_index_record *r = (void *)(d->data + best);
        return (const char *)(r + 1) + r->length * sizeof(WCHAR);
    }
    return NULL;
}

/* -1 = uncertain/fallback, 0 = absent, 1 = matched spelling. The caller must
 * retain its native exact stat (including the literal reparse-name probe).
 * A fresh path stat prevents ancestor replacement, rename and root-fd reuse
 * from turning a watch on the old inode into authority for the new path. */
static int ios_index_lookup(int root, const char *path, const WCHAR *name, int length,
                            char *out, BOOL *has_reparse)
{
    struct ios_index_dir *d = NULL, *victim = NULL;
    struct stat before, after;
    struct kevent event;
    struct dirent *de;
    WCHAR wide[MAX_DIR_ENTRY_LEN + 1];
    const char *match;
    DIR *dir = NULL;
    int i, fd = -1, result = -1, saved_errno = errno;
    BOOL complete = FALSE;

    if (length < 1 || length > MAX_DIR_ENTRY_LEN || pthread_mutex_trylock(&ios_index_mutex)) return -1;
    if (ios_index_enabled < 0)
    {
        const char *value = getenv("MADEIRA_DIRECTORY_INDEX");
        ios_index_enabled = !value || strcmp(value, "0");
    }
    if (!ios_index_enabled) goto done;
    if (ios_index_queue < 0)
    {
        if ((ios_index_queue = kqueue()) < 0) goto done;
        fcntl(ios_index_queue, F_SETFD, FD_CLOEXEC);
    }
    if (fstatat(root, path, &before, 0) || !S_ISDIR(before.st_mode) || !ios_index_poll()) goto done;
    for (i = 0; i < IOS_INDEX_SLOTS; i++)
    {
        struct ios_index_dir *slot = &ios_index_dirs[i];
        if (!victim || slot->age < victim->age) victim = slot;
        if (slot->age && slot->stamp.st_dev == before.st_dev && slot->stamp.st_ino == before.st_ino)
        {
            d = slot;
            break;
        }
    }
    if (d && !d->dirty && ios_index_same(&d->stamp, &before))
    {
        d->age = ++ios_index_age;
        if (d->overflow) goto done;
        IOS_FILE_COUNT(index_hit);
        goto lookup;
    }
    if (!d) d = victim;
    else IOS_FILE_COUNT(index_stale);
    ios_index_drop(d);
    if ((fd = openat(root, path, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0) goto done;
    if (fstat(fd, &after) || !ios_index_same(&before, &after)) goto done;
    d->fd = fd; fd = -1;
    d->stamp = before;
    d->age = ++ios_index_age;
    EV_SET(&event, d->fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
           NOTE_WRITE | NOTE_ATTRIB | NOTE_DELETE | NOTE_RENAME | NOTE_LINK | NOTE_REVOKE, 0, d);
    if (kevent(ios_index_queue, &event, 1, NULL, 0, NULL)) goto failed;
    /* Do not change semantics on case-insensitive volumes. The normal exact
     * lookup already failed there. Mount/case attributes are revalidated by
     * the identity and metadata checks on subsequent uses. */
    IOS_FILE_COUNT(case_query);
    if (!get_dir_case_sensitivity(root, path)) goto failed;
    if (!(d->data = malloc(IOS_INDEX_BYTES))) goto failed;
    memset(d->data, 0, IOS_INDEX_BUCKETS * sizeof(unsigned int));
    d->used = IOS_INDEX_BUCKETS * sizeof(unsigned int);
    /* A distinct open file description avoids shared readdir offsets. */
    if ((fd = openat(d->fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0) goto failed;
    if (!(dir = fdopendir(fd))) goto failed;
    fd = -1;
    IOS_FILE_COUNT(scans);
    IOS_FILE_COUNT(index_build);
    for (;;)
    {
        unsigned int bytes, size, hash;
        struct ios_index_record *r;
        int count;
        errno = 0;
        if (!(de = readdir(dir))) { complete = !errno; break; }
        IOS_FILE_COUNT(entries);
        bytes = strlen(de->d_name);
        count = ntdll_umbstowcs(de->d_name, bytes, wide, MAX_DIR_ENTRY_LEN);
        if (bytes > MAX_DIR_ENTRY_LEN || count <= 0 || count > MAX_DIR_ENTRY_LEN) goto failed;
        size = (sizeof(*r) + count * sizeof(WCHAR) + bytes + 1 + 3) & ~3u;
        if (size > IOS_INDEX_BYTES - d->used)
        {
            /* Remember admission failure until this directory changes. */
            d->overflow = 1;
            free(d->data); d->data = NULL;
            goto done;
        }
        hash = ios_index_hash(wide, count);
        r = (void *)(d->data + d->used);
        r->next = ((unsigned int *)d->data)[hash % IOS_INDEX_BUCKETS];
        r->hash = hash; r->length = count; r->bytes = bytes;
        memcpy(r + 1, wide, count * sizeof(WCHAR));
        memcpy((char *)(r + 1) + count * sizeof(WCHAR), de->d_name, bytes + 1);
        ((unsigned int *)d->data)[hash % IOS_INDEX_BUCKETS] = d->used;
        d->used += size;
    }
    if (!complete || fstatat(root, path, &after, 0) || !ios_index_same(&before, &after) ||
        !ios_index_poll() || d->dirty) goto failed;
lookup:
    match = ios_index_find(d, name, length);
    if (match) { strcpy(out, match); result = 1; }
    else
    {
        memcpy(wide, name, length * sizeof(WCHAR)); wide[length] = '?';
        *has_reparse = !!ios_index_find(d, wide, length + 1);
        result = 0;
    }
    goto done;
failed:
    ios_index_drop(d);
done:
    if (dir) closedir(dir);
    if (fd >= 0) close(fd);
    if (result < 0) IOS_FILE_COUNT(index_fallback);
    pthread_mutex_unlock(&ios_index_mutex);
    errno = saved_errno;
    return result;
}
#endif
