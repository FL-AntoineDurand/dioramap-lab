// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>

#include "v3/hash.h"
#include "v3/store.h"

// LSan suppression hook — declared only under ASan/LSan builds, no-op otherwise.
// Used to bracket the deliberately-abandoned-store section in
// test_writethrough_survives_uncloesd_close, which mimics SIGKILL and so
// genuinely leaks its handle. Without these brackets, LSan flags ~1 KB of
// expected leaks and fails ctest under DIORAMAP_ASAN=ON.
#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
extern void __lsan_disable(void);
extern void __lsan_enable(void);
#else
static inline void __lsan_disable(void) {}
static inline void __lsan_enable(void) {}
#endif

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
// Per-test scratch dir (mkdtemp).
// ----------------------------------------------------------------------------

static void rm_rf(const char* path) {
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    (void)!system(cmd);
}

static void make_tmpdir(char* out, size_t out_sz) {
    snprintf(out, out_sz, "/tmp/dioramap_store_test_XXXXXX");
    if (mkdtemp(out) == NULL) {
        fprintf(stderr, "mkdtemp failed\n");
        exit(1);
    }
}

// Put a heap copy of `data` into the store, taking the ownership-transfer
// signature seriously. Returns the content hash.
static hash_t put_bytes(store_t* s, const void* data, size_t len) {
    hash_t h = hash_bytes(data, len);
    uint8_t* buf = NULL;
    if (len > 0) {
        buf = malloc(len);
        memcpy(buf, data, len);
    }
    int rc = store_put(s, &h, buf, len);
    (void)rc;
    return h;
}

static hash_t put_str(store_t* s, const char* str) {
    return put_bytes(s, str, strlen(str));
}

// ----------------------------------------------------------------------------
// Warm-tier-only tests (hot_budget_bytes == 0: backward-compat path).
// ----------------------------------------------------------------------------

static void test_put_and_lookup_warm_only(void) {
    char root[256];
    make_tmpdir(root, sizeof(root));

    store_config_t cfg = {.root_dir = root, .budget_bytes = 0, .janitor_interval_sec = 0,
                          .hot_budget_bytes = 0};
    store_t* s = store_open(&cfg);
    CHECK(s != NULL);

    const char* payload = "hello, store";
    size_t len = strlen(payload);
    hash_t h = put_str(s, payload);

    store_handle_t handle;
    CHECK(store_lookup(s, &h, &handle) == 0);
    CHECK(handle.len == len);
    CHECK(memcmp(handle.bytes, payload, len) == 0);
    // Warm handle: mmap'd file behind the bytes.
    CHECK(handle._map != NULL);
    CHECK(handle._fd >= 0);
    store_release(s, &handle);

    store_stats_t stats;
    store_get_stats(s, &stats);
    CHECK(stats.entries == 1);
    CHECK(stats.hot_entries == 0);
    CHECK(stats.total_bytes == len);
    CHECK(stats.writes == 1);
    CHECK(stats.hits == 1);

    store_close(s);
    rm_rf(root);
}

static void test_miss(void) {
    char root[256];
    make_tmpdir(root, sizeof(root));

    store_config_t cfg = {.root_dir = root};
    store_t* s = store_open(&cfg);

    hash_t bogus = {{0xde, 0xad, 0xbe, 0xef}};
    store_handle_t handle = {0};
    CHECK(store_lookup(s, &bogus, &handle) == -1);

    store_stats_t stats;
    store_get_stats(s, &stats);
    CHECK(stats.misses == 1);
    CHECK(stats.hits == 0);

    store_close(s);
    rm_rf(root);
}

static void test_put_duplicate_is_noop(void) {
    char root[256];
    make_tmpdir(root, sizeof(root));

    store_config_t cfg = {.root_dir = root};
    store_t* s = store_open(&cfg);

    const char* payload = "dedup me";
    hash_t h1 = put_str(s, payload);
    hash_t h2 = put_str(s, payload);
    CHECK(hash_eq(&h1, &h2));

    store_stats_t stats;
    store_get_stats(s, &stats);
    CHECK(stats.entries == 1);

    store_close(s);
    rm_rf(root);
}

