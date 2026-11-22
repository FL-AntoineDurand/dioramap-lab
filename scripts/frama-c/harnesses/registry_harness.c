// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Frama-C audit harness for step registry (g_defs[256] global).
#include "v3/step.h"
#include <__fc_builtin.h>

static int dummy_exec(void* ctx) { (void)ctx; return 0; }

int main(void) {
    step_def_t def = {.name = "s1", .exec = dummy_exec};
    (void)step_register(&def);
    (void)step_register(&def);   // dup
    (void)step_register(NULL);
    (void)step_lookup("s1");
    (void)step_lookup(NULL);
    (void)step_lookup("nope");
    (void)step_registry_size();
    step_registry_reset();
    return 0;
}
