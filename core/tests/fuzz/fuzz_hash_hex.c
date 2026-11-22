// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// fuzz_hash_hex — coverage-guided fuzz target for hash_from_hex64.
//
// hash_from_hex64 lives in the WS server's REHYDRATE path (parses
// hex hashes from the run journal).  Small fixed-size parser; not
// likely to harbor bugs, but a 30-second fuzz session is cheap
// regression insurance.
//
// Build: enabled by DIORAMAP_FUZZ=ON (clang only).
// Run:   ./fuzz_hash_hex tests/fuzz/corpus/fuzz_hash_hex/
// Crash: a crashing input is saved as crash-<sha256>; reproduce with
//        ./fuzz_hash_hex <crashfile>.
//
// The function under test is static in server.c; we declare a local
// copy here.  When the function moves to a public surface (e.g. a
// hash util), drop the local copy and #include the real header.

#include <stddef.h>
#include <stdint.h>

#include "v3/hash.h"

// Local copy of hash_from_hex64 from core/src/server/server.c.  Keep
// in sync until the function is exported.
static int hash_from_hex64(const char* in, hash_t* out) {
    if (!in) return -1;
    size_t n = 0;
    while (in[n] && n < 65) n++;
    if (n != 64) return -1;
    for (size_t i = 0; i < 32; i++) {
        char hi = in[2*i], lo = in[2*i + 1];
        int hv = (hi >= '0' && hi <= '9') ? hi - '0'
               : (hi >= 'a' && hi <= 'f') ? 10 + (hi - 'a')
               : (hi >= 'A' && hi <= 'F') ? 10 + (hi - 'A') : -1;
        int lv = (lo >= '0' && lo <= '9') ? lo - '0'
               : (lo >= 'a' && lo <= 'f') ? 10 + (lo - 'a')
               : (lo >= 'A' && lo <= 'F') ? 10 + (lo - 'A') : -1;
        if (hv < 0 || lv < 0) return -1;
        out->bytes[i] = (uint8_t)((hv << 4) | lv);
    }
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // hash_from_hex64 expects a NUL-terminated string.  Build one
    // from `data` without overflowing.
    char buf[128];
    size_t n = size < sizeof(buf) - 1 ? size : sizeof(buf) - 1;
    for (size_t i = 0; i < n; i++) {
        // Reject embedded NULs by truncating early; the function
        // walks via strlen-style termination.
        if (data[i] == 0) { n = i; break; }
        buf[i] = (char)data[i];
    }
    buf[n] = '\0';

    hash_t h;
    (void)hash_from_hex64(buf, &h);
    return 0;
}
