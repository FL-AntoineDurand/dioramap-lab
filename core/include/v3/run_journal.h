// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#ifndef V3_RUN_JOURNAL_H
#define V3_RUN_JOURNAL_H

// Run journal — a SQLite-backed log of completed pipeline runs at
// <cache_root>/runs.sqlite. Source of truth for the lab-ui's L6
// history list and the seed for A14's control-plane usage push.
//
// One row per run, written atomically at run-end. There are no
// in-flight rows: the runner accumulates state on the session struct
// during the run (started_at, n_executed, n_cache_hits, the list of
// produced (artifact_name, content_hash) pairs) and assembles a single
// run_record_t at run-complete time. This keeps the journal API tiny
// and avoids per-step / per-artifact UPDATE traffic on a hot path.
//
// Concurrency: SQLite WAL mode + an internal mutex around prepared
// statements. Single-process today (one writer thread, occasional
// reader on the LIST_RUNS handler). If/when the processor goes
// multi-session-concurrent on writes, WAL still serialises them
// correctly; the mutex is just to keep the prepared-statement reuse
// safe.
//
// Retention: pruned at startup via run_journal_prune() — drops rows
// older than 30 days OR keeps at most 10k rows, whichever is tighter.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>   // ACSL valid_read_string

#ifdef __cplusplus
extern "C" {
#endif

typedef struct run_journal run_journal_t;

// One produced-artifact pair. produced_hashes in run_record_t is an
// array of these; the journal serialises it as JSON {name: hash_hex}.
typedef struct run_artifact {
    const char* name;       // borrowed; copied on record write
    const char* hash_hex;   // borrowed; 64-char + NUL
} run_artifact_t;

// Full run record passed to run_journal_record() at run-end. All
// borrowed pointers are copied; ownership stays with the caller.
typedef struct run_record {
    const char* run_id;            // session-scoped, NOT NULL
    const char* pipeline_name;     // NOT NULL
    const char* input_json;        // NOT NULL; "{}" for empty
    const char* input_hash;        // hex, NOT NULL (caller computes via hash module)
    const char* test_case_label;   // NULL allowed (SDK runs have no label)
    uint64_t started_at_us;        // epoch micros
    uint64_t duration_us;
    bool success;
    const char* error_kind;        // NULL allowed
    const char* error_message;     // NULL allowed
    uint32_t n_executed;
    uint32_t n_cache_hits;
    const char* const* subscribe;  // NOT NULL (may be empty)
    size_t n_subscribe;
    const run_artifact_t* produced;
    size_t n_produced;
} run_record_t;

// Read-side: row returned by run_journal_list() / decoded for a
// LIST_RUNS_RESPONSE. Strings are owned by the journal; the whole
// list is freed via run_journal_records_free().
//
// `journal_id` is the durable cross-session identifier — the runner's
// `run_id` is session-scoped (`r_1`, `r_2`, ...) and collides every
// time the processor restarts, so we expose the row's auto-increment
// PK as the stable handle. REHYDRATE_RUN takes `journal_id`, not run_id.
typedef struct run_journal_row {
    int64_t journal_id;           // auto-increment PK
    char* run_id;                 // runner's session-scoped id (informational)
    char* pipeline_name;
    char* test_case_label;        // NULL if absent
    char* input_hash;
    uint64_t started_at_us;
    uint64_t duration_us;
    int success;                  // 0 / 1
    uint32_t n_executed;
    uint32_t n_cache_hits;
    char* subscribe_set_json;     // raw JSON array string
} run_journal_row_t;

// Open or create the journal at db_path. Creates schema + indexes if
// missing. Returns NULL on failure (logged via the log subsystem).
/*@ requires db_path == \null || valid_read_string(db_path);
    assigns \nothing;
    ensures \result == \null || \valid(\result);
*/
run_journal_t* run_journal_open(const char* db_path);

/*@ requires j == \null || \valid(j);
    assigns *j;
*/
void run_journal_close(run_journal_t* j);

/*@ requires j == \null || \valid(j);
    assigns *j;
*/
void run_journal_prune(run_journal_t* j);

/*@ requires j == \null || \valid(j);
    requires rec == \null || \valid_read(rec);
    assigns *j;
    ensures \result == 0 || \result == -1;
*/
int run_journal_record(run_journal_t* j, const run_record_t* rec);

// List rows newest-first. Filters are optional (pass NULL/0 to skip).
//   limit         = max rows (capped at 200)
//   before_us     = strict upper bound on started_at; 0 = none
//   pipeline      = exact match; NULL = any
//   input_hash    = exact match; NULL = any
//   success_filter: -1 any, 0 fail-only, 1 success-only
// Caller frees with run_journal_rows_free.
/*@ requires j == \null || \valid(j);
    requires pipeline == \null || valid_read_string(pipeline);
    requires input_hash == \null || valid_read_string(input_hash);
    requires out == \null || \valid(out);
    requires n_out == \null || \valid(n_out);
    assigns *j, *out, *n_out;
    ensures \result == 0 || \result == -1;
*/
int run_journal_list(run_journal_t* j, uint32_t limit, uint64_t before_us,
                     const char* pipeline, const char* input_hash,
                     int success_filter,
                     run_journal_row_t** out, size_t* n_out);

/*@ requires rows == \null || \valid(rows + (0 .. n - 1));
    assigns *(rows + (0 .. n - 1));
*/
void run_journal_rows_free(run_journal_row_t* rows, size_t n);

// Read produced_hashes for a single row — used by REHYDRATE_RUN.
// Looked up by `journal_id` (the row's stable PK), not the runner's
// session-scoped run_id which collides on processor restart.
// Returns 0 on success (even if the row has no produced artifacts; n_out=0).
// Returns -1 if `journal_id` is not in the journal.
// names_out and hashes_out are parallel arrays of n_out malloc'd strings.
// Caller frees with run_journal_produced_free.
/*@ requires j == \null || \valid(j);
    requires names_out == \null || \valid(names_out);
    requires hashes_out == \null || \valid(hashes_out);
    requires n_out == \null || \valid(n_out);
    assigns *names_out, *hashes_out, *n_out;
    ensures \result == 0 || \result == -1;
*/
int run_journal_get_produced(run_journal_t* j, int64_t journal_id,
                             char*** names_out, char*** hashes_out, size_t* n_out);

/*@ requires names == \null || \valid(names + (0 .. n - 1));
    requires hashes == \null || \valid(hashes + (0 .. n - 1));
    assigns *(names + (0 .. n - 1)), *(hashes + (0 .. n - 1));
*/
void run_journal_produced_free(char** names, char** hashes, size_t n);

#ifdef __cplusplus
}
#endif
#endif
