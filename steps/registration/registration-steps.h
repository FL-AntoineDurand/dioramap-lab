// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#ifndef V3_STEPS_REGISTRATION_H
#define V3_STEPS_REGISTRATION_H

#include "v3/step.h"

extern const step_def_t build_lidar_rasters_def;
extern const step_def_t compute_ndsm_def;
extern const step_def_t compute_building_mask_def;
extern const step_def_t coregister_ortho_lidar_def;
extern const step_def_t rasterize_cadastre_edges_def;
extern const step_def_t fit_cadastre_to_lidar_def;

int registration_register_steps(void);

// Channel order in the `lidar-rasters` artifact (build-lidar-rasters output).
// Indices into raster_view_t.channels[].
typedef enum lidar_raster_channel {
    LIDAR_RC_MAX_Z = 0,
    LIDAR_RC_MIN_Z = 1,
    LIDAR_RC_MEAN_Z = 2,
    LIDAR_RC_DENSITY = 3,
    LIDAR_RC_INTENSITY = 4,
    LIDAR_RC_ROUGHNESS = 5,
    LIDAR_RC_MULTI_RETURN = 6,
    LIDAR_RC__COUNT = 7,
} lidar_raster_channel_t;

#endif
