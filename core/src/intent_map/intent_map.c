// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#define _POSIX_C_SOURCE 200809L

#include "v3/intent_map.h"
#include "v3/safe_alloc.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "v3/log.h"

// ----------------------------------------------------------------------------
// On-disk record layout (65 bytes):
//   [32 B identity] [32 B content_hash] [1 B flags]
// Flags bit 0: 1 = tombstone (invalidation). Remaining bits reserved.
// Single write() of 65 bytes is atomic under PIPE_BUF (>= 4096 on Linux).
// ----------------------------------------------------------------------------

#define INTENT_RECORD_SIZE 65
#define INTENT_FLAG_TOMBSTONE 0x01

// ----------------------------------------------------------------------------
// Entry stored in the hashmap. ``live=false`` means the most recent record for
// this identity was a tombstone; we keep the slot so subsequent puts know
// which bucket to reuse, but getters must treat it as a miss.
// ----------------------------------------------------------------------------

typedef struct intent_entry {
    hash_t identity;
    hash_t content_hash;
    bool live;
    bool occupied;
} intent_entry_t;

struct intent_map {
    char* log_path;
    int log_fd;
    pthread_mutex_t lock; // guards hashmap + log write + log_bytes + stats that are non-atomic

    // Open-addressing linear-probing hashmap. Capacity is a power of two.
    intent_entry_t* table;
    size_t cap;
    size_t n_occupied; // live + tombstoned
    size_t n_live;

    uint64_t log_bytes;       // current log file size in bytes
    uint64_t log_records;     // records appended since open

    atomic_uint_least64_t hits;
    atomic_uint_least64_t misses;
    atomic_uint_least64_t invalidations;
};

// ----------------------------------------------------------------------------
// Hashmap helpers
// ----------------------------------------------------------------------------

static size_t fingerprint(const hash_t* h) {
    uint64_t v = 0;
    memcpy(&v, h->bytes, sizeof(v));
    return (size_t)v;
}

// Find the slot for identity: either the matching slot or the first empty
// slot reached during probing. Returns the slot index.
static size_t probe(intent_entry_t* table, size_t cap, const hash_t* identity) {
    size_t mask = cap - 1;
    size_t i = fingerprint(identity) & mask;
    while (table[i].occupied && !hash_eq(&table[i].identity, identity)) {
        i = (i + 1) & mask;
    }
    return i;
}

static int grow_if_needed(intent_map_t* m) {
    if (m->cap != 0 && m->n_occupied * 2 + 1 < m->cap) {
        return 0;
    }
    size_t new_cap = m->cap ? m->cap * 2 : 128;
    intent_entry_t* nt = safe_calloc_array(new_cap, sizeof(intent_entry_t));
    if (!nt) {
        return -1;
    }
    if (m->table) {
        for (size_t i = 0; i < m->cap; i++) {
            if (!m->table[i].occupied) {
                continue;
            }
            size_t j = probe(nt, new_cap, &m->table[i].identity);
            nt[j] = m->table[i];
        }
    }
    free(m->table);
    m->table = nt;
    m->cap = new_cap;
    return 0;
}

// Apply a record's effect to the in-memory hashmap (used by startup scan and
// compaction replay). Caller must hold the lock.
static int apply_record_locked(intent_map_t* m, const uint8_t* rec) {
    hash_t identity, content_hash;
    memcpy(identity.bytes, rec, 32);
    memcpy(content_hash.bytes, rec + 32, 32);
    uint8_t flags = rec[64];
    bool tombstone = (flags & INTENT_FLAG_TOMBSTONE) != 0;

    if (grow_if_needed(m) != 0) {
        return -1;
    }
    size_t i = probe(m->table, m->cap, &identity);
    if (!m->table[i].occupied) {
        m->table[i].occupied = true;
        m->table[i].identity = identity;
        m->n_occupied++;
    }
    bool was_live = m->table[i].live;
    if (tombstone) {
        m->table[i].live = false;
        if (was_live) {
            m->n_live--;
        }
    } else {
        m->table[i].content_hash = content_hash;
        m->table[i].live = true;
        if (!was_live) {
            m->n_live++;
        }
    }
    return 0;
}

