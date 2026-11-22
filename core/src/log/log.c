// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Structured JSON-lines logging sink.
//
// A2: core API + sync stdout sink.
// L2+L3: MPSC ring buffer + dedicated drain thread (this file).
//
// After log_init():
//   producers serialize the event on their own stack, then push the formatted
//   line into a bounded ring. A single drain thread pops and writes to the
//   current sink. Producers never block — if the ring is full, the event is
//   dropped and a counter ticks.
//
// Before log_init() or after log_shutdown():
//   emit falls back to synchronous direct-to-sink writes (useful for very
//   early startup, test harnesses that don't init, and crash paths).

#define _POSIX_C_SOURCE 200809L

#include "v3/log.h"

#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ------------------------------------------------------------
// Sink
// ------------------------------------------------------------

typedef enum sink_kind {
    SINK_STDOUT = 0,
    SINK_BUFFER,
} sink_kind_t;

struct log_sink {
    sink_kind_t kind;
    // SINK_BUFFER only:
    char* buffer;
    size_t buffer_len;
    size_t buffer_cap;
};

static struct log_sink STDOUT_SINK = {.kind = SINK_STDOUT};
static struct log_sink* g_sink = &STDOUT_SINK;
static log_level_t g_level = LOG_INFO;

// Forward declarations: ring_push and g_initialized are defined below but
// referenced by log_emit_ above them. Keeping log_emit_ near the formatters
// reads better; a couple of forwards is the simpler fix.
static void ring_push(const char* data, size_t len);
static atomic_bool g_initialized;

// ------------------------------------------------------------
// Output buffer helpers
// ------------------------------------------------------------

// Frama-C audit finding LOG-4: snprintf returns the would-have-written count,
// which can exceed `sizeof(buf)` when the format output is truncated.
// Clamp `n` to `(int)sizeof(buf) - 1` before using it as the loop bound so the
// subsequent `buf[i]` read stays in-bounds even if a future format string
// produces longer output. (Current formats all fit; this is defense in depth.)
#define APPEND_BUF_CLAMP(buf, n) \
    ((n) < 0 ? 0 : ((n) < (int)sizeof(buf) ? (n) : (int)sizeof(buf) - 1))

static void append_byte(char** p, char* end, char c) {
    if (*p < end) {
        **p = c;
        (*p)++;
    }
}

static void append_raw(char** p, char* end, const char* s) {
    while (*s && *p < end) {
        *(*p)++ = *s++;
    }
}

static void append_json_string(char** p, char* end, const char* s) {
    append_byte(p, end, '"');
    if (s == NULL) {
        append_byte(p, end, '"');
        return;
    }
    while (*s && *p < end - 1) {
        unsigned char c = (unsigned char)*s++;
        switch (c) {
        case '"':
            append_byte(p, end, '\\');
            append_byte(p, end, '"');
            break;
        case '\\':
            append_byte(p, end, '\\');
            append_byte(p, end, '\\');
            break;
        case '\n':
            append_byte(p, end, '\\');
            append_byte(p, end, 'n');
            break;
        case '\r':
            append_byte(p, end, '\\');
            append_byte(p, end, 'r');
            break;
        case '\t':
            append_byte(p, end, '\\');
            append_byte(p, end, 't');
            break;
        case '\b':
            append_byte(p, end, '\\');
            append_byte(p, end, 'b');
            break;
        case '\f':
            append_byte(p, end, '\\');
            append_byte(p, end, 'f');
            break;
        default:
            if (c < 0x20) {
                char buf[8];
                int n = snprintf(buf, sizeof(buf), "\\u%04x", c);
                int actual = APPEND_BUF_CLAMP(buf, n);  // LOG-4
                for (int i = 0; i < actual && *p < end; i++) {
                    append_byte(p, end, buf[i]);
                }
            } else {
                append_byte(p, end, (char)c);
            }
        }
    }
    append_byte(p, end, '"');
}

static void append_int(char** p, char* end, int64_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%lld", (long long)v);
    int actual = APPEND_BUF_CLAMP(buf, n);
    for (int i = 0; i < actual && *p < end; i++) {
        append_byte(p, end, buf[i]);
    }
}

static void append_uint(char** p, char* end, uint64_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v);
    int actual = APPEND_BUF_CLAMP(buf, n);
    for (int i = 0; i < actual && *p < end; i++) {
        append_byte(p, end, buf[i]);
    }
}

