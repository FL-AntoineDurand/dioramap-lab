// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#define _POSIX_C_SOURCE 200809L

#include "v3/store.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "v3/log.h"

// ----------------------------------------------------------------------------
// Two-tier content-addressed store (HASH-MODEL.md §5).
//
// - Hot tier: heap-resident buffer owned by the store. Zero-copy reads via a
//   direct pointer through the handle.
// - Warm tier: disk-resident at <root>/xx/yy/<full-hex>.bin, mmap'd on read.
//
// One index + one LRU list span both tiers. Each entry's `tier` field
// discriminates. Demotion (hot → warm) preserves the hash key; eviction
// (warm → unlink) removes the entry entirely. An entry can be pinned to
// block eviction / demotion; in-flight handles increment the pin count
// automatically.
// ----------------------------------------------------------------------------

typedef enum store_tier {
    TIER_HOT = 0,
    TIER_WARM = 1,
    TIER_DEMOTING = 2, // transient during demotion; lookups retry or wait
} store_tier_t;

typedef struct store_entry {
    hash_t hash;
    uint64_t size_bytes;
    uint64_t last_access_us; // monotonic microseconds
    uint64_t pin_count;      // protected by store->lock
    store_tier_t tier;
    uint8_t* hot_bytes;      // owned iff tier == TIER_HOT
    struct store_entry* lru_prev;
    struct store_entry* lru_next;
} store_entry_t;

struct store {
    char* root_dir;
    uint64_t budget_bytes;
    uint64_t hot_budget_bytes;
    double janitor_interval_sec;

    // One lock guards: index, LRU list, per-entry state, totals.
    pthread_mutex_t lock;
    pthread_cond_t demotion_done_cv; // signals when a DEMOTING entry finishes

    // Open-addressing hashmap, linear probing, power-of-two capacity.
    store_entry_t** index;
    size_t index_cap;
    size_t index_count;

    // LRU list (newest at head, oldest at tail) spanning both tiers.
    store_entry_t* lru_head;
    store_entry_t* lru_tail;

    uint64_t total_bytes; // warm footprint
    uint64_t hot_bytes_total;

    atomic_uint_least64_t hits;
    atomic_uint_least64_t misses;
    atomic_uint_least64_t writes;
    atomic_uint_least64_t evictions;
    atomic_uint_least64_t demotions;

    // Disk janitor
    pthread_t janitor_tid;
    bool janitor_running;
    pthread_mutex_t janitor_lock;
    pthread_cond_t janitor_wake;
    bool shutdown;
};

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------

static uint64_t now_monotonic_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static int mkdir_p(const char* path) {
    char buf[4096];
    size_t n = strlen(path);
    if (n >= sizeof(buf)) {
        return -1;
    }
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i <= n; i++) {
        if (buf[i] == '/' || buf[i] == '\0') {
            char c = buf[i];
            buf[i] = '\0';
            if (mkdir(buf, 0700) != 0 && errno != EEXIST) {
                return -1;
            }
            buf[i] = c;
        }
    }
    return 0;
}

#define STORE_MAX_ROOT_LEN 2048
#define STORE_PATH_BUFSZ 4096

static int hash_to_path(const char* root, const hash_t* h, char* out, size_t out_sz) {
    char hex[65];
    hash_format_hex(h, hex);
    int n = snprintf(out, out_sz, "%s/%c%c/%c%c/%s.bin", root, hex[0], hex[1], hex[2], hex[3], hex);
    if (n < 0 || (size_t)n >= out_sz) {
        return -1;
    }
    return 0;
}

static int parse_hex_char(char c, uint8_t* out) {
    if (c >= '0' && c <= '9') {
        *out = (uint8_t)(c - '0');
    } else if (c >= 'a' && c <= 'f') {
        *out = (uint8_t)(c - 'a' + 10);
    } else {
        return -1;
    }
    return 0;
}

