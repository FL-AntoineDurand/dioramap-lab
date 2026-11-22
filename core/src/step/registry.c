// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Global step registry.
//
// Fixed-capacity array; registration happens at startup before any runner
// starts, so thread-safety is not required here. If registration ever needs
// to be concurrent, add a mutex.

#include <string.h>

#include "v3/step.h"

#define STEP_REGISTRY_CAP 256

static const step_def_t* g_defs[STEP_REGISTRY_CAP];
static size_t g_count;

int step_register(const step_def_t* def) {
    if (!def || !def->name || !*def->name || !def->exec) {
        return -1;
    }
    for (size_t i = 0; i < g_count; i++) {
        if (strcmp(g_defs[i]->name, def->name) == 0) {
            return -1; // duplicate
        }
    }
    if (g_count >= STEP_REGISTRY_CAP) {
        return -1;
    }
    // Reject steps that write the reserved pipeline-input name.
    for (size_t w = 0; w < def->n_writes; w++) {
        if (strcmp(def->writes[w].name, PIPELINE_INPUT_ARTIFACT) == 0) {
            return -1;
        }
    }
    g_defs[g_count++] = def;
    return 0;
}

const step_def_t* step_lookup(const char* name) {
    if (!name) {
        return NULL;
    }
    for (size_t i = 0; i < g_count; i++) {
        if (strcmp(g_defs[i]->name, name) == 0) {
            return g_defs[i];
        }
    }
    return NULL;
}

void step_registry_reset(void) {
    g_count = 0;
    memset(g_defs, 0, sizeof(g_defs));
}

size_t step_registry_size(void) {
    return g_count;
}
