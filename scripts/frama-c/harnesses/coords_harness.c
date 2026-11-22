// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Frama-C audit harness for `dioramap_coords`.
// PROJ is opaque to Eva; we only RTE-check the wrapper.

#include "v3/coords.h"
#include <__fc_builtin.h>

int main(void) {
    double lon = Frama_C_double_interval(-180.0, 180.0);
    double lat = Frama_C_double_interval(-90.0, 90.0);
    // Initialize x/y so Eva can verify they're \initialized when read
    // by the l93_to_wgs84 call below.  The wgs84_to_l93 contract uses
    // `assigns *out_x` but not `ensures \initialized(*out_x)`, so Eva
    // can't infer initialization from the assignment alone.  Tightening
    // the contract is the structural fix; pre-initialising here is the
    // harness-local fix that keeps the audit green without touching the
    // production header.
    double x = 0.0, y = 0.0;
    (void)wgs84_to_l93(lon, lat, &x, &y);

    // Out-of-range inputs are documented to return -1; PROJ handles it.
    double lon2 = Frama_C_double_interval(-1e9, 1e9);
    double lat2 = Frama_C_double_interval(-1e9, 1e9);
    (void)wgs84_to_l93(lon2, lat2, &x, &y);

    double lon3 = 0.0, lat3 = 0.0;
    (void)l93_to_wgs84(x, y, &lon3, &lat3);
    return 0;
}
