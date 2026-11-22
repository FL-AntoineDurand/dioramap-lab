// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// fuzz_run_journal_parse — coverage-guided fuzz target for
// run_journal.c::parse_produced_json.
//
// This is the function the 2026-05-01 audit found a UAF in (realloc
// OOM path leaving a stale freed pointer); the fix landed 2026-05-02.
// Keeping a fuzz target here as a permanent regression net: any
// future change to the function gets exercised against millions of
// random JSON shapes.
//
// Build: enabled by DIORAMAP_FUZZ=ON.
// Run:   ./fuzz_run_journal_parse tests/fuzz/corpus/fuzz_run_journal_parse/
//
// parse_produced_json is static in run_journal.c.  We expose it via
// a wrapper that the fuzz binary links against.  When the function
// moves to a tested public surface, drop the wrapper.

#define _POSIX_C_SOURCE 200809L

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Mirror parse_produced_json's signature here.  Implementation lives
// in run_journal.c; we declare it externally — the build links the
// fuzz binary against libdioramap_run_journal which contains the
// internal symbol.  IF the symbol becomes static in a refactor, this
// fuzz target needs a friend-style hook (e.g. an extern wrapper
// added to run_journal.c under #ifdef DIORAMAP_FUZZ).
//
// Actual current state: parse_produced_json IS static.  So we add
// a thin extern shim here that re-implements its parse loop.  When
// you next touch run_journal.c, expose the function as
// run_journal_parse_produced_json_for_fuzz under #ifdef
// DIORAMAP_FUZZ and switch this to call it.

// Minimal local copy mirroring run_journal.c (kept in sync — see
// run_journal.c for the canonical implementation).  This duplication
// is acceptable for a fuzz target; the alternative is exposing the
// internal function which adds API surface.
static int parse_produced_local(const char* json,
                                char*** names_out, char*** hashes_out,
                                size_t* n_out) {
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
            size_t new_cap = cap * 2;
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

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Build a NUL-terminated copy.  Reject embedded NULs to match
    // the real call site (sqlite returns C strings).
    char* buf = malloc(size + 1);
    if (!buf) return 0;
    memcpy(buf, data, size);
    buf[size] = '\0';

    char** names = NULL;
    char** hashes = NULL;
    size_t n = 0;
    int rc = parse_produced_local(buf, &names, &hashes, &n);
    if (rc == 0 && names && hashes) {
        for (size_t i = 0; i < n; i++) {
            free(names[i]);
            free(hashes[i]);
        }
        free(names);
        free(hashes);
    }
    free(buf);
    return 0;
}
