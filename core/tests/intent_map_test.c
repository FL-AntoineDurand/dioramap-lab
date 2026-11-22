// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Unit tests for the intent_map subsystem (HASH-MODEL.md §6, PLAN-STORE S14).
//
// Covers: round-trip, persistence across open/close, tombstones, partial-tail
// recovery after a simulated crash mid-append, compaction, concurrent puts
// from multiple threads, and sizing invariants.

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "v3/hash.h"
#include "v3/intent_map.h"

static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        g_checks++;                                                                                \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                        \
            g_failed++;                                                                            \
        }                                                                                          \
    } while (0)

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------

static hash_t mk_hash(const char* s) {
    return hash_bytes(s, strlen(s));
}

static void tmp_path(char out[256], const char* name) {
    snprintf(out, 256, "/tmp/im_test_%d_%s.log", (int)getpid(), name);
}

static void cleanup_path(const char* path) {
    unlink(path);
}

// ----------------------------------------------------------------------------
// Tests
// ----------------------------------------------------------------------------

static void test_basic_put_get_miss(void) {
    char path[256];
    tmp_path(path, "basic");
    cleanup_path(path);

    intent_map_t* m = intent_map_open(path);
    CHECK(m != NULL);

    hash_t id = mk_hash("step-id-1");
    hash_t ch = mk_hash("content-1");
    hash_t out;

    // Miss before any put.
    CHECK(intent_map_get(m, &id, &out) == -1);

    // Put + hit.
    CHECK(intent_map_put(m, &id, &ch) == 0);
    CHECK(intent_map_get(m, &id, &out) == 0);
    CHECK(hash_eq(&out, &ch));

    intent_map_close(m);
    cleanup_path(path);
}

static void test_persistence_across_reopen(void) {
    char path[256];
    tmp_path(path, "persist");
    cleanup_path(path);

    hash_t id1 = mk_hash("id1");
    hash_t ch1 = mk_hash("content1");
    hash_t id2 = mk_hash("id2");
    hash_t ch2 = mk_hash("content2");

    intent_map_t* m = intent_map_open(path);
    CHECK(m != NULL);
    CHECK(intent_map_put(m, &id1, &ch1) == 0);
    CHECK(intent_map_put(m, &id2, &ch2) == 0);
    intent_map_close(m);

    // Reopen; both entries should be present.
    m = intent_map_open(path);
    CHECK(m != NULL);
    hash_t out;
    CHECK(intent_map_get(m, &id1, &out) == 0 && hash_eq(&out, &ch1));
    CHECK(intent_map_get(m, &id2, &out) == 0 && hash_eq(&out, &ch2));

    intent_map_close(m);
    cleanup_path(path);
}

static void test_latest_supersedes(void) {
    // Two puts for the same identity — the second wins, both in-memory and
    // after a reopen (startup scan applies records in order).
    char path[256];
    tmp_path(path, "supersede");
    cleanup_path(path);

    hash_t id = mk_hash("id");
    hash_t old = mk_hash("old");
    hash_t fresh = mk_hash("fresh");

    intent_map_t* m = intent_map_open(path);
    CHECK(intent_map_put(m, &id, &old) == 0);
    CHECK(intent_map_put(m, &id, &fresh) == 0);
    hash_t out;
    CHECK(intent_map_get(m, &id, &out) == 0 && hash_eq(&out, &fresh));
    intent_map_close(m);

    m = intent_map_open(path);
    CHECK(intent_map_get(m, &id, &out) == 0 && hash_eq(&out, &fresh));
    intent_map_close(m);
    cleanup_path(path);
}

static void test_tombstone_invalidates(void) {
    char path[256];
    tmp_path(path, "tomb");
    cleanup_path(path);

    hash_t id = mk_hash("idt");
    hash_t ch = mk_hash("cht");

    intent_map_t* m = intent_map_open(path);
    CHECK(intent_map_put(m, &id, &ch) == 0);
    hash_t out;
    CHECK(intent_map_get(m, &id, &out) == 0);
    CHECK(intent_map_invalidate(m, &id) == 0);
    CHECK(intent_map_get(m, &id, &out) == -1);

    // A subsequent put resurrects the entry.
    hash_t ch2 = mk_hash("cht2");
    CHECK(intent_map_put(m, &id, &ch2) == 0);
    CHECK(intent_map_get(m, &id, &out) == 0 && hash_eq(&out, &ch2));

    // And the tombstone/revival survives a reopen.
    intent_map_close(m);
    m = intent_map_open(path);
    CHECK(intent_map_get(m, &id, &out) == 0 && hash_eq(&out, &ch2));
    intent_map_close(m);
    cleanup_path(path);
}

