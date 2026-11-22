// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#ifndef V3_STEPS_DEMO_H
#define V3_STEPS_DEMO_H

#include "v3/artifact.h"
#include "v3/step.h"

// Public step defs.
extern const step_def_t demo_scale_by_factor_def;
extern const step_def_t demo_add_bias_def;
extern const step_def_t demo_multiply_def;
extern const step_def_t demo_report_def;
// T13 array-artifact test steps.
extern const step_def_t demo_split_range_def;
extern const step_def_t demo_fan_process_def;
extern const step_def_t demo_reduce_sum_def;
// B3.5 sub-scope cached_compute demo.
extern const step_def_t demo_cube_memoized_def;
// B9 streaming primitive demo (PLAN-VIEWER-PORT §7).
extern const step_def_t demo_stream_def;

// Register all demo steps with the global registry.
// Returns 0 on success, -1 on any registration failure.
int demo_register_steps(void);

#endif
