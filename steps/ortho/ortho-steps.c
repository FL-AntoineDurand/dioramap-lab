// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#include "ortho-steps.h"

int ortho_register_steps(void) {
    if (step_register(&build_ortho_rgb_rasters_def) != 0) {
        return -1;
    }
    if (step_register(&build_ortho_irc_rasters_def) != 0) {
        return -1;
    }
    return 0;
}