static void test_persistence_across_reopen(void) {
    char root[256];
    make_tmpdir(root, sizeof(root));

    store_config_t cfg = {.root_dir = root, .hot_budget_bytes = 0};

    store_t* s = store_open(&cfg);
    const char* payload = "persist me";
    hash_t h = put_str(s, payload);
    store_close(s);

    s = store_open(&cfg);
    store_stats_t stats;
    store_get_stats(s, &stats);
    CHECK(stats.entries == 1);
    CHECK(stats.total_bytes == strlen(payload));

    store_handle_t handle;
    CHECK(store_lookup(s, &h, &handle) == 0);
    CHECK(handle.len == strlen(payload));
    CHECK(memcmp(handle.bytes, payload, strlen(payload)) == 0);
    store_release(s, &handle);

    store_close(s);
    rm_rf(root);
}

static void test_eviction_under_budget(void) {
    char root[256];
    make_tmpdir(root, sizeof(root));

    // Warm-only. Budget fits ~2 entries.
    store_config_t cfg = {.root_dir = root, .budget_bytes = 250, .hot_budget_bytes = 0};
    store_t* s = store_open(&cfg);

    char payload[100];
    hash_t hashes[5];
    for (int i = 0; i < 5; i++) {
        memset(payload, 'a' + i, sizeof(payload));
        hashes[i] = put_bytes(s, payload, sizeof(payload));
    }

    store_stats_t stats;
    store_get_stats(s, &stats);
    CHECK(stats.entries == 5);
    CHECK(stats.total_bytes == 500);

    size_t evicted = store_evict_to(s, 250);
    CHECK(evicted >= 2);

    store_get_stats(s, &stats);
    CHECK(stats.total_bytes <= 250);

    store_handle_t handle;
    CHECK(store_lookup(s, &hashes[4], &handle) == 0); // newest survives
    store_release(s, &handle);
    CHECK(store_lookup(s, &hashes[0], &handle) == -1); // oldest evicted

    store_close(s);
    rm_rf(root);
}

static void test_pinned_entry_not_evicted(void) {
    char root[256];
    make_tmpdir(root, sizeof(root));

    store_config_t cfg = {.root_dir = root, .budget_bytes = 100, .hot_budget_bytes = 0};
    store_t* s = store_open(&cfg);

    char payload[100];
    hash_t hashes[3];
    for (int i = 0; i < 3; i++) {
        memset(payload, 'a' + i, sizeof(payload));
        hashes[i] = put_bytes(s, payload, sizeof(payload));
    }
    store_pin(s, &hashes[0]);
    store_evict_to(s, 0);

    store_handle_t handle;
    CHECK(store_lookup(s, &hashes[0], &handle) == 0);
    store_release(s, &handle);
    CHECK(store_lookup(s, &hashes[1], &handle) == -1);

    store_unpin(s, &hashes[0]);
    store_evict_to(s, 0);
    CHECK(store_lookup(s, &hashes[0], &handle) == -1);

    store_close(s);
    rm_rf(root);
}

static void test_read_pin_blocks_eviction(void) {
    char root[256];
    make_tmpdir(root, sizeof(root));
    store_config_t cfg = {.root_dir = root, .hot_budget_bytes = 0};
    store_t* s = store_open(&cfg);

    const char* payload = "reading...";
    hash_t h = put_str(s, payload);

    store_handle_t handle;
    CHECK(store_lookup(s, &h, &handle) == 0);
    store_evict_to(s, 0);
    store_stats_t stats;
    store_get_stats(s, &stats);
    CHECK(stats.entries == 1);

    store_release(s, &handle);
    store_evict_to(s, 0);
    store_get_stats(s, &stats);
    CHECK(stats.entries == 0);

    store_close(s);
    rm_rf(root);
}

static void test_many_writes(void) {
    char root[256];
    make_tmpdir(root, sizeof(root));
    store_config_t cfg = {.root_dir = root, .hot_budget_bytes = 0};
    store_t* s = store_open(&cfg);

    hash_t hashes[200];
    char buf[64];
    for (int i = 0; i < 200; i++) {
        snprintf(buf, sizeof(buf), "payload-%d", i);
        hashes[i] = put_str(s, buf);
    }
    store_stats_t stats;
    store_get_stats(s, &stats);
    CHECK(stats.entries == 200);

    store_handle_t handle;
    CHECK(store_lookup(s, &hashes[0], &handle) == 0);
    store_release(s, &handle);
    CHECK(store_lookup(s, &hashes[100], &handle) == 0);
    store_release(s, &handle);
    CHECK(store_lookup(s, &hashes[199], &handle) == 0);
    store_release(s, &handle);

    store_close(s);
    rm_rf(root);
}

