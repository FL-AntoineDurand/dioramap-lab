// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Frama-C audit harness shim for v7 bench files.
//
// Each v7-*-bench.c declares `int main(int argc, char** argv)` and expects
// exactly two CLI args (refs-dir, out-dir). Under Frama-C with -main main,
// Eva starts with argc/argv totally unconstrained — the bench's
// `if (argc != 3) return 2;` is taken on every abstract path and Eva never
// enters the interesting code below.
//
// The runner compiles the bench with `-D main=audit_bench_main` so the
// bench's symbol becomes `audit_bench_main`. This shim provides the real
// `main` and calls into it with a synthesized argv of two known-valid
// paths. The paths themselves don't matter to Eva — the bench's `slurp()`
// calls fopen, which Frama-C's libc spec already returns abstractly
// (NULL or valid FILE*).

#include <stddef.h>

extern int audit_bench_main(int argc, char** argv);

int main(void) {
    // Two non-NULL, NUL-terminated dummy path strings. Static so they
    // live for the duration of the analysis; Eva sees them as valid
    // read strings without needing further annotation.
    static char refs[] = "/tmp/audit-refs";
    static char out[]  = "/tmp/audit-out";
    static char prog[] = "bench";
    static char* argv[4] = { prog, refs, out, (char*)0 };
    return audit_bench_main(3, argv);
}
