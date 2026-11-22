// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#include "shadow-steps.h"

int shadow_register_steps(void) {
    if (step_register(&detect_photo_shadows_def) != 0) return -1;
    if (step_register(&cast_shadows_from_dsm_def) != 0) return -1;
    if (step_register(&find_best_sun_time_def) != 0) return -1;
    if (step_register(&deshadow_ortho_rgb_def) != 0) return -1;
    return 0;
}