static int hash_from_hex(const char* hex, hash_t* out) {
    for (size_t i = 0; i < 32; i++) {
        uint8_t hi, lo;
        if (parse_hex_char(hex[i * 2], &hi) != 0 || parse_hex_char(hex[i * 2 + 1], &lo) != 0) {
            return -1;
        }
        out->bytes[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

// ----------------------------------------------------------------------------
// Hashmap
// ----------------------------------------------------------------------------

static size_t hash_fingerprint(const hash_t* h) {
    uint64_t v = 0;
    memcpy(&v, h->bytes, sizeof(v));
    return (size_t)v;
}

static void index_insert_bucket(store_entry_t** index, size_t cap, store_entry_t* e) {
    size_t mask = cap - 1;
    size_t i = hash_fingerprint(&e->hash) & mask;
    while (index[i] != NULL) {
        i = (i + 1) & mask;
    }
    index[i] = e;
}

static int index_grow_if_needed(store_t* s) {
    if (s->index_count * 2 + 1 < s->index_cap) {
        return 0;
    }
    size_t new_cap = s->index_cap ? s->index_cap * 2 : 64;
    store_entry_t** new_idx = calloc(new_cap, sizeof(store_entry_t*));
    if (!new_idx) {
        return -1;
    }
    for (size_t i = 0; i < s->index_cap; i++) {
        if (s->index[i]) {
            index_insert_bucket(new_idx, new_cap, s->index[i]);
        }
    }
    free(s->index);
    s->index = new_idx;
    s->index_cap = new_cap;
    return 0;
}

static store_entry_t* index_find(store_t* s, const hash_t* h) {
    if (s->index_cap == 0) {
        return NULL;
    }
    size_t mask = s->index_cap - 1;
    size_t i = hash_fingerprint(h) & mask;
    while (s->index[i] != NULL) {
        if (hash_eq(&s->index[i]->hash, h)) {
            return s->index[i];
        }
        i = (i + 1) & mask;
    }
    return NULL;
}

static void index_delete(store_t* s, store_entry_t* e) {
    if (s->index_cap == 0) {
        return;
    }
    size_t mask = s->index_cap - 1;
    size_t i = hash_fingerprint(&e->hash) & mask;
    while (s->index[i] != NULL && s->index[i] != e) {
        i = (i + 1) & mask;
    }
    if (s->index[i] != e) {
        return;
    }
    size_t j = i;
    for (;;) {
        s->index[j] = NULL;
        size_t k = j;
        for (;;) {
            k = (k + 1) & mask;
            if (s->index[k] == NULL) {
                s->index_count--;
                return;
            }
            size_t desired = hash_fingerprint(&s->index[k]->hash) & mask;
            size_t diff_k_j = (k - j) & mask;
            size_t diff_desired_j = (desired - j) & mask;
            if (diff_desired_j > diff_k_j) {
                continue;
            }
            break;
        }
        s->index[j] = s->index[k];
        j = k;
    }
}

// ----------------------------------------------------------------------------
// LRU list (under lock)
// ----------------------------------------------------------------------------

static void lru_push_head(store_t* s, store_entry_t* e) {
    e->lru_prev = NULL;
    e->lru_next = s->lru_head;
    if (s->lru_head) {
        s->lru_head->lru_prev = e;
    } else {
        s->lru_tail = e;
    }
    s->lru_head = e;
}

static void lru_unlink(store_t* s, store_entry_t* e) {
    if (e->lru_prev) {
        e->lru_prev->lru_next = e->lru_next;
    } else {
        s->lru_head = e->lru_next;
    }
    if (e->lru_next) {
        e->lru_next->lru_prev = e->lru_prev;
    } else {
        s->lru_tail = e->lru_prev;
    }
    e->lru_prev = e->lru_next = NULL;
}

static void lru_touch(store_t* s, store_entry_t* e) {
    if (s->lru_head == e) {
        return;
    }
    lru_unlink(s, e);
    lru_push_head(s, e);
}

// ----------------------------------------------------------------------------
// Disk I/O helpers (pure bytes → file; no index work)
// ----------------------------------------------------------------------------

static int write_warm_file(const char* root, const hash_t* h, const uint8_t* bytes,
                           size_t len) {
    char hex[65];
    hash_format_hex(h, hex);

    char l1[STORE_PATH_BUFSZ];
    char l2[STORE_PATH_BUFSZ];
    char final_path[STORE_PATH_BUFSZ];
    char tmp_path[STORE_PATH_BUFSZ];
    int n;
    n = snprintf(l1, sizeof(l1), "%s/%c%c", root, hex[0], hex[1]);
    if (n < 0 || (size_t)n >= sizeof(l1)) {
        return -1;
    }
    n = snprintf(l2, sizeof(l2), "%.*s/%c%c", (int)strlen(l1), l1, hex[2], hex[3]);
    if (n < 0 || (size_t)n >= sizeof(l2)) {
        return -1;
    }
    mkdir(l1, 0700);
    mkdir(l2, 0700);
    n = snprintf(final_path, sizeof(final_path), "%.*s/%s.bin", (int)strlen(l2), l2, hex);
    if (n < 0 || (size_t)n >= sizeof(final_path)) {
        return -1;
    }
    // tmp suffix is per-invocation (pid + thread + monotonic-us) so two
    // concurrent puts of the same content hash don't share a .tmp path
    // and clobber each other's writes mid-flight. The final rename to
    // <hash>.bin is atomic; the second rename simply overwrites the
    // first with byte-identical content (content-addressed → same hash
    // means same bytes by construction).
    // DIORAMAP_TID(): pthread_self() in prod, fixed 0 under Frama-C audit.
    // glibc happens to implement pthread_t as `unsigned long` so the cast
    // below is safe in practice, but Frama-C 25's kernel can't see through
    // the typedef chain and rejects the cast. The macro keeps this code
    // unchanged in production while letting the audit parse the file.
#ifdef DIORAMAP_FRAMA_C_AUDIT
#  define DIORAMAP_TID() 0UL
#else
#  define DIORAMAP_TID() ((unsigned long)pthread_self())
#endif
    n = snprintf(tmp_path, sizeof(tmp_path), "%.*s/.%s.%d.%lx.%lx.tmp",
                 (int)strlen(l2), l2, hex,
                 (int)getpid(), DIORAMAP_TID(),
                 (unsigned long)now_monotonic_us());
#undef DIORAMAP_TID
    if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
        return -1;
    }

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        return -1;
    }
    const uint8_t* p = bytes;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t nw = write(fd, p, remaining);
        if (nw < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            unlink(tmp_path);
            return -1;
        }
        p += nw;
        remaining -= (size_t)nw;
    }
    if (fsync(fd) != 0) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }
    close(fd);
    if (rename(tmp_path, final_path) != 0) {
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

// ----------------------------------------------------------------------------
// Forward decls for eviction + demotion
// ----------------------------------------------------------------------------

static size_t do_warm_evict_locked(store_t* s, uint64_t target_bytes);
static int demote_entry_locked(store_t* s, store_entry_t* e);
static size_t demote_to_fit_locked(store_t* s, size_t need_bytes);

// ----------------------------------------------------------------------------
// Janitor thread (warm-tier only; hot demotion is on-demand from puts)
// ----------------------------------------------------------------------------

static void* janitor_loop(void* arg) {
    store_t* s = (store_t*)arg;
    for (;;) {
        pthread_mutex_lock(&s->janitor_lock);
        if (s->shutdown) {
            pthread_mutex_unlock(&s->janitor_lock);
            return NULL;
        }
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += (time_t)s->janitor_interval_sec;
        long nsec = (long)((s->janitor_interval_sec - (time_t)s->janitor_interval_sec) * 1e9);
        deadline.tv_nsec += nsec;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&s->janitor_wake, &s->janitor_lock, &deadline);
        bool should_exit = s->shutdown;
        pthread_mutex_unlock(&s->janitor_lock);
        if (should_exit) {
            return NULL;
        }

        size_t evicted = 0;
        if (s->budget_bytes > 0) {
            pthread_mutex_lock(&s->lock);
            if (s->total_bytes > s->budget_bytes) {
                evicted = do_warm_evict_locked(s, s->budget_bytes);
            }
            pthread_mutex_unlock(&s->lock);
        }
        LOG_EVENT(SUB_STORE, LOG_DEBUG, EV_STORE_JANITOR_CYCLE, LF_UINT("evicted", evicted));
    }
}