static void append_float(char** p, char* end, double v) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%.6g", v);
    int actual = APPEND_BUF_CLAMP(buf, n);
    for (int i = 0; i < actual && *p < end; i++) {
        append_byte(p, end, buf[i]);
    }
}

// ------------------------------------------------------------
// Sink write
// ------------------------------------------------------------

static void sink_write_line(struct log_sink* sink, const char* data, size_t len) {
    switch (sink->kind) {
    case SINK_STDOUT:
        fwrite(data, 1, len, stdout);
        fputc('\n', stdout);
        fflush(stdout);
        break;
    case SINK_BUFFER: {
        // Frama-C audit finding LOG-5: guard against
        //  (a) `buffer_len + len + 2` wrap.
        //  (b) `buffer_cap * 2` doubling wrap.
        if (len > SIZE_MAX - sink->buffer_len - 2) return;
        size_t need = sink->buffer_len + len + 2;
        if (need > sink->buffer_cap) {
            if (sink->buffer_cap > SIZE_MAX / 2) return;
            size_t new_cap = sink->buffer_cap ? sink->buffer_cap * 2 : 4096;
            while (new_cap < need) {
                if (new_cap > SIZE_MAX / 2) return;
                new_cap *= 2;
            }
            char* nb = realloc(sink->buffer, new_cap);
            if (!nb) {
                return;
            }
            sink->buffer = nb;
            sink->buffer_cap = new_cap;
        }
        memcpy(sink->buffer + sink->buffer_len, data, len);
        sink->buffer_len += len;
        sink->buffer[sink->buffer_len++] = '\n';
        sink->buffer[sink->buffer_len] = '\0';
        break;
    }
    }
}

// ------------------------------------------------------------
// Time / level helpers
// ------------------------------------------------------------

static const char* level_name(log_level_t l) {
    switch (l) {
    case LOG_DEBUG:
        return "debug";
    case LOG_INFO:
        return "info";
    case LOG_WARN:
        return "warn";
    case LOG_ERROR:
        return "error";
    case LOG_FATAL:
        return "fatal";
    }
    return "?";
}

