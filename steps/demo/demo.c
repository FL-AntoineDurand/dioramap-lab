// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#include "demo.h"

int demo_register_steps(void) {
    if (step_register(&demo_scale_by_factor_def) != 0)
        return -1;
    if (step_register(&demo_add_bias_def) != 0)
        return -1;
    if (step_register(&demo_multiply_def) != 0)
        return -1;
    if (step_register(&demo_report_def) != 0)
        return -1;
    if (step_register(&demo_split_range_def) != 0)
        return -1;
    if (step_register(&demo_fan_process_def) != 0)
        return -1;
    if (step_register(&demo_reduce_sum_def) != 0)
        return -1;
    if (step_register(&demo_cube_memoized_def) != 0)
        return -1;
    if (step_register(&demo_stream_def) != 0)
        return -1;
    return 0;
}
