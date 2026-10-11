/* Bounded iOS file diagnostics. No lookup results are cached or changed.
 * Copyright 2026 Madeira fork contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef __WINE_IOS_FILE_PROFILE_H
#define __WINE_IOS_FILE_PROFILE_H

#ifndef IOS_FILE_PROFILE_NOW
static uint64_t ios_file_profile_now(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
    return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
#define IOS_FILE_PROFILE_NOW() ios_file_profile_now()
#endif
#ifndef IOS_FILE_PROFILE_EMIT
#define IOS_FILE_PROFILE_EMIT(...) dprintf(2, __VA_ARGS__)
#endif
#ifndef IOS_FILE_PROFILE_TID
#define IOS_FILE_PROFILE_TID() ((unsigned int)(ULONG_PTR)NtCurrentTeb()->ClientId.UniqueThread)
#endif

#define IOS_FILE_COUNTERS(X) \
    X(full_hit) X(full_miss) X(parent_try) X(parent_hit) X(components) \
    X(component_miss) X(case_query) X(scans) X(entries) X(prefix_hint) X(reparse_saved)

enum ios_file_profile_operation { IOS_FILE_CREATE, IOS_FILE_QUERY, IOS_FILE_QUERY_FULL, IOS_FILE_OPERATIONS };

struct ios_file_profile_call
{
    uint64_t start, lookup_ns;
    enum ios_file_profile_operation operation;
#define IOS_FILE_FIELD(n) uint64_t n;
    IOS_FILE_COUNTERS(IOS_FILE_FIELD)
#undef IOS_FILE_FIELD
};

struct ios_file_profile_sample
{
    uint64_t total_ns, lookup_ns, parent_hit, components, scans, entries;
    unsigned int status, disposition;
    enum ios_file_profile_operation operation;
    char path[256];
};

static __thread struct ios_file_profile_call *ios_file_profile_active;
static __thread struct ios_file_profile_totals
{
    uint64_t since, calls, ok, fail, lookup_ns, total_ns, audio_ok, audio_fail;
    uint64_t operations[IOS_FILE_OPERATIONS];
#define IOS_FILE_FIELD(n) uint64_t n;
    IOS_FILE_COUNTERS(IOS_FILE_FIELD)
#undef IOS_FILE_FIELD
    struct ios_file_profile_sample worst, failure, audio;
} ios_file_profile_stats;
static int ios_file_profile_enabled = -1;

#define IOS_FILE_COUNT(n) do { if (ios_file_profile_active) ios_file_profile_active->n++; } while (0)

static void ios_file_profile_begin(struct ios_file_profile_call *call, enum ios_file_profile_operation operation)
{
    int enabled = __atomic_load_n(&ios_file_profile_enabled, __ATOMIC_RELAXED);
    int saved_errno = errno;
    if (enabled < 0)
    {
        /* Diagnostic candidate: enabled by default; 0 removes clocks and logging. */
        const char *value = getenv("MADEIRA_FILE_PROFILE");
        enabled = !value || strcmp(value, "0");
        __atomic_store_n(&ios_file_profile_enabled, enabled, __ATOMIC_RELAXED);
    }
    if (enabled && !ios_file_profile_active)
    {
        memset(call, 0, sizeof(*call));
        call->operation = operation;
        call->start = IOS_FILE_PROFILE_NOW();
        if (!ios_file_profile_stats.since) ios_file_profile_stats.since = call->start;
        ios_file_profile_active = call;
    }
    errno = saved_errno;
}

static void ios_file_profile_lookup_done(struct ios_file_profile_call *call)
{
    int saved_errno = errno;
    if (ios_file_profile_active == call) call->lookup_ns = IOS_FILE_PROFILE_NOW() - call->start;
    errno = saved_errno;
}

static int ios_file_profile_audio(const WCHAR *name, unsigned int length)
{
    static const char *const extensions[] = {".bank", ".fsb", ".fev", ".wem", ".wav", ".ogg", ".mp3"};
    unsigned int i, j;
    if (!name) return 0;
    for (i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++)
    {
        unsigned int n = strlen(extensions[i]);
        if (length < n) continue;
        for (j = 0; j < n; j++)
        {
            WCHAR c = name[length - n + j];
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (c != (unsigned char)extensions[i][j]) break;
        }
        if (j == n) return 1;
    }
    return 0;
}

static void ios_file_profile_sample(struct ios_file_profile_sample *s,
                                   const struct ios_file_profile_call *call, uint64_t total,
                                   const WCHAR *name, unsigned int length,
                                   unsigned int disposition, unsigned int status)
{
    unsigned int i = 0, start = 0;
    s->total_ns = total;
    s->lookup_ns = call->lookup_ns;
    s->parent_hit = call->parent_hit;
    s->components = call->components;
    s->scans = call->scans;
    s->entries = call->entries;
    s->status = status;
    s->disposition = disposition;
    s->operation = call->operation;
    if (!name) length = 0;
    if (length >= sizeof(s->path))
    {
        memcpy(s->path, "...", 3);
        i = 3;
        start = length - (sizeof(s->path) - 4);
    }
    while (start < length && i < sizeof(s->path) - 1)
    {
        WCHAR c = name[start++];
        s->path[i++] = c >= 32 && c < 127 ? c : '?';
    }
    s->path[i] = 0;
}