// ----------------------------------------------------------------------------
// Disk scan (startup)
// ----------------------------------------------------------------------------

typedef struct scan_item {
    hash_t hash;
    uint64_t size;
} scan_item_t;

static int scan_level2_dir(const char* path, scan_item_t** out, size_t* out_len, size_t* out_cap) {
    DIR* d = opendir(path);
    if (!d) {
        return 0;
    }
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen != 64 + 4) {
            continue;
        }
        if (memcmp(ent->d_name + 64, ".bin", 4) != 0) {
            continue;
        }
        hash_t h;
        if (hash_from_hex(ent->d_name, &h) != 0) {
            continue;
        }
        char fpath[STORE_PATH_BUFSZ];
        int pn = snprintf(fpath, sizeof(fpath), "%.*s/%.*s", STORE_MAX_ROOT_LEN + 6, path,
                          (int)nlen, ent->d_name);
        if (pn < 0 || (size_t)pn >= sizeof(fpath)) {
            continue;
        }
        struct stat st;
        if (stat(fpath, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        if (*out_len == *out_cap) {
            size_t new_cap = *out_cap ? *out_cap * 2 : 64;
            scan_item_t* np = realloc(*out, new_cap * sizeof(scan_item_t));
            if (!np) {
                closedir(d);
                return -1;
            }
            *out = np;
            *out_cap = new_cap;
        }
        (*out)[*out_len].hash = h;
        (*out)[*out_len].size = (uint64_t)st.st_size;
        (*out_len)++;
    }
    closedir(d);
    return 0;
}

