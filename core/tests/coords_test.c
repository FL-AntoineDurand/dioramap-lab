// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Sanity check for WGS84 ↔ Lambert-93 conversions. Uses well-known points
// with reference values cross-checked against IGN's calculation service.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "v3/coords.h"

static void check(int cond, const char* msg) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        exit(1);
    }
}

static int near(double a, double b, double tol) {
    return fabs(a - b) <= tol;
}

int main(void) {
    // Paris, Arc de Triomphe. L93 reference per PROJ 9.4 with the GRS80
    // datum: ≈ (648252.6, 6863984.0). Test tolerance ±1 m.
    double x, y;
    check(wgs84_to_l93(2.2945, 48.8738, &x, &y) == 0, "wgs84_to_l93");
    check(near(x, 648252.6, 1.0), "arc x");
    check(near(y, 6863984.0, 1.0), "arc y");

    // Roundtrip.
    double lon, lat;
    check(l93_to_wgs84(x, y, &lon, &lat) == 0, "l93_to_wgs84");
    check(near(lon, 2.2945, 1e-5), "roundtrip lon");
    check(near(lat, 48.8738, 1e-5), "roundtrip lat");

    printf("OK\n");
    return 0;
}