// ----------------------------------------------------------------------------
// File path helpers
// ----------------------------------------------------------------------------

static int mkdir_parents(const char* path) {
    // Create parent directories only; ignore EEXIST.
    char buf[4096];
    size_t n = strlen(path);
    if (n >= sizeof(buf)) {
        return -1;
    }
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            if (mkdir(buf, 0700) != 0 && errno != EEXIST) {
                return -1;
            }
            buf[i] = '/';
        }
    }
    return 0;
}

// ----------------------------------------------------------------------------
// Startup scan
// ----------------------------------------------------------------------------

static int scan_log_locked(intent_map_t* m) {
    int fd = open(m->log_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) {
            return 0;
        }
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return -1;
    }
    off_t full_size = st.st_size;
    off_t complete_size = full_size - (full_size % INTENT_RECORD_SIZE);
    if (complete_size != full_size) {
        LOG_EVENT(SUB_STORE, LOG_WARN, EV_INTENT_MAP_TAIL_TRUNC,
                  LF_UINT("discarded_bytes", (uint64_t)(full_size - complete_size)));
    }

    uint8_t buf[INTENT_RECORD_SIZE * 64];
    off_t remaining = complete_size;
    off_t pos = 0;
    int rc = 0;
    while (remaining > 0) {
        size_t chunk = remaining > (off_t)sizeof(buf) ? sizeof(buf) : (size_t)remaining;
        ssize_t n = pread(fd, buf, chunk, pos);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            rc = -1;
            break;
        }
        if ((size_t)n < chunk) {
            // Short read; treat as truncation boundary.
            complete_size = pos + (n - (n % INTENT_RECORD_SIZE));
            chunk = (size_t)(complete_size - pos);
        }
        for (size_t off = 0; off + INTENT_RECORD_SIZE <= chunk; off += INTENT_RECORD_SIZE) {
            if (apply_record_locked(m, buf + off) != 0) {
                rc = -1;
                break;
            }
        }
        if (rc != 0) {
            break;
        }
        pos += (off_t)chunk;
        remaining = complete_size - pos;
    }
    close(fd);

    if (rc == 0 && complete_size != full_size) {
        // Truncate the partial tail to keep the file well-formed for subsequent appends.
        if (truncate(m->log_path, complete_size) != 0) {
            rc = -1;
        }
    }
    m->log_bytes = (uint64_t)(rc == 0 ? complete_size : 0);
    return rc;
}

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