static void test_scan_skips_malformed_files(void) {
    char root[256];
    make_tmpdir(root, sizeof(root));

    char bad_dir[512];
    snprintf(bad_dir, sizeof(bad_dir), "%s/ab/cd", root);
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", bad_dir);
    (void)!system(cmd);
    char bad_file[1024];
    snprintf(bad_file, sizeof(bad_file), "%s/not-a-hash.txt", bad_dir);
    FILE* f = fopen(bad_file, "w");
    fprintf(f, "garbage");
    fclose(f);
    char good_file[1024];
    snprintf(good_file, sizeof(good_file),
             "%s/"
             "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789.bin",
             bad_dir);
    f = fopen(good_file, "w");
    fprintf(f, "hello");
    fclose(f);

    store_config_t cfg = {.root_dir = root};
    store_t* s = store_open(&cfg);
    CHECK(s != NULL);
    store_stats_t stats;
    store_get_stats(s, &stats);
    CHECK(stats.entries == 1);
    store_close(s);
    rm_rf(root);
}

// ----------------------------------------------------------------------------
// Hot-tier tests (hot_budget_bytes > 0)
// ----------------------------------------------------------------------------

static void test_hot_tier_put_read_zero_copy(void) {
    char root[256];
    make_tmpdir(root, sizeof(root));
    store_config_t cfg = {.root_dir = root, .hot_budget_bytes = 1024 * 1024};
    store_t* s = store_open(&cfg);

    const char* payload = "hot-resident-bytes";
    size_t len = strlen(payload);
    hash_t h = put_str(s, payload);

    store_stats_t stats;
    store_get_stats(s, &stats);
    CHECK(stats.entries == 1);
    CHECK(stats.hot_entries == 1);
    CHECK(stats.hot_bytes == len);
    // Write-through (HASH-MODEL §11): every put lands on disk regardless
    // of tier. total_bytes counts the on-disk footprint — which equals
    // the value's length even when it's also resident in hot RAM.
    CHECK(stats.total_bytes == len);

    store_handle_t handle;
    CHECK(store_lookup(s, &h, &handle) == 0);
    CHECK(handle.len == len);
    CHECK(memcmp(handle.bytes, payload, len) == 0);
    // Hot handle: no mmap, no fd.
    CHECK(handle._map == NULL);
    CHECK(handle._fd == -1);
    store_release(s, &handle);

    store_close(s);
    rm_rf(root);
}

static void test_hot_tier_demotes_on_budget_pressure(void) {
    char root[256];
    make_tmpdir(root, sizeof(root));
    // Hot budget fits ~2 entries of 100 bytes each.
    store_config_t cfg = {.root_dir = root, .hot_budget_bytes = 250};
    store_t* s = store_open(&cfg);

    char payload[100];
    hash_t hashes[5];
    for (int i = 0; i < 5; i++) {
        memset(payload, 'a' + i, sizeof(payload));
        hashes[i] = put_bytes(s, payload, sizeof(payload));
    }

    store_stats_t stats;
    store_get_stats(s, &stats);
    CHECK(stats.entries == 5);
    // At least some entries had to demote to make room.
    CHECK(stats.demotions >= 1);
    CHECK(stats.hot_bytes <= 250);
    // Write-through: ALL 5 entries are on disk; total_bytes is 5×100. Hot
    // is the on-disk-AND-RAM subset; warm is on-disk-only. No entry is
    // ever RAM-only post write-through, so hot_entries + warm_entries == 5
    // (rather than the old "hot + total/100 == 5" which assumed hot meant
    // RAM-only).
    CHECK(stats.total_bytes == 5 * 100);
    CHECK(stats.hot_entries >= 1 && stats.hot_entries <= 5);
    // Newest must still be in hot tier (or at least retrievable).
    store_handle_t handle;
    CHECK(store_lookup(s, &hashes[4], &handle) == 0);
    store_release(s, &handle);
    // Oldest must still be retrievable (demoted to warm, not evicted).
    CHECK(store_lookup(s, &hashes[0], &handle) == 0);
    CHECK(handle.bytes[0] == 'a');
    store_release(s, &handle);

    store_close(s);
    rm_rf(root);
}