static int scan_disk(const char* root, scan_item_t** out, size_t* out_len) {
    *out = NULL;
    *out_len = 0;
    size_t cap = 0;
    DIR* d = opendir(root);
    if (!d) {
        return 0;
    }
    struct dirent* a;
    while ((a = readdir(d)) != NULL) {
        if (strlen(a->d_name) != 2) {
            continue;
        }
        char l1[STORE_PATH_BUFSZ];
        int pn = snprintf(l1, sizeof(l1), "%.*s/%.2s", STORE_MAX_ROOT_LEN, root, a->d_name);
        if (pn < 0 || (size_t)pn >= sizeof(l1)) {
            continue;
        }
        DIR* d2 = opendir(l1);
        if (!d2) {
            continue;
        }
        struct dirent* b;
        while ((b = readdir(d2)) != NULL) {
            if (strlen(b->d_name) != 2) {
                continue;
            }
            char l2[STORE_PATH_BUFSZ];
            int pn2 = snprintf(l2, sizeof(l2), "%.*s/%.2s", STORE_MAX_ROOT_LEN + 3, l1, b->d_name);
            if (pn2 < 0 || (size_t)pn2 >= sizeof(l2)) {
                continue;
            }
            if (scan_level2_dir(l2, out, out_len, &cap) != 0) {
                closedir(d2);
                closedir(d);
                return -1;
            }
        }
        closedir(d2);
    }
    closedir(d);
    return 0;
}

// ----------------------------------------------------------------------------
// Open / close
// ----------------------------------------------------------------------------

store_t* store_open(const store_config_t* cfg) {
    if (!cfg || !cfg->root_dir) {
        return NULL;
    }
    if (strlen(cfg->root_dir) > STORE_MAX_ROOT_LEN) {
        return NULL;
    }
    if (mkdir_p(cfg->root_dir) != 0) {
        return NULL;
    }

    store_t* s = calloc(1, sizeof(store_t));
    if (!s) {
        return NULL;
    }
    s->root_dir = strdup(cfg->root_dir);
    if (!s->root_dir) {
        free(s);
        return NULL;
    }
    s->budget_bytes = cfg->budget_bytes;
    s->hot_budget_bytes = cfg->hot_budget_bytes;
    s->janitor_interval_sec = cfg->janitor_interval_sec;
    pthread_mutex_init(&s->lock, NULL);
    pthread_mutex_init(&s->janitor_lock, NULL);
    pthread_cond_init(&s->janitor_wake, NULL);
    pthread_cond_init(&s->demotion_done_cv, NULL);

    // Warm tier: populate index from disk. Hot tier starts empty; first put
    // after startup repopulates it.
    scan_item_t* items = NULL;
    size_t nitems = 0;
    if (scan_disk(s->root_dir, &items, &nitems) != 0) {
        store_close(s);
        return NULL;
    }
    uint64_t now = now_monotonic_us();
    for (size_t i = 0; i < nitems; i++) {
        if (index_grow_if_needed(s) != 0) {
            free(items);
            store_close(s);
            return NULL;
        }
        store_entry_t* e = calloc(1, sizeof(store_entry_t));
        if (!e) {
            free(items);
            store_close(s);
            return NULL;
        }
        e->hash = items[i].hash;
        e->size_bytes = items[i].size;
        e->last_access_us = now;
        e->pin_count = 0;
        e->tier = TIER_WARM;
        e->hot_bytes = NULL;
        index_insert_bucket(s->index, s->index_cap, e);
        s->index_count++;
        lru_push_head(s, e);
        s->total_bytes += e->size_bytes;
    }
    free(items);

    LOG_EVENT(SUB_STORE, LOG_INFO, EV_STORE_SCAN_COMPLETE, LF_STR("root", s->root_dir),
              LF_UINT("entries", s->index_count), LF_UINT("total_bytes", s->total_bytes));

    if (s->janitor_interval_sec > 0) {
        s->janitor_running = true;
        if (pthread_create(&s->janitor_tid, NULL, janitor_loop, s) != 0) {
            s->janitor_running = false;
            store_close(s);
            return NULL;
        }
    }

    LOG_EVENT(SUB_STORE, LOG_INFO, EV_STORE_OPENED, LF_STR("root", s->root_dir),
              LF_UINT("budget_bytes", s->budget_bytes),
              LF_UINT("hot_budget_bytes", s->hot_budget_bytes),
              LF_FLOAT("janitor_interval_sec", s->janitor_interval_sec));
    return s;
}

