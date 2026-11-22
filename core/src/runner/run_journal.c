// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Run journal — SQLite-backed log of completed pipeline runs.
//
// See include/v3/run_journal.h for the API rationale.
//
// One row per run, written at run-end. Schema versioning is implicit:
// CREATE TABLE IF NOT EXISTS is idempotent; future schema changes will
// add columns via `ALTER TABLE ADD COLUMN` (additive only — SQLite
// supports it natively and old readers safely ignore unknown columns).

// strdup + strndup are POSIX, not C11 — exposed via this feature-test
// macro on glibc. Must precede every system header.
#define _POSIX_C_SOURCE 200809L

#include "v3/run_journal.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JOURNAL_RETENTION_DAYS  30
#define JOURNAL_MAX_ROWS        10000

struct run_journal {
    sqlite3* db;
    pthread_mutex_t lock;          // serialises prepared-statement reuse
    sqlite3_stmt* stmt_insert;
    sqlite3_stmt* stmt_get_produced;
};

// ─── Helpers ──────────────────────────────────────────────────────────

static void journal_warn(const char* msg, sqlite3* db) {
    fprintf(stderr, "[run_journal] %s%s%s\n", msg,
            db ? ": " : "", db ? sqlite3_errmsg(db) : "");
}

// Append-only string builder for the JSON arrays / objects we serialise.
typedef struct {
    char* buf;
    size_t len;
    size_t cap;
    int oom;
} sb_t;

static void sb_reserve(sb_t* s, size_t need) {
    if (s->oom) return;
    // Frama-C audit finding RJ-1: guard against
    //  (a) `s->len + need + 1` wrap (size_t overflow).
    //  (b) capacity doubling `cap *= 2` wrap.
    if (need > SIZE_MAX - s->len - 1) { s->oom = 1; return; }
    if (s->len + need + 1 <= s->cap) return;
    size_t cap = s->cap ? s->cap : 256;
    while (cap < s->len + need + 1) {
        if (cap > SIZE_MAX / 2) { s->oom = 1; return; }
        cap *= 2;
    }
    char* nb = realloc(s->buf, cap);
    if (!nb) { s->oom = 1; return; }
    s->buf = nb;
    s->cap = cap;
}

static void sb_putc(sb_t* s, char c) {
    sb_reserve(s, 1);
    if (s->oom) return;
    s->buf[s->len++] = c;
    s->buf[s->len] = '\0';
}

static void sb_puts(sb_t* s, const char* str) {
    size_t n = strlen(str);
    sb_reserve(s, n);
    if (s->oom) return;
    memcpy(s->buf + s->len, str, n);
    s->len += n;
    s->buf[s->len] = '\0';
}

// Append a JSON-quoted string. Handles ", \, control chars per RFC 8259.
static void sb_put_json_str(sb_t* s, const char* str) {
    if (!str) { sb_puts(s, "null"); return; }
    sb_putc(s, '"');
    for (const unsigned char* p = (const unsigned char*)str; *p; p++) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') { sb_putc(s, '\\'); sb_putc(s, (char)c); }
        else if (c == '\n') sb_puts(s, "\\n");
        else if (c == '\r') sb_puts(s, "\\r");
        else if (c == '\t') sb_puts(s, "\\t");
        else if (c < 0x20) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", c);
            sb_puts(s, buf);
        } else {
            sb_putc(s, (char)c);
        }
    }
    sb_putc(s, '"');
}

// ─── Schema ───────────────────────────────────────────────────────────

// Schema: `id` is the stable durable PK (auto-increment) — `run_id` is
// the runner's session-scoped identifier (`r_1`, `r_2`, ...) which
// resets every time the processor restarts. Using run_id as the PK
// would silently overwrite older rows; we keep run_id as a regular
// indexed text column for diagnostic display only.
static const char* SCHEMA_SQL =
    "PRAGMA journal_mode=WAL;"
    "PRAGMA synchronous=NORMAL;"
    "CREATE TABLE IF NOT EXISTS runs ("
    " id              INTEGER PRIMARY KEY AUTOINCREMENT,"
    " run_id          TEXT NOT NULL,"
    " pipeline_name   TEXT NOT NULL,"
    " input_json      TEXT NOT NULL,"
    " input_hash      TEXT NOT NULL,"
    " test_case_label TEXT,"
    " started_at      INTEGER NOT NULL,"
    " duration_us     INTEGER NOT NULL,"
    " success         INTEGER NOT NULL,"
    " error_kind      TEXT,"
    " error_message   TEXT,"
    " n_executed      INTEGER NOT NULL,"
    " n_cache_hits    INTEGER NOT NULL,"
    " subscribe_set   TEXT NOT NULL,"
    " produced_hashes TEXT NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS runs_started_at_idx     ON runs(started_at DESC);"
    "CREATE INDEX IF NOT EXISTS runs_pipeline_input_idx ON runs(pipeline_name, input_hash);";

