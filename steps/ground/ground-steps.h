// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#ifndef V3_STEPS_GROUND_H
#define V3_STEPS_GROUND_H

#include "v3/step.h"

extern const step_def_t build_lidar_ground_def;
extern const step_def_t build_parcel_lines_def;

int ground_register_steps(void);

#endif