// Drain all hot entries to warm during close. Preserves the cache across
// restarts: every hot byte lands on disk before we release resources.
static void drain_hot_tier_locked(store_t* s) {
    store_entry_t* cur = s->lru_tail;
    while (cur) {
        store_entry_t* prev = cur->lru_prev;
        if (cur->tier == TIER_HOT && cur->pin_count == 0) {
            // pin_count should be zero at shutdown (all sessions done).
            demote_entry_locked(s, cur);
        }
        cur = prev;
    }
}

void store_close(store_t* s) {
    if (!s) {
        return;
    }
    if (s->janitor_running) {
        pthread_mutex_lock(&s->janitor_lock);
        s->shutdown = true;
        pthread_cond_broadcast(&s->janitor_wake);
        pthread_mutex_unlock(&s->janitor_lock);
        pthread_join(s->janitor_tid, NULL);
    }

    // Drain hot entries to disk so the cache survives restart.
    pthread_mutex_lock(&s->lock);
    drain_hot_tier_locked(s);
    pthread_mutex_unlock(&s->lock);

    LOG_EVENT(SUB_STORE, LOG_INFO, EV_STORE_CLOSED, LF_STR("root", s->root_dir),
              LF_UINT("entries", s->index_count), LF_UINT("total_bytes", s->total_bytes),
              LF_UINT("hits", atomic_load_explicit(&s->hits, memory_order_relaxed)),
              LF_UINT("misses", atomic_load_explicit(&s->misses, memory_order_relaxed)),
              LF_UINT("writes", atomic_load_explicit(&s->writes, memory_order_relaxed)),
              LF_UINT("evictions", atomic_load_explicit(&s->evictions, memory_order_relaxed)),
              LF_UINT("demotions", atomic_load_explicit(&s->demotions, memory_order_relaxed)));

    for (size_t i = 0; i < s->index_cap; i++) {
        if (s->index[i]) {
            free(s->index[i]->hot_bytes);
            free(s->index[i]);
        }
    }
    free(s->index);
    pthread_cond_destroy(&s->demotion_done_cv);
    pthread_cond_destroy(&s->janitor_wake);
    pthread_mutex_destroy(&s->janitor_lock);
    pthread_mutex_destroy(&s->lock);
    free(s->root_dir);
    free(s);
}

// ----------------------------------------------------------------------------
// Demotion (hot → warm)
// ----------------------------------------------------------------------------

// Demote a single hot entry in place. Caller holds s->lock; we release it
// during the synchronous write and reacquire. Concurrent lookups see
// TIER_DEMOTING and wait on demotion_done_cv.
//
// Drops the hot-tier RAM copy. Under write-through (HASH-MODEL §11) the
// disk file already exists from the original store_put — demotion is a
// pure RAM-free, no I/O. Always succeeds; the int return is preserved
// for ABI compat with the cancel-paths.
static int demote_entry_locked(store_t* s, store_entry_t* e) {
    if (e->tier != TIER_HOT) {
        return 0;
    }
    uint8_t* bytes = e->hot_bytes;
    size_t len = (size_t)e->size_bytes;
    hash_t hash = e->hash;

    e->tier = TIER_WARM;
    e->hot_bytes = NULL;
    free(bytes);
    s->hot_bytes_total -= len;
    // total_bytes unchanged: file was on disk since write_through put.
    atomic_fetch_add_explicit(&s->demotions, 1, memory_order_relaxed);
    pthread_cond_broadcast(&s->demotion_done_cv);

    char hex[65];
    hash_format_hex(&hash, hex);
    LOG_EVENT(SUB_STORE, LOG_DEBUG, EV_STORE_DEMOTE, LF_STR("hash", hex),
              LF_UINT("bytes", (uint64_t)len));
    return 0;
}

