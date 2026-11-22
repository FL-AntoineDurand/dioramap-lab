// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#include "buildings-steps.h"

int buildings_register_steps(void) {
    if (step_register(&extract_buildings_def) != 0) return -1;
    return 0;
}