intent_map_t* intent_map_open(const char* log_path) {
    if (!log_path) {
        return NULL;
    }
    intent_map_t* m = calloc(1, sizeof(*m));
    if (!m) {
        return NULL;
    }
    m->log_fd = -1;
    m->log_path = strdup(log_path);
    if (!m->log_path) {
        free(m);
        return NULL;
    }
    // Frama-C audit finding IM-1: pthread_mutex_init can fail (ENOMEM,
    // EAGAIN). On failure the mutex is unusable and subsequent
    // pthread_mutex_lock is UB. Roll back the partial init cleanly.
    if (pthread_mutex_init(&m->lock, NULL) != 0) {
        free(m->log_path);
        free(m);
        return NULL;
    }

    if (mkdir_parents(log_path) != 0) {
        free(m->log_path);
        pthread_mutex_destroy(&m->lock);
        free(m);
        return NULL;
    }

    pthread_mutex_lock(&m->lock);
    if (scan_log_locked(m) != 0) {
        pthread_mutex_unlock(&m->lock);
        free(m->table);
        free(m->log_path);
        pthread_mutex_destroy(&m->lock);
        free(m);
        return NULL;
    }
    pthread_mutex_unlock(&m->lock);

    // Open the log for append. O_APPEND makes each write atomic at the
    // position of the file end.
    m->log_fd = open(m->log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (m->log_fd < 0) {
        free(m->table);
        free(m->log_path);
        pthread_mutex_destroy(&m->lock);
        free(m);
        return NULL;
    }

    return m;
}

void intent_map_close(intent_map_t* m) {
    if (!m) {
        return;
    }
    if (m->log_fd >= 0) {
        fsync(m->log_fd);
        close(m->log_fd);
    }
    free(m->table);
    free(m->log_path);
    pthread_mutex_destroy(&m->lock);
    free(m);
}

int intent_map_get(intent_map_t* m, const hash_t* identity, hash_t* out_content_hash) {
    if (!m || !identity) {
        return -1;
    }
    pthread_mutex_lock(&m->lock);
    if (m->cap == 0 || m->n_occupied == 0) {
        pthread_mutex_unlock(&m->lock);
        atomic_fetch_add_explicit(&m->misses, 1, memory_order_relaxed);
        return -1;
    }
    size_t i = probe(m->table, m->cap, identity);
    if (!m->table[i].occupied || !m->table[i].live) {
        pthread_mutex_unlock(&m->lock);
        atomic_fetch_add_explicit(&m->misses, 1, memory_order_relaxed);
        return -1;
    }
    if (out_content_hash) {
        *out_content_hash = m->table[i].content_hash;
    }
    pthread_mutex_unlock(&m->lock);
    atomic_fetch_add_explicit(&m->hits, 1, memory_order_relaxed);
    return 0;
}

static int write_record_locked(intent_map_t* m, const hash_t* identity,
                               const hash_t* content_hash, uint8_t flags) {
    uint8_t rec[INTENT_RECORD_SIZE];
    memcpy(rec, identity->bytes, 32);
    if (content_hash) {
        memcpy(rec + 32, content_hash->bytes, 32);
    } else {
        memset(rec + 32, 0, 32);
    }
    rec[64] = flags;

    // O_APPEND + single write of 65 B (<< PIPE_BUF) is atomic across processes.
    ssize_t n = write(m->log_fd, rec, sizeof(rec));
    if (n != (ssize_t)sizeof(rec)) {
        return -1;
    }
    m->log_bytes += sizeof(rec);
    m->log_records++;
    return apply_record_locked(m, rec);
}

int intent_map_put(intent_map_t* m, const hash_t* identity, const hash_t* content_hash) {
    if (!m || !identity || !content_hash) {
        return -1;
    }
    pthread_mutex_lock(&m->lock);
    int rc = write_record_locked(m, identity, content_hash, 0);
    pthread_mutex_unlock(&m->lock);
    return rc;
}

int intent_map_invalidate(intent_map_t* m, const hash_t* identity) {
    if (!m || !identity) {
        return -1;
    }
    pthread_mutex_lock(&m->lock);
    int rc = write_record_locked(m, identity, NULL, INTENT_FLAG_TOMBSTONE);
    pthread_mutex_unlock(&m->lock);
    if (rc == 0) {
        atomic_fetch_add_explicit(&m->invalidations, 1, memory_order_relaxed);
    }
    return rc;
}

int intent_map_flush(intent_map_t* m) {
    if (!m || m->log_fd < 0) {
        return -1;
    }
    return fsync(m->log_fd) == 0 ? 0 : -1;
}

int intent_map_compact(intent_map_t* m) {
    if (!m) {
        return -1;
    }
    pthread_mutex_lock(&m->lock);

    // Collect live entries.
    // ALLOC: `live` holds copies of every live entry (~64 bytes each).
    // Freed on EVERY return path below (success and failure). Lock is
    // held throughout to prevent concurrent intent_map_put from growing
    // the table or invalidating our snapshot.  File I/O under the lock
    // is acknowledged debt (§10) — pipelines compact rarely.
    size_t n = m->n_live;
    intent_entry_t* live = n ? safe_calloc_array(n, sizeof(intent_entry_t)) : NULL;
    if (n && !live) {
        pthread_mutex_unlock(&m->lock);
        return -1;
    }
    size_t w = 0;
    for (size_t i = 0; i < m->cap; i++) {
        if (m->table[i].occupied && m->table[i].live) {
            live[w++] = m->table[i];
        }
    }

    char tmp_path[4096];
    int np = snprintf(tmp_path, sizeof(tmp_path), "%s.compact", m->log_path);
    if (np < 0 || (size_t)np >= sizeof(tmp_path)) {
        free(live);
        pthread_mutex_unlock(&m->lock);
        return -1;
    }
    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        free(live);
        pthread_mutex_unlock(&m->lock);
        return -1;
    }
    uint64_t bytes_written = 0;
    for (size_t i = 0; i < w; i++) {
        uint8_t rec[INTENT_RECORD_SIZE];
        memcpy(rec, live[i].identity.bytes, 32);
        memcpy(rec + 32, live[i].content_hash.bytes, 32);
        rec[64] = 0;
        ssize_t wr = write(fd, rec, sizeof(rec));
        if (wr != (ssize_t)sizeof(rec)) {
            close(fd);
            unlink(tmp_path);
            free(live);
            pthread_mutex_unlock(&m->lock);
            return -1;
        }
        bytes_written += sizeof(rec);
    }
    if (fsync(fd) != 0) {
        close(fd);
        unlink(tmp_path);
        free(live);
        pthread_mutex_unlock(&m->lock);
        return -1;
    }
    close(fd);

    // Close the append-only fd before the rename so any external observer
    // (e.g. fsync on exit) isn't surprised by a new inode.
    int old_fd = m->log_fd;
    m->log_fd = -1;
    if (old_fd >= 0) {
        close(old_fd);
    }

    if (rename(tmp_path, m->log_path) != 0) {
        unlink(tmp_path);
        free(live);
        // Try to restore the append fd so the map is still usable.
        m->log_fd = open(m->log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        pthread_mutex_unlock(&m->lock);
        return -1;
    }

    m->log_bytes = bytes_written;
    m->log_fd = open(m->log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    free(live);
    pthread_mutex_unlock(&m->lock);
    return m->log_fd >= 0 ? 0 : -1;
}

void intent_map_get_stats(intent_map_t* m, intent_map_stats_t* out) {
    if (!m || !out) {
        return;
    }
    pthread_mutex_lock(&m->lock);
    out->live_entries = m->n_live;
    out->log_records = m->log_records;
    out->log_bytes = m->log_bytes;
    pthread_mutex_unlock(&m->lock);
    out->hits = atomic_load_explicit(&m->hits, memory_order_relaxed);
    out->misses = atomic_load_explicit(&m->misses, memory_order_relaxed);
    out->invalidations = atomic_load_explicit(&m->invalidations, memory_order_relaxed);
}

void intent_map_for_each(intent_map_t* m, intent_map_walk_cb cb, void* ctx) {
    if (!m || !cb) {
        return;
    }
    // Snapshot live entries under lock, then dispatch outside the lock so
    // the callback can call back into the store / log subsystems without
    // creating a lock ordering issue.
    pthread_mutex_lock(&m->lock);
    size_t n = m->n_live;
    if (n == 0) {
        pthread_mutex_unlock(&m->lock);
        return;
    }
    hash_t* identities = safe_malloc_array(n, sizeof(hash_t));
    hash_t* contents = safe_malloc_array(n, sizeof(hash_t));
    if (!identities || !contents) {
        free(identities);
        free(contents);
        pthread_mutex_unlock(&m->lock);
        return;
    }
    size_t k = 0;
    for (size_t i = 0; i < m->cap && k < n; i++) {
        if (m->table[i].occupied && m->table[i].live) {
            identities[k] = m->table[i].identity;
            contents[k] = m->table[i].content_hash;
            k++;
        }
    }
    pthread_mutex_unlock(&m->lock);

    for (size_t i = 0; i < k; i++) {
        cb(&identities[i], &contents[i], ctx);
    }
    free(identities);
    free(contents);
}