// Walk the LRU from the tail, demoting HOT entries with refcount==0 until
// hot_bytes_total + need_bytes fits in hot_budget_bytes (or no candidates
// remain). Returns the number of entries demoted.
static size_t demote_to_fit_locked(store_t* s, size_t need_bytes) {
    size_t n = 0;
    if (s->hot_budget_bytes == 0) {
        return 0;
    }
    store_entry_t* cur = s->lru_tail;
    while (cur) {
        store_entry_t* prev = cur->lru_prev;
        if (s->hot_bytes_total + need_bytes <= s->hot_budget_bytes) {
            break;
        }
        if (cur->tier == TIER_HOT && cur->pin_count == 0) {
            if (demote_entry_locked(s, cur) == 0) {
                n++;
            }
        }
        cur = prev;
    }
    return n;
}

// ----------------------------------------------------------------------------
// Warm eviction
// ----------------------------------------------------------------------------

static size_t do_warm_evict_locked(store_t* s, uint64_t target_bytes) {
    size_t n_evicted = 0;
    store_entry_t* cur = s->lru_tail;
    while (cur && s->total_bytes > target_bytes) {
        store_entry_t* prev = cur->lru_prev;
        if (cur->pin_count > 0 || cur->tier != TIER_WARM) {
            cur = prev;
            continue;
        }
        char path[4096];
        if (hash_to_path(s->root_dir, &cur->hash, path, sizeof(path)) == 0) {
            unlink(path);
        }
        char hex[65];
        hash_format_hex(&cur->hash, hex);
        uint64_t evicted_size = cur->size_bytes;
        s->total_bytes -= cur->size_bytes;
        lru_unlink(s, cur);
        index_delete(s, cur);
        free(cur);
        n_evicted++;
        atomic_fetch_add_explicit(&s->evictions, 1, memory_order_relaxed);
        LOG_EVENT(SUB_STORE, LOG_INFO, EV_STORE_EVICT, LF_STR("hash", hex),
                  LF_UINT("bytes", evicted_size));
        cur = prev;
    }
    return n_evicted;
}

size_t store_evict_to(store_t* s, uint64_t target_bytes) {
    if (!s) {
        return 0;
    }
    pthread_mutex_lock(&s->lock);
    size_t n = do_warm_evict_locked(s, target_bytes);
    pthread_mutex_unlock(&s->lock);
    return n;
}

// ----------------------------------------------------------------------------
// Write path: store_put takes ownership of bytes.
// ----------------------------------------------------------------------------

int store_put(store_t* s, const hash_t* key, uint8_t* bytes, size_t len) {
    // Ownership contract: on rc == 0 (including the dedupe fast path),
    // store_put has either consumed or freed `bytes`. On rc != 0 the caller
    // still owns `bytes` and must free. The validation branch below can
    // only trigger with bytes == NULL (free is then a no-op anyway), so
    // there is no in-function free of caller bytes on the failure path.
    if (!s || !key || (!bytes && len > 0)) {
        return -1;
    }

    hash_t h = *key;

    // Dedupe under lock — but drop the lock for the disk write to avoid
    // serialising every put on the global store mutex.
    pthread_mutex_lock(&s->lock);
    store_entry_t* existing = index_find(s, &h);
    if (existing) {
        existing->last_access_us = now_monotonic_us();
        lru_touch(s, existing);
        pthread_mutex_unlock(&s->lock);
        free(bytes);
        return 0;
    }
    pthread_mutex_unlock(&s->lock);

    // Write-through (HASH-MODEL §11): every put lands on disk before we
    // claim success. The hot-tier RAM copy is a perf overlay on top of
    // this — never the only home for a value. Crash safety follows: a
    // SIGKILL'd process loses at most the in-flight put currently inside
    // write_warm_file; everything previously committed is on disk and
    // recoverable on restart.
    //
    // Ownership contract: store_put takes ownership of `bytes` on
    // success (rc==0) — frees internally for warm-only entries, retains
    // for hot entries. On failure (rc!=0) ownership stays with the
    // caller and store_put MUST NOT free. cached_compute relies on this
    // to avoid double-frees when retrying after a transient write fail.
    int rc = write_warm_file(s->root_dir, &h, bytes, len);
    if (rc != 0) {
        return -1; // caller frees
    }

    pthread_mutex_lock(&s->lock);
    // Race check: another thread may have inserted while we were writing.
    existing = index_find(s, &h);
    if (existing) {
        existing->last_access_us = now_monotonic_us();
        lru_touch(s, existing);
        pthread_mutex_unlock(&s->lock);
        free(bytes); // dedup is success → store owns; release the dup buffer.
        return 0;
    }
    if (index_grow_if_needed(s) != 0) {
        pthread_mutex_unlock(&s->lock);
        return -1; // caller frees
    }

    // Tier choice: keep the buffer in hot if budget allows + value fits.
    // Whether or not we keep the RAM copy, the disk file is already
    // committed — demotion (hot → warm) is now a pure RAM-free, no I/O.
    bool use_hot = s->hot_budget_bytes > 0 && (uint64_t)len <= s->hot_budget_bytes;
    if (use_hot) {
        demote_to_fit_locked(s, len);
        if (s->hot_bytes_total + (uint64_t)len > s->hot_budget_bytes) {
            // All hot entries pinned — no room. Fall through to warm-only.
            use_hot = false;
        }
    }

    store_entry_t* e = calloc(1, sizeof(store_entry_t));
    if (!e) {
        pthread_mutex_unlock(&s->lock);
        return -1; // caller frees
    }
    e->hash = h;
    e->size_bytes = (uint64_t)len;
    e->last_access_us = now_monotonic_us();
    e->pin_count = 0;
    e->tier = use_hot ? TIER_HOT : TIER_WARM;
    if (use_hot) {
        e->hot_bytes = bytes; // ownership transfer to the entry
        s->hot_bytes_total += (uint64_t)len;
    } else {
        e->hot_bytes = NULL;
        free(bytes);
    }
    s->total_bytes += (uint64_t)len; // disk file always counted
    index_insert_bucket(s->index, s->index_cap, e);
    s->index_count++;
    lru_push_head(s, e);
    atomic_fetch_add_explicit(&s->writes, 1, memory_order_relaxed);
    pthread_mutex_unlock(&s->lock);

    char hexbuf[65];
    hash_format_hex(&h, hexbuf);
    LOG_EVENT(SUB_STORE, LOG_DEBUG, EV_STORE_WRITE, LF_STR("hash", hexbuf),
              LF_UINT("bytes", (uint64_t)len),
              LF_STR("tier", use_hot ? "hot+warm" : "warm"));
    return 0;
}

