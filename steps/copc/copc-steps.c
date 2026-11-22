// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#include "copc-steps.h"

int copc_steps_register(void) {
    if (step_register(&find_copc_tiles_def) != 0)
        return -1;
    if (step_register(&fetch_copc_tiles_def) != 0)
        return -1;
    if (step_register(&laz_extract_disc_def) != 0)
        return -1;
    return 0;
}