static void test_hot_tier_entry_larger_than_budget_goes_warm(void) {
    char root[256];
    make_tmpdir(root, sizeof(root));
    store_config_t cfg = {.root_dir = root, .hot_budget_bytes = 64};
    store_t* s = store_open(&cfg);

    // Entry is larger than hot budget — must go straight to warm.
    char big[1024];
    memset(big, 'X', sizeof(big));
    hash_t h = put_bytes(s, big, sizeof(big));

    store_stats_t stats;
    store_get_stats(s, &stats);
    CHECK(stats.hot_entries == 0);
    CHECK(stats.total_bytes == sizeof(big));

    store_handle_t handle;
    CHECK(store_lookup(s, &h, &handle) == 0);
    CHECK(handle._map != NULL); // warm: mmap'd
    CHECK(handle.len == sizeof(big));
    store_release(s, &handle);

    store_close(s);
    rm_rf(root);
}

static void test_hot_tier_survives_close(void) {
    // On close, hot entries must demote to disk so the cache survives restart.
    char root[256];
    make_tmpdir(root, sizeof(root));
    store_config_t cfg = {.root_dir = root, .hot_budget_bytes = 1024 * 1024};
    store_t* s = store_open(&cfg);
    hash_t h = put_str(s, "will-survive-restart");
    store_stats_t stats;
    store_get_stats(s, &stats);
    CHECK(stats.hot_entries == 1);
    store_close(s);

    // Reopen — hot entries should have been demoted.
    s = store_open(&cfg);
    store_get_stats(s, &stats);
    CHECK(stats.entries == 1);
    CHECK(stats.hot_entries == 0);
    CHECK(stats.total_bytes == strlen("will-survive-restart"));
    store_handle_t handle;
    CHECK(store_lookup(s, &h, &handle) == 0);
    CHECK(memcmp(handle.bytes, "will-survive-restart", strlen("will-survive-restart")) == 0);
    store_release(s, &handle);
    store_close(s);
    rm_rf(root);
}

static void test_hot_tier_idempotent_put(void) {
    // Put the same key twice: second call must drop the caller's buffer.
    char root[256];
    make_tmpdir(root, sizeof(root));
    store_config_t cfg = {.root_dir = root, .hot_budget_bytes = 1024 * 1024};
    store_t* s = store_open(&cfg);

    hash_t h1 = put_str(s, "dedup-payload");
    hash_t h2 = put_str(s, "dedup-payload");
    CHECK(hash_eq(&h1, &h2));
    store_stats_t stats;
    store_get_stats(s, &stats);
    CHECK(stats.entries == 1);
    CHECK(stats.hot_entries == 1);

    store_close(s);
    rm_rf(root);
}

static void test_hot_tier_dedup_skips_warm_write(void) {
    // An existing hot entry dedupes even a larger-than-hot put.
    char root[256];
    make_tmpdir(root, sizeof(root));
    store_config_t cfg = {.root_dir = root, .hot_budget_bytes = 1024 * 1024};
    store_t* s = store_open(&cfg);

    const char* payload = "shared-content-x";
    hash_t h1 = put_str(s, payload);
    hash_t h2 = put_str(s, payload);
    CHECK(hash_eq(&h1, &h2));
    store_stats_t stats;
    store_get_stats(s, &stats);
    CHECK(stats.hot_entries == 1);
    // Write-through: dedup'd entry has its bytes on disk from the first put.
    CHECK(stats.total_bytes == strlen(payload));

    store_close(s);
    rm_rf(root);
}

