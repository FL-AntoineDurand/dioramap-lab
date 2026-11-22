// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#ifndef V3_STEPS_COORDS_H
#define V3_STEPS_COORDS_H

#include "v3/step.h"

extern const step_def_t canonicalize_location_def;
extern const step_def_t compute_scene_origin_def;

int coord_steps_register(void);

#endif