static void ios_file_profile_print(const char *kind, const struct ios_file_profile_sample *s)
{
    static const char *const operations[] = {"create", "query", "query-full"};
    if (!s->path[0]) return;
    IOS_FILE_PROFILE_EMIT("[file-profile-path] tid=%04x kind=%s total_us=%llu lookup_us=%llu "
                         "parent_hit=%llu components=%llu scans=%llu entries=%llu "
                         "status=%08x disp=%u op=%s name=%s\n", IOS_FILE_PROFILE_TID(), kind,
                         (unsigned long long)(s->total_ns / 1000), (unsigned long long)(s->lookup_ns / 1000),
                         (unsigned long long)s->parent_hit, (unsigned long long)s->components,
                         (unsigned long long)s->scans, (unsigned long long)s->entries,
                         s->status, s->disposition, operations[s->operation], s->path);
}

static void ios_file_profile_end(struct ios_file_profile_call *call, const WCHAR *name,
                                unsigned int length, unsigned int disposition, unsigned int status)
{
    struct ios_file_profile_totals *s = &ios_file_profile_stats;
    uint64_t now, total;
    int saved_errno = errno;
    if (ios_file_profile_active != call) return;
    ios_file_profile_active = NULL; /* reporting cannot become a nested measured operation */
    now = IOS_FILE_PROFILE_NOW();
    total = now - call->start;
    s->calls++;
    s->operations[call->operation]++;
    if (status) s->fail++; else s->ok++;
    s->total_ns += total;
    s->lookup_ns += call->lookup_ns;
#define IOS_FILE_ADD(n) s->n += call->n;
    IOS_FILE_COUNTERS(IOS_FILE_ADD)
#undef IOS_FILE_ADD
    if (!s->worst.path[0] || total > s->worst.total_ns)
        ios_file_profile_sample(&s->worst, call, total, name, length, disposition, status);
    /* Keep the costliest read-open/query failure rather than startup CREATE collisions. */
    if (status && disposition == 1 && (!s->failure.path[0] || total > s->failure.total_ns))
        ios_file_profile_sample(&s->failure, call, total, name, length, disposition, status);
    if (call->operation == IOS_FILE_CREATE && ios_file_profile_audio(name, length))
    {
        if (status) s->audio_fail++; else s->audio_ok++;
        /* A successful open cannot erase the failed audio path in this interval. */
        if (!s->audio.path[0] || !s->audio.status || (status && total > s->audio.total_ns))
            ios_file_profile_sample(&s->audio, call, total, name, length, disposition, status);
    }
    if (now - s->since >= 5000000000ULL)
    {
        IOS_FILE_PROFILE_EMIT("[file-profile] tid=%04x span_ms=%llu calls=%llu ok=%llu fail=%llu "
                             "lookup_us=%llu total_us=%llu full_hit=%llu full_miss=%llu "
                             "parent_try=%llu parent_hit=%llu components=%llu component_miss=%llu "
                             "case_query=%llu scans=%llu entries=%llu audio_ok=%llu audio_fail=%llu "
                             "create=%llu query=%llu query_full=%llu prefix_hint=%llu reparse_saved=%llu\n",
                             IOS_FILE_PROFILE_TID(), (unsigned long long)((now - s->since) / 1000000),
                             (unsigned long long)s->calls, (unsigned long long)s->ok, (unsigned long long)s->fail,
                             (unsigned long long)(s->lookup_ns / 1000), (unsigned long long)(s->total_ns / 1000),
                             (unsigned long long)s->full_hit, (unsigned long long)s->full_miss,
                             (unsigned long long)s->parent_try, (unsigned long long)s->parent_hit,
                             (unsigned long long)s->components, (unsigned long long)s->component_miss,
                             (unsigned long long)s->case_query, (unsigned long long)s->scans,
                             (unsigned long long)s->entries, (unsigned long long)s->audio_ok,
                             (unsigned long long)s->audio_fail,
                             (unsigned long long)s->operations[IOS_FILE_CREATE],
                             (unsigned long long)s->operations[IOS_FILE_QUERY],
                             (unsigned long long)s->operations[IOS_FILE_QUERY_FULL],
                             (unsigned long long)s->prefix_hint, (unsigned long long)s->reparse_saved);
        ios_file_profile_print("worst", &s->worst);
        ios_file_profile_print("failed", &s->failure);
        ios_file_profile_print("audio", &s->audio);
        memset(s, 0, sizeof(*s));
        s->since = now;
    }
    errno = saved_errno;
}
#endif