// store_forget: the F2 desync-recovery helper. Verifies that:
//   - forget on a missing hash is a no-op (returns 0).
//   - forget on a present unpinned entry: removes from index, unlinks
//     the on-disk file, decrements totals.
//   - forget on a pinned entry refuses (returns -1, leaves intact).
static void test_store_forget_purges_in_memory_and_on_disk(void) {
    char root[256];
    make_tmpdir(root, sizeof(root));
    store_config_t cfg = {.root_dir = root, .hot_budget_bytes = 1024 * 1024};
    store_t* s = store_open(&cfg);

    const char* payload = "forget-target-bytes";
    hash_t h = put_str(s, payload);

    // File exists on disk under write-through.
    char hex[65];
    hash_format_hex(&h, hex);
    char fpath[1024];
    snprintf(fpath, sizeof(fpath), "%s/%c%c/%c%c/%s.bin",
             root, hex[0], hex[1], hex[2], hex[3], hex);
    struct stat st;
    CHECK(stat(fpath, &st) == 0);

    // Idempotent: forgetting an unknown hash is a no-op.
    hash_t bogus = hash_bytes("nope", 4);
    CHECK(store_forget(s, &bogus) == 0);

    // Forget the real entry. File should disappear, lookup should miss.
    CHECK(store_forget(s, &h) == 0);
    CHECK(stat(fpath, &st) != 0); // unlinked
    CHECK(!store_contains(s, &h));
    store_handle_t handle;
    CHECK(store_lookup(s, &h, &handle) == -1);

    // Re-put the same payload — should produce a fresh on-disk file.
    hash_t h2 = put_str(s, payload);
    CHECK(hash_eq(&h, &h2));
    CHECK(stat(fpath, &st) == 0);

    // Pin it, then forget should refuse.
    CHECK(store_lookup(s, &h2, &handle) == 0);
    CHECK(store_forget(s, &h2) == -1); // pinned
    CHECK(store_contains(s, &h2));     // still there
    store_release(s, &handle);

    // Once unpinned, forget succeeds.
    CHECK(store_forget(s, &h2) == 0);
    CHECK(!store_contains(s, &h2));

    store_close(s);
    rm_rf(root);
}

// Write-through (HASH-MODEL §11) crash-safety guarantee. Simulates a
// SIGKILL'd process: store_t handle abandoned without store_close, so
// drain_hot_tier_locked never runs. With write-through every put has
// already landed on disk, so a fresh process opening the same root
// recovers every entry. Pre write-through this test would lose every
// hot-tier-only entry.
static void test_writethrough_survives_uncloesd_close(void) {
    char root[256];
    make_tmpdir(root, sizeof(root));

    // ── Process #1: put 3 hot entries, abandon the handle without
    //               calling store_close. Mimics SIGKILL.
    // The deliberately-abandoned store + its inner allocations are real
    // leaks; bracket this section so LSan doesn't fail the test under ASan.
    __lsan_disable();
    store_config_t cfg1 = {.root_dir = root, .hot_budget_bytes = 1024 * 1024};
    store_t* s1 = store_open(&cfg1);
    const char* payloads[3] = {"alpha-bytes-0001", "beta-bytes-00002", "gamma-bytes-0003"};
    hash_t hs[3];
    size_t total_payload = 0;
    for (int i = 0; i < 3; i++) {
        hs[i] = put_str(s1, payloads[i]);
        total_payload += strlen(payloads[i]);
    }
    // Confirm all three are hot (RAM) AND warm (disk) under write-through.
    store_stats_t stats;
    store_get_stats(s1, &stats);
    CHECK(stats.entries == 3);
    CHECK(stats.hot_entries == 3);
    CHECK(stats.total_bytes == total_payload);
    // Critically: NOT calling store_close(s1). Just leak the handle —
    // drains, fsync(), shutdown bookkeeping all skipped.
    // (Janitor thread will get cleaned up at process exit; that's fine
    // for a unit test running in a process about to call further tests.)
    // Don't free(s1) either — that would still run shutdown logic via
    // any destructor; instead just lose the pointer:
    s1 = NULL;
    __lsan_enable();

    // ── Process #2: open the same root fresh. With write-through the
    //               disk has every .bin from the abandoned session.
    store_config_t cfg2 = {.root_dir = root, .hot_budget_bytes = 1024 * 1024};
    store_t* s2 = store_open(&cfg2);
    CHECK(s2 != NULL);
    store_get_stats(s2, &stats);
    CHECK(stats.entries == 3);

    for (int i = 0; i < 3; i++) {
        store_handle_t handle;
        CHECK(store_lookup(s2, &hs[i], &handle) == 0);
        CHECK(handle.len == strlen(payloads[i]));
        CHECK(memcmp(handle.bytes, payloads[i], handle.len) == 0);
        store_release(s2, &handle);
    }

    store_close(s2);
    rm_rf(root);
}