static void test_partial_tail_recovery(void) {
    // Simulate a crash mid-append: append a garbage suffix of <65 bytes.
    // Startup must detect and truncate it, keeping all earlier full records.
    char path[256];
    tmp_path(path, "partial");
    cleanup_path(path);

    intent_map_t* m = intent_map_open(path);
    hash_t id = mk_hash("persisted");
    hash_t ch = mk_hash("value");
    CHECK(intent_map_put(m, &id, &ch) == 0);
    intent_map_close(m);

    // Append 37 junk bytes (less than one record).
    int fd = open(path, O_WRONLY | O_APPEND);
    CHECK(fd >= 0);
    uint8_t junk[37] = {0};
    CHECK(write(fd, junk, sizeof(junk)) == (ssize_t)sizeof(junk));
    close(fd);

    // Reopen — startup scan discards the partial tail.
    m = intent_map_open(path);
    CHECK(m != NULL);
    hash_t out;
    CHECK(intent_map_get(m, &id, &out) == 0 && hash_eq(&out, &ch));

    // And the file is actually truncated (subsequent clean puts must land on
    // a record boundary).
    intent_map_close(m);
    struct stat st;
    CHECK(stat(path, &st) == 0);
    CHECK(st.st_size % 65 == 0);
    cleanup_path(path);
}

static void test_compaction_shrinks_log(void) {
    // Write the same identity N times; then compact. The log should have
    // exactly one record; a reopen must still return the latest value.
    char path[256];
    tmp_path(path, "compact");
    cleanup_path(path);

    intent_map_t* m = intent_map_open(path);
    hash_t id = mk_hash("repeated");
    char buf[32];
    hash_t latest;
    for (int i = 0; i < 50; i++) {
        snprintf(buf, sizeof(buf), "value-%d", i);
        latest = hash_bytes(buf, strlen(buf));
        CHECK(intent_map_put(m, &id, &latest) == 0);
    }
    intent_map_stats_t stats_before;
    intent_map_get_stats(m, &stats_before);
    CHECK(stats_before.log_records == 50);
    CHECK(stats_before.live_entries == 1);

    CHECK(intent_map_compact(m) == 0);

    intent_map_stats_t stats_after;
    intent_map_get_stats(m, &stats_after);
    CHECK(stats_after.log_bytes == 65);

    // Add one more after compaction; it must persist cleanly.
    hash_t post = mk_hash("post-compact");
    CHECK(intent_map_put(m, &id, &post) == 0);
    intent_map_close(m);

    m = intent_map_open(path);
    hash_t out;
    CHECK(intent_map_get(m, &id, &out) == 0 && hash_eq(&out, &post));
    intent_map_close(m);
    cleanup_path(path);
}

static void test_compaction_drops_tombstones(void) {
    // Tombstones in the log shouldn't survive compaction.
    char path[256];
    tmp_path(path, "compact_tomb");
    cleanup_path(path);

    intent_map_t* m = intent_map_open(path);
    hash_t id_a = mk_hash("alive");
    hash_t id_d = mk_hash("deleted");
    hash_t v_a = mk_hash("val-a");
    hash_t v_d = mk_hash("val-d");
    CHECK(intent_map_put(m, &id_a, &v_a) == 0);
    CHECK(intent_map_put(m, &id_d, &v_d) == 0);
    CHECK(intent_map_invalidate(m, &id_d) == 0);
    CHECK(intent_map_compact(m) == 0);
    intent_map_close(m);

    m = intent_map_open(path);
    hash_t out;
    CHECK(intent_map_get(m, &id_a, &out) == 0 && hash_eq(&out, &v_a));
    CHECK(intent_map_get(m, &id_d, &out) == -1);
    intent_map_stats_t stats;
    intent_map_get_stats(m, &stats);
    CHECK(stats.live_entries == 1);
    CHECK(stats.log_bytes == 65);
    intent_map_close(m);
    cleanup_path(path);
}

static void test_empty_log_open(void) {
    char path[256];
    tmp_path(path, "empty");
    cleanup_path(path);

    intent_map_t* m = intent_map_open(path);
    CHECK(m != NULL);
    hash_t id = mk_hash("anything");
    hash_t out;
    CHECK(intent_map_get(m, &id, &out) == -1);
    intent_map_stats_t stats;
    intent_map_get_stats(m, &stats);
    CHECK(stats.live_entries == 0);
    CHECK(stats.log_bytes == 0);
    intent_map_close(m);
    cleanup_path(path);
}

