// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#include "coord-steps.h"

int coord_steps_register(void) {
    if (step_register(&canonicalize_location_def) != 0)
        return -1;
    if (step_register(&compute_scene_origin_def) != 0)
        return -1;
    return 0;
}
