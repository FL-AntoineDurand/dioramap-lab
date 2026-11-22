// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Minimal runner harness — heavy lifting requires a pipeline + store + im
// graph, which would need stubs of the underlying world model. Eva will
// mostly see the public-API guard clauses.

#include "v3/runner.h"

int main(void) {
    runner_t* r = runner_new(NULL);  // NULL cfg
    (void)r;
    runner_free(NULL);
    return 0;
}