static void test_growth_under_many_entries(void) {
    // Force hashmap growth through several capacity doublings and verify
    // everything round-trips.
    char path[256];
    tmp_path(path, "grow");
    cleanup_path(path);

    const int N = 500;
    hash_t* ids = calloc(N, sizeof(hash_t));
    hash_t* chs = calloc(N, sizeof(hash_t));
    CHECK(ids && chs);
    char buf[32];

    intent_map_t* m = intent_map_open(path);
    for (int i = 0; i < N; i++) {
        snprintf(buf, sizeof(buf), "id-%d", i);
        ids[i] = hash_bytes(buf, strlen(buf));
        snprintf(buf, sizeof(buf), "ch-%d", i);
        chs[i] = hash_bytes(buf, strlen(buf));
        CHECK(intent_map_put(m, &ids[i], &chs[i]) == 0);
    }
    for (int i = 0; i < N; i++) {
        hash_t out;
        CHECK(intent_map_get(m, &ids[i], &out) == 0 && hash_eq(&out, &chs[i]));
    }
    intent_map_close(m);

    // Reopen — everything still there after startup scan.
    m = intent_map_open(path);
    for (int i = 0; i < N; i++) {
        hash_t out;
        CHECK(intent_map_get(m, &ids[i], &out) == 0 && hash_eq(&out, &chs[i]));
    }
    intent_map_close(m);

    free(ids);
    free(chs);
    cleanup_path(path);
}

// ----------------------------------------------------------------------------
// Concurrency
// ----------------------------------------------------------------------------

typedef struct concurrent_ctx {
    intent_map_t* m;
    int thread_id;
    int n_puts;
    int* errors;
} concurrent_ctx_t;

static void* concurrent_put_thread(void* arg) {
    concurrent_ctx_t* c = arg;
    char buf[32];
    for (int i = 0; i < c->n_puts; i++) {
        snprintf(buf, sizeof(buf), "t%d-id-%d", c->thread_id, i);
        hash_t id = hash_bytes(buf, strlen(buf));
        snprintf(buf, sizeof(buf), "t%d-ch-%d", c->thread_id, i);
        hash_t ch = hash_bytes(buf, strlen(buf));
        if (intent_map_put(c->m, &id, &ch) != 0) {
            (*c->errors)++;
        }
    }
    return NULL;
}

static void test_concurrent_puts_all_land(void) {
    char path[256];
    tmp_path(path, "concurrent");
    cleanup_path(path);

    intent_map_t* m = intent_map_open(path);
    CHECK(m != NULL);

    const int T = 8;
    const int N = 100;
    pthread_t threads[T];
    concurrent_ctx_t ctxs[T];
    int errors = 0;
    for (int i = 0; i < T; i++) {
        ctxs[i].m = m;
        ctxs[i].thread_id = i;
        ctxs[i].n_puts = N;
        ctxs[i].errors = &errors;
        CHECK(pthread_create(&threads[i], NULL, concurrent_put_thread, &ctxs[i]) == 0);
    }
    for (int i = 0; i < T; i++) {
        pthread_join(threads[i], NULL);
    }
    CHECK(errors == 0);

    intent_map_stats_t stats;
    intent_map_get_stats(m, &stats);
    CHECK(stats.live_entries == (uint64_t)(T * N));

    // Every identity must round-trip.
    char buf[32];
    for (int t = 0; t < T; t++) {
        for (int i = 0; i < N; i++) {
            snprintf(buf, sizeof(buf), "t%d-id-%d", t, i);
            hash_t id = hash_bytes(buf, strlen(buf));
            snprintf(buf, sizeof(buf), "t%d-ch-%d", t, i);
            hash_t want = hash_bytes(buf, strlen(buf));
            hash_t got;
            CHECK(intent_map_get(m, &id, &got) == 0 && hash_eq(&got, &want));
        }
    }
    intent_map_close(m);
    cleanup_path(path);
}

static void test_record_size_is_exactly_65(void) {
    char path[256];
    tmp_path(path, "sizing");
    cleanup_path(path);

    intent_map_t* m = intent_map_open(path);
    hash_t id = mk_hash("one");
    hash_t ch = mk_hash("onec");
    CHECK(intent_map_put(m, &id, &ch) == 0);
    CHECK(intent_map_invalidate(m, &id) == 0);
    intent_map_close(m);

    struct stat st;
    CHECK(stat(path, &st) == 0);
    CHECK(st.st_size == 130); // two records × 65 bytes
    cleanup_path(path);
}

int main(void) {
    test_basic_put_get_miss();
    test_persistence_across_reopen();
    test_latest_supersedes();
    test_tombstone_invalidates();
    test_partial_tail_recovery();
    test_compaction_shrinks_log();
    test_compaction_drops_tombstones();
    test_empty_log_open();
    test_growth_under_many_entries();
    test_concurrent_puts_all_land();
    test_record_size_is_exactly_65();

    printf("%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}