static const char* INSERT_SQL =
    "INSERT INTO runs ("
    " run_id, pipeline_name, input_json, input_hash, test_case_label,"
    " started_at, duration_us, success, error_kind, error_message,"
    " n_executed, n_cache_hits, subscribe_set, produced_hashes"
    ") VALUES (?,?,?,?,?, ?,?,?,?,?, ?,?,?,?)";

static const char* SELECT_PRODUCED_SQL =
    "SELECT produced_hashes FROM runs WHERE id = ?";

// ─── Open / close ─────────────────────────────────────────────────────

run_journal_t* run_journal_open(const char* db_path) {
    if (!db_path) return NULL;
    sqlite3* db = NULL;
    int rc = sqlite3_open_v2(db_path, &db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE
                                 | SQLITE_OPEN_FULLMUTEX,
                             NULL);
    if (rc != SQLITE_OK) {
        journal_warn("open failed", db);
        if (db) sqlite3_close(db);
        return NULL;
    }
    char* errmsg = NULL;
    if (sqlite3_exec(db, SCHEMA_SQL, NULL, NULL, &errmsg) != SQLITE_OK) {
        fprintf(stderr, "[run_journal] schema init failed: %s\n",
                errmsg ? errmsg : "(unknown)");
        sqlite3_free(errmsg);
        sqlite3_close(db);
        return NULL;
    }

    run_journal_t* j = calloc(1, sizeof(*j));
    if (!j) { sqlite3_close(db); return NULL; }
    j->db = db;
    pthread_mutex_init(&j->lock, NULL);

    if (sqlite3_prepare_v2(db, INSERT_SQL, -1, &j->stmt_insert, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, SELECT_PRODUCED_SQL, -1, &j->stmt_get_produced, NULL) != SQLITE_OK) {
        journal_warn("prepare failed", db);
        run_journal_close(j);
        return NULL;
    }
    return j;
}

void run_journal_close(run_journal_t* j) {
    if (!j) return;
    if (j->stmt_insert)        sqlite3_finalize(j->stmt_insert);
    if (j->stmt_get_produced)  sqlite3_finalize(j->stmt_get_produced);
    if (j->db)                 sqlite3_close(j->db);
    pthread_mutex_destroy(&j->lock);
    free(j);
}

// ─── Prune ────────────────────────────────────────────────────────────

