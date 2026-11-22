// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Frama-C audit harness for `dioramap_mmap_file`.

#include "v3/mmap_file.h"
#include <__fc_builtin.h>

int main(void) {
    mmap_file_t f;
    (void)mmap_file_open("/tmp/audit-mmap-test", &f);
    mmap_file_close(&f);

    // NULL out — documented to return -1.
    (void)mmap_file_open("/tmp/x", (mmap_file_t*)0);

    // NULL path — should return -1, not crash.
    (void)mmap_file_open((const char*)0, &f);

    // close on a never-opened struct (defensive — caller's responsibility).
    mmap_file_t g = {0, 0, -1, 0};
    mmap_file_close(&g);

    // close on NULL.
    mmap_file_close((mmap_file_t*)0);
    return 0;
}