bool store_contains(store_t* s, const hash_t* h) {
    if (!s || !h) {
        return false;
    }
    pthread_mutex_lock(&s->lock);
    store_entry_t* e = index_find(s, h);
    if (e) {
        e->last_access_us = now_monotonic_us();
        lru_touch(s, e);
    }
    pthread_mutex_unlock(&s->lock);
    return e != NULL;
}

int store_forget(store_t* s, const hash_t* h) {
    if (!s || !h) {
        return -1;
    }
    pthread_mutex_lock(&s->lock);
    store_entry_t* e = index_find(s, h);
    if (!e) {
        pthread_mutex_unlock(&s->lock);
        return 0; // already gone — idempotent
    }
    if (e->pin_count > 0) {
        pthread_mutex_unlock(&s->lock);
        return -1; // pinned — caller must release first
    }

    // Free hot bytes if any.
    if (e->tier == TIER_HOT && e->hot_bytes) {
        free(e->hot_bytes);
        s->hot_bytes_total -= e->size_bytes;
    }

    // Unlink the .bin file. The file may already be gone (which is the
    // exact case this function is for) — ignore unlink failures.
    char hex[65];
    hash_format_hex(h, hex);
    char path[STORE_PATH_BUFSZ];
    int n = snprintf(path, sizeof(path), "%s/%c%c/%c%c/%s.bin",
                     s->root_dir, hex[0], hex[1], hex[2], hex[3], hex);
    if (n > 0 && (size_t)n < sizeof(path)) {
        unlink(path);
    }

    s->total_bytes -= e->size_bytes;
    lru_unlink(s, e);
    index_delete(s, e);
    free(e);
    pthread_mutex_unlock(&s->lock);
    return 0;
}

// ----------------------------------------------------------------------------
// Read path
// ----------------------------------------------------------------------------

