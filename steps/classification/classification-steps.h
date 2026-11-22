// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#ifndef V3_STEPS_CLASSIFICATION_H
#define V3_STEPS_CLASSIFICATION_H

#include "v3/step.h"

extern const step_def_t build_tile_ndvi_def;
extern const step_def_t compute_normal_scatter_def;
extern const step_def_t compute_near_building_mask_def;
extern const step_def_t classify_vegetation_def;

int classification_register_steps(void);

#endif
