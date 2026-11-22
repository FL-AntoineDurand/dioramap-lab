// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#ifndef V3_STEPS_ORTHO_H
#define V3_STEPS_ORTHO_H

#include "v3/step.h"

extern const step_def_t build_ortho_rgb_rasters_def;
extern const step_def_t build_ortho_irc_rasters_def;

int ortho_register_steps(void);

// Channel order for the `ortho-rgb-rasters` artifact (build-ortho-rgb-rasters
// output). 7 channels in this fixed order.
typedef enum ortho_rgb_channel {
    ORGB_R = 0,
    ORGB_G = 1,
    ORGB_B = 2,
    ORGB_LUMINANCE = 3,
    ORGB_SATURATION = 4,
    ORGB_GREENNESS = 5,
    ORGB_LUM_STDDEV = 6,
    ORGB__COUNT = 7,
} ortho_rgb_channel_t;

// Channel order for the `ortho-irc-rasters` artifact (build-ortho-irc-rasters
// output). 2 channels.
typedef enum ortho_irc_channel {
    OIRC_NIR = 0,
    OIRC_NDVI = 1,
    OIRC__COUNT = 2,
} ortho_irc_channel_t;

#endif