static void test_pinned_hot_entry_not_demoted(void) {
    // With 3 tiny entries filling hot tier and the oldest pinned, the next put
    // should still make room by demoting a non-pinned peer — the pinned entry
    // stays hot.
    char root[256];
    make_tmpdir(root, sizeof(root));
    store_config_t cfg = {.root_dir = root, .hot_budget_bytes = 200};
    store_t* s = store_open(&cfg);

    char payload[100];
    memset(payload, 'X', sizeof(payload));
    hash_t h0 = put_bytes(s, payload, sizeof(payload));
    memset(payload, 'Y', sizeof(payload));
    hash_t h1 = put_bytes(s, payload, sizeof(payload));
    (void)h1;
    store_pin(s, &h0);

    memset(payload, 'Z', sizeof(payload));
    hash_t h2 = put_bytes(s, payload, sizeof(payload));
    (void)h2;

    store_handle_t handle;
    CHECK(store_lookup(s, &h0, &handle) == 0);
    CHECK(handle._map == NULL); // still hot
    store_release(s, &handle);

    store_unpin(s, &h0);
    store_close(s);
    rm_rf(root);
}

// ----------------------------------------------------------------------------
// Concurrency: many threads writing and reading at the same time.
// ----------------------------------------------------------------------------

typedef struct thread_ctx {
    store_t* s;
    int thread_id;
    int n_ops;
    int* errors;
} thread_ctx_t;

static void* concurrent_rw_thread(void* arg) {
    thread_ctx_t* c = arg;
    char buf[64];
    for (int i = 0; i < c->n_ops; i++) {
        snprintf(buf, sizeof(buf), "t%d-payload-%d", c->thread_id, i);
        size_t len = strlen(buf);
        hash_t h = put_str(c->s, buf);
        store_handle_t handle;
        if (store_lookup(c->s, &h, &handle) != 0) {
            __atomic_fetch_add(c->errors, 1, __ATOMIC_RELAXED);
            continue;
        }
        if (handle.len != len || memcmp(handle.bytes, buf, len) != 0) {
            __atomic_fetch_add(c->errors, 1, __ATOMIC_RELAXED);
        }
        store_release(c->s, &handle);
    }
    return NULL;
}

static void test_concurrent_rw(void) {
    char root[256];
    make_tmpdir(root, sizeof(root));
    // Small hot budget forces frequent demotions under concurrent writers.
    store_config_t cfg = {.root_dir = root, .hot_budget_bytes = 1024};
    store_t* s = store_open(&cfg);

    const int T = 8;
    const int N = 50;
    pthread_t threads[T];
    thread_ctx_t ctxs[T];
    int errors = 0;
    for (int i = 0; i < T; i++) {
        ctxs[i].s = s;
        ctxs[i].thread_id = i;
        ctxs[i].n_ops = N;
        ctxs[i].errors = &errors;
        CHECK(pthread_create(&threads[i], NULL, concurrent_rw_thread, &ctxs[i]) == 0);
    }
    for (int i = 0; i < T; i++) {
        pthread_join(threads[i], NULL);
    }
    CHECK(errors == 0);
    store_stats_t stats;
    store_get_stats(s, &stats);
    CHECK(stats.entries == (uint64_t)(T * N));

    store_close(s);
    rm_rf(root);
}

// ----------------------------------------------------------------------------

int main(void) {
    test_put_and_lookup_warm_only();
    test_miss();
    test_put_duplicate_is_noop();
    test_persistence_across_reopen();
    test_eviction_under_budget();
    test_pinned_entry_not_evicted();
    test_read_pin_blocks_eviction();
    test_many_writes();
    test_scan_skips_malformed_files();
    test_hot_tier_put_read_zero_copy();
    test_hot_tier_demotes_on_budget_pressure();
    test_hot_tier_entry_larger_than_budget_goes_warm();
    test_hot_tier_survives_close();
    test_hot_tier_idempotent_put();
    test_hot_tier_dedup_skips_warm_write();
    test_writethrough_survives_uncloesd_close();
    test_store_forget_purges_in_memory_and_on_disk();
    test_pinned_hot_entry_not_demoted();
    test_concurrent_rw();

    printf("%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}
