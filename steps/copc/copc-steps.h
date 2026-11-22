// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#ifndef V3_STEPS_COPC_H
#define V3_STEPS_COPC_H

#include "v3/step.h"

extern const step_def_t find_copc_tiles_def;
extern const step_def_t fetch_copc_tiles_def;
extern const step_def_t laz_extract_disc_def;

int copc_steps_register(void);

#endif
