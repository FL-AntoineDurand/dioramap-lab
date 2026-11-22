// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#ifndef V3_STEPS_SHADOW_H
#define V3_STEPS_SHADOW_H

#include "v3/step.h"

extern const step_def_t detect_photo_shadows_def;
extern const step_def_t cast_shadows_from_dsm_def;
extern const step_def_t find_best_sun_time_def;
extern const step_def_t deshadow_ortho_rgb_def;

int shadow_register_steps(void);

#endif