static void format_iso8601(char* out, size_t out_len, struct timespec ts) {
    // Frama-C audit finding LOG-3: gmtime_r can return NULL on
    // out-of-representable-range time_t. The output `struct tm` is then
    // indeterminate; reading tm.tm_* below would be UB. Fall back to
    // a fixed epoch string so the log line stays well-formed JSON.
    struct tm tm;
    if (gmtime_r(&ts.tv_sec, &tm) == NULL) {
        snprintf(out, out_len, "1970-01-01T00:00:00.000Z");
        return;
    }
    int ms = (int)(ts.tv_nsec / 1000000);
    snprintf(out, out_len, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
}

static int eq_ascii_ci(const char* a, const char* b) {
    if (!a || !b) {
        return 0;
    }
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static log_level_t parse_level(const char* s) {
    if (!s) {
        return LOG_INFO;
    }
    if (eq_ascii_ci(s, "debug")) {
        return LOG_DEBUG;
    }
    if (eq_ascii_ci(s, "info")) {
        return LOG_INFO;
    }
    if (eq_ascii_ci(s, "warn") || eq_ascii_ci(s, "warning")) {
        return LOG_WARN;
    }
    if (eq_ascii_ci(s, "error")) {
        return LOG_ERROR;
    }
    if (eq_ascii_ci(s, "fatal")) {
        return LOG_FATAL;
    }
    return LOG_INFO;
}

// ------------------------------------------------------------
// Emit
// ------------------------------------------------------------

void log_emit_(subsystem_t subsystem, log_level_t level, event_kind_t event_kind,
               const log_field_t* fields, size_t n_fields) {
    if (level < g_level) {
        return;
    }

    // Frama-C audit finding LOG-3: clock_gettime can fail (EINVAL on bad
    // clk_id, EFAULT on bad address). Zero-init the timespec values so a
    // failure produces a well-formed epoch-flavored timestamp rather than
    // reading indeterminate memory.
    struct timespec ts_wall = {0}, ts_mono = {0};
    (void)clock_gettime(CLOCK_REALTIME, &ts_wall);
    (void)clock_gettime(CLOCK_MONOTONIC, &ts_mono);

    char iso[32];
    format_iso8601(iso, sizeof(iso), ts_wall);
    uint64_t mono_ns = (uint64_t)ts_mono.tv_sec * 1000000000ULL + (uint64_t)ts_mono.tv_nsec;

    // Upper bound for one event line. Oversize fields get truncated inside the
    // helpers; worst case we lose trailing bytes but never corrupt JSON shape.
    char buf[4096];
    char* p = buf;
    char* end = buf + sizeof(buf) - 1;

    append_byte(&p, end, '{');

    append_raw(&p, end, "\"t\":");
    append_json_string(&p, end, iso);

    append_raw(&p, end, ",\"mono_ns\":");
    append_uint(&p, end, mono_ns);

    append_raw(&p, end, ",\"level\":");
    append_json_string(&p, end, level_name(level));

    append_raw(&p, end, ",\"subsystem\":");
    append_json_string(&p, end, subsystem_name(subsystem));

    append_raw(&p, end, ",\"event\":");
    append_json_string(&p, end, event_kind_name(event_kind));

    for (size_t i = 0; i < n_fields; i++) {
        const log_field_t* f = &fields[i];
        append_byte(&p, end, ',');
        append_json_string(&p, end, f->key ? f->key : "?");
        append_byte(&p, end, ':');
        switch (f->value.type) {
        case LV_INT:
            append_int(&p, end, f->value.i);
            break;
        case LV_UINT:
            append_uint(&p, end, f->value.u);
            break;
        case LV_FLOAT:
            append_float(&p, end, f->value.f);
            break;
        case LV_STR:
            if (f->value.s) {
                append_json_string(&p, end, f->value.s);
            } else {
                append_raw(&p, end, "null");
            }
            break;
        case LV_BOOL:
            append_raw(&p, end, f->value.b ? "true" : "false");
            break;
        }
    }

    append_byte(&p, end, '}');
    *p = '\0';

    if (atomic_load_explicit(&g_initialized, memory_order_acquire)) {
        ring_push(buf, (size_t)(p - buf));
    } else {
        // Fallback for pre-init / post-shutdown / unit tests that don't init.
        sink_write_line(g_sink, buf, (size_t)(p - buf));
    }
}

// ------------------------------------------------------------
// Ring buffer (MPSC) + drain thread
// ------------------------------------------------------------

#define RING_CAPACITY 4096
#define RING_SLOT_BYTES 1024 // longest event line we keep; longer lines get truncated

typedef struct log_slot {
    char buf[RING_SLOT_BYTES];
    size_t len;
} log_slot_t;

static struct {
    pthread_mutex_t lock;
    pthread_cond_t not_empty; // drain waits here
    pthread_cond_t drained;   // log_flush waits here
    log_slot_t slots[RING_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    size_t in_flight; // popped from the ring but not yet written to the sink
    bool shutdown;
    atomic_uint_least64_t dropped; // events discarded because ring was full
    uint64_t reported_dropped;     // last value we emitted a summary for
} g_ring;

static pthread_t g_drain_tid;
static atomic_bool g_initialized = false;

static void ring_push(const char* data, size_t len) {
    pthread_mutex_lock(&g_ring.lock);
    if (g_ring.count == RING_CAPACITY) {
        atomic_fetch_add_explicit(&g_ring.dropped, 1, memory_order_relaxed);
        pthread_mutex_unlock(&g_ring.lock);
        return;
    }
    log_slot_t* slot = &g_ring.slots[g_ring.tail];
    size_t copy_len = len < RING_SLOT_BYTES ? len : RING_SLOT_BYTES;
    memcpy(slot->buf, data, copy_len);
    slot->len = copy_len;
    g_ring.tail = (g_ring.tail + 1) % RING_CAPACITY;
    g_ring.count++;
    pthread_cond_signal(&g_ring.not_empty);
    pthread_mutex_unlock(&g_ring.lock);
}

// Synthesize and emit a "dropped events" summary when the counter advances.
// Called from the drain thread only, so we can write straight to the sink.
static void emit_dropped_summary_if_needed(void) {
    uint64_t current = atomic_load_explicit(&g_ring.dropped, memory_order_relaxed);
    if (current == g_ring.reported_dropped) {
        return;
    }
    uint64_t lost = current - g_ring.reported_dropped;
    g_ring.reported_dropped = current;

    // LOG-3 (second site): same defensive zero-init.
    struct timespec ts_wall = {0}, ts_mono = {0};
    (void)clock_gettime(CLOCK_REALTIME, &ts_wall);
    (void)clock_gettime(CLOCK_MONOTONIC, &ts_mono);
    char iso[32];
    format_iso8601(iso, sizeof(iso), ts_wall);
    uint64_t mono_ns = (uint64_t)ts_mono.tv_sec * 1000000000ULL + (uint64_t)ts_mono.tv_nsec;

    char line[256];
    int n = snprintf(line, sizeof(line),
                     "{\"t\":\"%s\",\"mono_ns\":%llu,\"level\":\"warn\","
                     "\"subsystem\":\"log\",\"event\":\"dropped\",\"count\":%llu,"
                     "\"total\":%llu}",
                     iso, (unsigned long long)mono_ns, (unsigned long long)lost,
                     (unsigned long long)current);
    // LOG-4: clamp the written length to the buffer size so a future format
    // change that could over-produce can't make sink_write_line read past `line`.
    if (n > 0) {
        size_t actual = ((size_t)n < sizeof(line)) ? (size_t)n : sizeof(line) - 1;
        sink_write_line(g_sink, line, actual);
    }
}

static void* drain_loop(void* arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_ring.lock);
        while (g_ring.count == 0 && !g_ring.shutdown) {
            pthread_cond_wait(&g_ring.not_empty, &g_ring.lock);
        }

        // Drain a batch while holding the lock (fast — just memcpy out of the ring).
        log_slot_t batch[16];
        size_t n = 0;
        while (n < 16 && g_ring.count > 0) {
            batch[n++] = g_ring.slots[g_ring.head];
            g_ring.head = (g_ring.head + 1) % RING_CAPACITY;
            g_ring.count--;
        }
        bool done = (g_ring.count == 0) && g_ring.shutdown;
        // Mark this batch popped-but-unwritten so log_flush can't return until
        // it has actually reached the sink — count==0 alone only means "popped
        // out of the ring", not "written". Broadcasting `drained` here (the old
        // behaviour) woke log_flush while the last batch was still in-flight
        // between this unlock and the sink writes below, so a concurrent
        // reader saw a short buffer (flaky under load — see log_test).
        g_ring.in_flight = n;
        pthread_mutex_unlock(&g_ring.lock);

        // Writes happen outside the lock so producers never wait on I/O.
        for (size_t i = 0; i < n; i++) {
            sink_write_line(g_sink, batch[i].buf, batch[i].len);
        }
        emit_dropped_summary_if_needed();

        // Batch is now durably in the sink: clear the in-flight marker and wake
        // log_flush iff the ring is fully drained AND nothing is mid-write.
        pthread_mutex_lock(&g_ring.lock);
        g_ring.in_flight = 0;
        if (g_ring.count == 0) {
            pthread_cond_broadcast(&g_ring.drained);
        }
        pthread_mutex_unlock(&g_ring.lock);

        if (done) {
            return NULL;
        }
    }
}

// ------------------------------------------------------------
// Lifecycle + config
// ------------------------------------------------------------

void log_init(void) {
    // Always re-read env (cheap; lets tests change level between runs).
    g_level = parse_level(getenv("DIORAMAP_LOG_LEVEL"));

    // Two-phase atomic guard so we satisfy three contradictory needs:
    //   (a) reentrant log_init() must be idempotent (old behavior),
    //   (b) racing log_init() callers from N threads must pick exactly one
    //       winner (old behavior — was done via the atomic_exchange below),
    //   (c) the "module is ready" flag (g_initialized) must be set ONLY
    //       AFTER pthread_create succeeds, else a thread-spawn failure
    //       leaves a falsely-ready module behind (Frama-C audit finding
    //       LOG-1).
    //
    // The old code used a single atomic_exchange that simultaneously
    // claimed "I'm the initializer" and "init is done" — works as long
    // as init can't fail, which is exactly the precondition LOG-1
    // exposed. We now split: `g_init_in_progress` is the "claim" flag,
    // `g_initialized` is the "ready" flag. On failure we release the
    // claim; the next caller can retry.
    static atomic_int g_init_in_progress = 0;
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &g_init_in_progress, &expected, 1,
            memory_order_acq_rel, memory_order_acquire)) {
        return; // another thread is initializing or did so already
    }
    if (atomic_load_explicit(&g_initialized, memory_order_acquire)) {
        atomic_store_explicit(&g_init_in_progress, 0, memory_order_release);
        return; // already done
    }

    // Frama-C audit finding LOG-2: check every pthread_* return so we
    // can roll back cleanly on failure. Glibc never fails any of these
    // for plain (NULL-attr) primitives in practice, but the API allows
    // it under resource pressure, and the kernel's rollback policy is
    // "if anything fails, leave g_initialized=false and fall back to
    // synchronous emit forever".
    if (pthread_mutex_init(&g_ring.lock, NULL) != 0) {
        atomic_store_explicit(&g_init_in_progress, 0, memory_order_release);
        return;
    }
    if (pthread_cond_init(&g_ring.not_empty, NULL) != 0) {
        pthread_mutex_destroy(&g_ring.lock);
        atomic_store_explicit(&g_init_in_progress, 0, memory_order_release);
        return;
    }
    if (pthread_cond_init(&g_ring.drained, NULL) != 0) {
        pthread_cond_destroy(&g_ring.not_empty);
        pthread_mutex_destroy(&g_ring.lock);
        atomic_store_explicit(&g_init_in_progress, 0, memory_order_release);
        return;
    }
    g_ring.head = g_ring.tail = g_ring.count = 0;
    g_ring.shutdown = false;
    atomic_store_explicit(&g_ring.dropped, 0, memory_order_relaxed);
    g_ring.reported_dropped = 0;

    // LOG-1: if pthread_create fails, roll back ring state and leave
    // g_initialized=false so emit stays in synchronous-fallback mode.
    if (pthread_create(&g_drain_tid, NULL, drain_loop, NULL) != 0) {
        pthread_cond_destroy(&g_ring.drained);
        pthread_cond_destroy(&g_ring.not_empty);
        pthread_mutex_destroy(&g_ring.lock);
        atomic_store_explicit(&g_init_in_progress, 0, memory_order_release);
        return;
    }
    // Everything wired up; expose the initialized state to emit / shutdown.
    atomic_store_explicit(&g_initialized, true, memory_order_release);
    // Keep g_init_in_progress = 1 — that signals "init complete" to future
    // callers (they'll see g_initialized via the acquire-load above and
    // bail). We never go back to 0 except on failure rollback.
}