int store_lookup(store_t* s, const hash_t* h, store_handle_t* out) {
    if (!s || !h || !out) {
        return -1;
    }

    pthread_mutex_lock(&s->lock);
    store_entry_t* e = index_find(s, h);
    if (!e) {
        pthread_mutex_unlock(&s->lock);
        atomic_fetch_add_explicit(&s->misses, 1, memory_order_relaxed);
        return -1;
    }

    // Wait out an in-progress demotion so we see a settled tier.
    while (e->tier == TIER_DEMOTING) {
        pthread_cond_wait(&s->demotion_done_cv, &s->lock);
        // After the wait the entry is WARM; it's still in the index.
    }

    e->last_access_us = now_monotonic_us();
    lru_touch(s, e);
    e->pin_count++;
    store_tier_t tier = e->tier;
    uint8_t* hot = e->hot_bytes;
    size_t len = (size_t)e->size_bytes;
    pthread_mutex_unlock(&s->lock);

    if (tier == TIER_HOT) {
        out->bytes = hot;
        out->len = len;
        out->hash = *h;
        out->_map = NULL;
        out->_map_len = 0;
        out->_fd = -1;
        out->_entry = e;
        atomic_fetch_add_explicit(&s->hits, 1, memory_order_relaxed);
        return 0;
    }

    // Warm: open + mmap outside the lock.
    char path[4096];
    if (hash_to_path(s->root_dir, h, path, sizeof(path)) != 0) {
        pthread_mutex_lock(&s->lock);
        e->pin_count--;
        pthread_mutex_unlock(&s->lock);
        return -1;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        pthread_mutex_lock(&s->lock);
        e->pin_count--;
        pthread_mutex_unlock(&s->lock);
        atomic_fetch_add_explicit(&s->misses, 1, memory_order_relaxed);
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        pthread_mutex_lock(&s->lock);
        e->pin_count--;
        pthread_mutex_unlock(&s->lock);
        return -1;
    }
    size_t file_len = (size_t)st.st_size;
    void* map = NULL;
    if (file_len > 0) {
        map = mmap(NULL, file_len, PROT_READ, MAP_PRIVATE, fd, 0);
        if (map == MAP_FAILED) {
            close(fd);
            pthread_mutex_lock(&s->lock);
            e->pin_count--;
            pthread_mutex_unlock(&s->lock);
            return -1;
        }
    }
    out->bytes = (const uint8_t*)map;
    out->len = file_len;
    out->hash = *h;
    out->_map = map;
    out->_map_len = file_len;
    out->_fd = fd;
    out->_entry = e;
    atomic_fetch_add_explicit(&s->hits, 1, memory_order_relaxed);
    return 0;
}

void store_release(store_t* s, store_handle_t* h) {
    if (!s || !h) {
        return;
    }
    if (h->_map && h->_map_len > 0) {
        munmap(h->_map, h->_map_len);
    }
    if (h->_fd >= 0) {
        close(h->_fd);
    }
    if (h->_entry) {
        pthread_mutex_lock(&s->lock);
        store_entry_t* e = (store_entry_t*)h->_entry;
        if (e->pin_count > 0) {
            e->pin_count--;
        }
        pthread_mutex_unlock(&s->lock);
    }
    h->bytes = NULL;
    h->len = 0;
    h->_map = NULL;
    h->_map_len = 0;
    h->_fd = -1;
    h->_entry = NULL;
}

// ----------------------------------------------------------------------------
// Pinning
// ----------------------------------------------------------------------------

void store_pin(store_t* s, const hash_t* h) {
    if (!s || !h) {
        return;
    }
    pthread_mutex_lock(&s->lock);
    store_entry_t* e = index_find(s, h);
    if (e) {
        e->pin_count++;
    }
    pthread_mutex_unlock(&s->lock);
}

void store_unpin(store_t* s, const hash_t* h) {
    if (!s || !h) {
        return;
    }
    pthread_mutex_lock(&s->lock);
    store_entry_t* e = index_find(s, h);
    if (e && e->pin_count > 0) {
        e->pin_count--;
    }
    pthread_mutex_unlock(&s->lock);
}

// ----------------------------------------------------------------------------
// Stats
// ----------------------------------------------------------------------------

void store_get_stats(store_t* s, store_stats_t* out) {
    if (!s || !out) {
        return;
    }
    pthread_mutex_lock(&s->lock);
    out->entries = s->index_count;
    out->total_bytes = s->total_bytes;
    out->hot_bytes = s->hot_bytes_total;
    out->budget_bytes = s->budget_bytes;
    out->hot_budget_bytes = s->hot_budget_bytes;
    size_t hot_entries = 0;
    for (size_t i = 0; i < s->index_cap; i++) {
        if (s->index[i] && s->index[i]->tier == TIER_HOT) {
            hot_entries++;
        }
    }
    out->hot_entries = hot_entries;
    pthread_mutex_unlock(&s->lock);
    out->hits = atomic_load_explicit(&s->hits, memory_order_relaxed);
    out->misses = atomic_load_explicit(&s->misses, memory_order_relaxed);
    out->writes = atomic_load_explicit(&s->writes, memory_order_relaxed);
    out->evictions = atomic_load_explicit(&s->evictions, memory_order_relaxed);
    out->demotions = atomic_load_explicit(&s->demotions, memory_order_relaxed);
}