void run_journal_prune(run_journal_t* j) {
    if (!j) return;
    pthread_mutex_lock(&j->lock);
    char* errmsg = NULL;
    // 1) drop rows older than the retention horizon.
    char sql[256];
    int64_t cutoff_us = -((int64_t)JOURNAL_RETENTION_DAYS) * 24LL * 3600LL * 1000000LL;
    snprintf(sql, sizeof(sql),
             "DELETE FROM runs WHERE started_at < (CAST(strftime('%%s','now') AS INTEGER) * 1000000 + (%lld))",
             (long long)cutoff_us);
    if (sqlite3_exec(j->db, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
        fprintf(stderr, "[run_journal] prune (age) failed: %s\n", errmsg ? errmsg : "");
        sqlite3_free(errmsg); errmsg = NULL;
    }
    // 2) keep at most JOURNAL_MAX_ROWS rows.
    snprintf(sql, sizeof(sql),
             "DELETE FROM runs WHERE rowid IN ("
             "  SELECT rowid FROM runs ORDER BY started_at DESC LIMIT -1 OFFSET %d)",
             JOURNAL_MAX_ROWS);
    if (sqlite3_exec(j->db, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
        fprintf(stderr, "[run_journal] prune (count) failed: %s\n", errmsg ? errmsg : "");
        sqlite3_free(errmsg);
    }
    pthread_mutex_unlock(&j->lock);
}

// ─── Record ───────────────────────────────────────────────────────────

static char* build_subscribe_json(const char* const* sub, size_t n) {
    sb_t s = {0};
    sb_putc(&s, '[');
    for (size_t i = 0; i < n; i++) {
        if (i) sb_putc(&s, ',');
        sb_put_json_str(&s, sub[i] ? sub[i] : "");
    }
    sb_putc(&s, ']');
    if (s.oom) { free(s.buf); return NULL; }
    return s.buf ? s.buf : strdup("[]");
}

static char* build_produced_json(const run_artifact_t* arts, size_t n) {
    sb_t s = {0};
    sb_putc(&s, '{');
    for (size_t i = 0; i < n; i++) {
        if (!arts[i].name || !arts[i].hash_hex) continue;
        if (i) sb_putc(&s, ',');
        sb_put_json_str(&s, arts[i].name);
        sb_putc(&s, ':');
        sb_put_json_str(&s, arts[i].hash_hex);
    }
    sb_putc(&s, '}');
    if (s.oom) { free(s.buf); return NULL; }
    return s.buf ? s.buf : strdup("{}");
}

int run_journal_record(run_journal_t* j, const run_record_t* rec) {
    if (!j || !rec || !rec->run_id || !rec->pipeline_name ||
        !rec->input_json || !rec->input_hash) {
        return -1;
    }
    char* sub_json = build_subscribe_json(rec->subscribe, rec->n_subscribe);
    char* prod_json = build_produced_json(rec->produced, rec->n_produced);
    if (!sub_json || !prod_json) {
        free(sub_json); free(prod_json);
        return -1;
    }

    pthread_mutex_lock(&j->lock);
    sqlite3_stmt* st = j->stmt_insert;
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    int rc = SQLITE_OK;
    rc |= sqlite3_bind_text  (st, 1,  rec->run_id,         -1, SQLITE_TRANSIENT);
    rc |= sqlite3_bind_text  (st, 2,  rec->pipeline_name,  -1, SQLITE_TRANSIENT);
    rc |= sqlite3_bind_text  (st, 3,  rec->input_json,     -1, SQLITE_TRANSIENT);
    rc |= sqlite3_bind_text  (st, 4,  rec->input_hash,     -1, SQLITE_TRANSIENT);
    if (rec->test_case_label)
        rc |= sqlite3_bind_text(st, 5, rec->test_case_label, -1, SQLITE_TRANSIENT);
    else
        rc |= sqlite3_bind_null(st, 5);
    rc |= sqlite3_bind_int64 (st, 6,  (sqlite3_int64)rec->started_at_us);
    rc |= sqlite3_bind_int64 (st, 7,  (sqlite3_int64)rec->duration_us);
    rc |= sqlite3_bind_int   (st, 8,  rec->success ? 1 : 0);
    if (rec->error_kind)    rc |= sqlite3_bind_text(st, 9,  rec->error_kind,    -1, SQLITE_TRANSIENT);
    else                    rc |= sqlite3_bind_null(st, 9);
    if (rec->error_message) rc |= sqlite3_bind_text(st, 10, rec->error_message, -1, SQLITE_TRANSIENT);
    else                    rc |= sqlite3_bind_null(st, 10);
    rc |= sqlite3_bind_int   (st, 11, (int)rec->n_executed);
    rc |= sqlite3_bind_int   (st, 12, (int)rec->n_cache_hits);
    rc |= sqlite3_bind_text  (st, 13, sub_json,  -1, SQLITE_TRANSIENT);
    rc |= sqlite3_bind_text  (st, 14, prod_json, -1, SQLITE_TRANSIENT);

    int step_rc = SQLITE_DONE;
    if (rc == SQLITE_OK) {
        step_rc = sqlite3_step(st);
    }
    pthread_mutex_unlock(&j->lock);
    free(sub_json);
    free(prod_json);

    if (rc != SQLITE_OK || step_rc != SQLITE_DONE) {
        journal_warn("insert failed", j->db);
        return -1;
    }
    return 0;
}

// ─── List ─────────────────────────────────────────────────────────────

int run_journal_list(run_journal_t* j, uint32_t limit, uint64_t before_us,
                     const char* pipeline, const char* input_hash,
                     int success_filter,
                     run_journal_row_t** out, size_t* n_out) {
    if (!j || !out || !n_out) return -1;
    *out = NULL;
    *n_out = 0;
    if (limit == 0 || limit > 200) limit = 200;

    // Build SQL dynamically. Bind params in a known order: pipeline (or
    // skipped), input_hash (or skipped), before_us (or skipped),
    // success_filter (or skipped), limit (always).
    sb_t sb = {0};
    sb_puts(&sb,
            "SELECT id, run_id, pipeline_name, test_case_label, input_hash,"
            " started_at, duration_us, success, n_executed, n_cache_hits,"
            " subscribe_set"
            " FROM runs WHERE 1=1");
    int idx = 0;
    int idx_pipeline = 0, idx_hash = 0, idx_before = 0, idx_success = 0;
    if (pipeline)   { sb_puts(&sb, " AND pipeline_name = ?"); idx_pipeline = ++idx; }
    if (input_hash) { sb_puts(&sb, " AND input_hash = ?");    idx_hash     = ++idx; }
    if (before_us)  { sb_puts(&sb, " AND started_at < ?");    idx_before   = ++idx; }
    if (success_filter == 0 || success_filter == 1) {
        sb_puts(&sb, " AND success = ?"); idx_success = ++idx;
    }
    sb_puts(&sb, " ORDER BY started_at DESC LIMIT ?");
    int idx_limit = ++idx;
    if (sb.oom) { free(sb.buf); return -1; }

    pthread_mutex_lock(&j->lock);
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(j->db, sb.buf, -1, &st, NULL);
    free(sb.buf);
    if (rc != SQLITE_OK) {
        journal_warn("list prepare failed", j->db);
        pthread_mutex_unlock(&j->lock);
        return -1;
    }
    if (idx_pipeline) sqlite3_bind_text (st, idx_pipeline, pipeline,   -1, SQLITE_TRANSIENT);
    if (idx_hash)     sqlite3_bind_text (st, idx_hash,     input_hash, -1, SQLITE_TRANSIENT);
    if (idx_before)   sqlite3_bind_int64(st, idx_before,   (sqlite3_int64)before_us);
    if (idx_success)  sqlite3_bind_int  (st, idx_success,  success_filter);
    sqlite3_bind_int(st, idx_limit, (int)limit);

    size_t cap = limit;
    run_journal_row_t* rows = calloc(cap, sizeof(*rows));
    if (!rows) { sqlite3_finalize(st); pthread_mutex_unlock(&j->lock); return -1; }
    size_t n = 0;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        run_journal_row_t* r = &rows[n];
        r->journal_id      = sqlite3_column_int64(st, 0);
        const char* s_run_id    = (const char*)sqlite3_column_text(st, 1);
        const char* s_pipeline  = (const char*)sqlite3_column_text(st, 2);
        const char* s_label     = (const char*)sqlite3_column_text(st, 3);
        const char* s_hash      = (const char*)sqlite3_column_text(st, 4);
        const char* s_subscribe = (const char*)sqlite3_column_text(st, 10);
        r->run_id          = s_run_id    ? strdup(s_run_id)    : NULL;
        r->pipeline_name   = s_pipeline  ? strdup(s_pipeline)  : NULL;
        r->test_case_label = s_label     ? strdup(s_label)     : NULL;
        r->input_hash      = s_hash      ? strdup(s_hash)      : NULL;
        r->started_at_us   = (uint64_t)sqlite3_column_int64(st, 5);
        r->duration_us     = (uint64_t)sqlite3_column_int64(st, 6);
        r->success         = sqlite3_column_int(st, 7);
        r->n_executed      = (uint32_t)sqlite3_column_int(st, 8);
        r->n_cache_hits    = (uint32_t)sqlite3_column_int(st, 9);
        r->subscribe_set_json = s_subscribe ? strdup(s_subscribe) : strdup("[]");
        n++;
        if (n >= cap) break;
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&j->lock);

    *out = rows;
    *n_out = n;
    return 0;
}

void run_journal_rows_free(run_journal_row_t* rows, size_t n) {
    if (!rows) return;
    for (size_t i = 0; i < n; i++) {
        free(rows[i].run_id);
        free(rows[i].pipeline_name);
        free(rows[i].test_case_label);
        free(rows[i].input_hash);
        free(rows[i].subscribe_set_json);
    }
    free(rows);
}

// ─── Get produced ─────────────────────────────────────────────────────

// Tiny JSON-object scanner — just enough to walk {"name1":"hash1",...}.
// Strings are unescaped in place; we only handle the escapes we emit.
static int parse_produced_json(const char* json, char*** names_out,
                               char*** hashes_out, size_t* n_out) {
    *names_out = NULL;
    *hashes_out = NULL;
    *n_out = 0;
    if (!json) return 0;
    const char* p = json;
    while (*p && *p != '{') p++;
    if (*p != '{') return 0;
    p++;

    size_t cap = 8;
    char** names = calloc(cap, sizeof(char*));
    char** hashes = calloc(cap, sizeof(char*));
    size_t n = 0;
    if (!names || !hashes) { free(names); free(hashes); return -1; }

    while (*p) {
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t') p++;
        if (*p == '}' || !*p) break;
        if (*p != '"') goto fail;
        p++;
        const char* k_start = p;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) p++;
            p++;
        }
        if (*p != '"') goto fail;
        size_t k_len = (size_t)(p - k_start);
        char* k = strndup(k_start, k_len);
        p++;
        while (*p == ' ' || *p == ':') p++;
        if (*p != '"') { free(k); goto fail; }
        p++;
        const char* v_start = p;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) p++;
            p++;
        }
        if (*p != '"') { free(k); goto fail; }
        size_t v_len = (size_t)(p - v_start);
        char* v = strndup(v_start, v_len);
        p++;

        if (!k || !v) { free(k); free(v); goto fail; }
        if (n == cap) {
            // Assign each realloc result to its slot immediately on
            // success: realloc may have already freed the original
            // storage, so we MUST publish the new pointer before the
            // sibling realloc can fail. Otherwise the `fail:` cleanup
            // loop reads through a stale pointer (heap-UAF). The new
            // tail entries are zeroed so the cleanup loop's free()s on
            // them are no-ops.
            // Frama-C audit finding RJ-2: guard against cap * 2
            // and new_cap * sizeof(char*) overflowing size_t.
            if (cap > SIZE_MAX / 2) { free(k); free(v); goto fail; }
            size_t new_cap = cap * 2;
            if (new_cap > SIZE_MAX / sizeof(char*)) { free(k); free(v); goto fail; }
            char** nn = realloc(names, new_cap * sizeof(char*));
            if (!nn) { free(k); free(v); goto fail; }
            names = nn;
            memset(names + cap, 0, (new_cap - cap) * sizeof(char*));
            char** nh = realloc(hashes, new_cap * sizeof(char*));
            if (!nh) { free(k); free(v); goto fail; }
            hashes = nh;
            memset(hashes + cap, 0, (new_cap - cap) * sizeof(char*));
            cap = new_cap;
        }
        names[n] = k;
        hashes[n] = v;
        n++;
    }

    *names_out = names;
    *hashes_out = hashes;
    *n_out = n;
    return 0;

fail:
    for (size_t i = 0; i < n; i++) { free(names[i]); free(hashes[i]); }
    free(names); free(hashes);
    return -1;
}

int run_journal_get_produced(run_journal_t* j, int64_t journal_id,
                             char*** names_out, char*** hashes_out, size_t* n_out) {
    if (!j || !names_out || !hashes_out || !n_out) return -1;
    pthread_mutex_lock(&j->lock);
    sqlite3_stmt* st = j->stmt_get_produced;
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)journal_id);
    int rc = sqlite3_step(st);
    int ret = -1;
    if (rc == SQLITE_ROW) {
        const char* json = (const char*)sqlite3_column_text(st, 0);
        ret = parse_produced_json(json, names_out, hashes_out, n_out);
    }
    pthread_mutex_unlock(&j->lock);
    return ret;
}

void run_journal_produced_free(char** names, char** hashes, size_t n) {
    if (names) {
        for (size_t i = 0; i < n; i++) free(names[i]);
        free(names);
    }
    if (hashes) {
        for (size_t i = 0; i < n; i++) free(hashes[i]);
        free(hashes);
    }
}