void log_shutdown(void) {
    if (!atomic_load_explicit(&g_initialized, memory_order_acquire)) {
        fflush(stdout);
        return;
    }

    pthread_mutex_lock(&g_ring.lock);
    g_ring.shutdown = true;
    pthread_cond_broadcast(&g_ring.not_empty);
    pthread_mutex_unlock(&g_ring.lock);

    pthread_join(g_drain_tid, NULL);

    pthread_cond_destroy(&g_ring.drained);
    pthread_cond_destroy(&g_ring.not_empty);
    pthread_mutex_destroy(&g_ring.lock);

    atomic_store_explicit(&g_initialized, false, memory_order_release);
    fflush(stdout);
}

void log_flush(void) {
    if (!atomic_load_explicit(&g_initialized, memory_order_acquire)) {
        return;
    }
    pthread_mutex_lock(&g_ring.lock);
    while (g_ring.count > 0 || g_ring.in_flight > 0) {
        pthread_cond_wait(&g_ring.drained, &g_ring.lock);
    }
    pthread_mutex_unlock(&g_ring.lock);
}

void log_set_level(log_level_t level) {
    g_level = level;
}

log_level_t log_get_level(void) {
    return g_level;
}

// ------------------------------------------------------------
// Testing sink support
// ------------------------------------------------------------

log_sink_t* log_sink_new_buffer(void) {
    log_sink_t* s = calloc(1, sizeof(log_sink_t));
    if (!s) {
        return NULL;
    }
    s->kind = SINK_BUFFER;
    s->buffer_cap = 4096;
    s->buffer = calloc(1, s->buffer_cap);
    if (!s->buffer) {
        free(s);
        return NULL;
    }
    return s;
}

void log_sink_free(log_sink_t* sink) {
    if (!sink || sink == &STDOUT_SINK) {
        return;
    }
    free(sink->buffer);
    free(sink);
}

const char* log_sink_buffer_contents(log_sink_t* sink) {
    if (!sink || sink->kind != SINK_BUFFER) {
        return NULL;
    }
    return sink->buffer ? sink->buffer : "";
}

log_sink_t* log_sink_swap_for_testing(log_sink_t* new_sink) {
    // Drain pending events to the OLD sink before swapping so tests don't see
    // events from previous tests in the new buffer.
    log_flush();
    log_sink_t* old = g_sink;
    g_sink = new_sink ? new_sink : &STDOUT_SINK;
    return old;
}
