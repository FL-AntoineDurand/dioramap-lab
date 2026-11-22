// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#define _USE_MATH_DEFINES
#include "pca.h"

#include <math.h>
#include <stdlib.h>

// Pi to enough digits for double; avoids relying on the non-C11 M_PI macro.
static const double TWO_PI_OVER_3 = 2.0 * 3.14159265358979323846 / 3.0;

static int cmp_double_desc(const void* a, const void* b) {
    double da = *(const double*)a;
    double db = *(const double*)b;
    if (db < da) {
        return -1;
    }
    if (db > da) {
        return 1;
    }
    return 0;
}

void pca_cardano_eigenvalues(const double m[9], double out[3]) {
    double a = m[0], b = m[1], c = m[2];
    double d = m[4], e = m[5];
    double f = m[8];

    double off_diag_sq = b * b + c * c + e * e;
    if (off_diag_sq < 1e-30) {
        out[0] = a;
        out[1] = d;
        out[2] = f;
        qsort(out, 3, sizeof(double), cmp_double_desc);
        return;
    }

    double q = (a + d + f) / 3.0;
    double da = a - q, dd = d - q, df = f - q;
    double p2 = (da * da + dd * dd + df * df + 2.0 * off_diag_sq) / 6.0;
    double p = sqrt(p2);

    if (p < 1e-15) {
        out[0] = q;
        out[1] = q;
        out[2] = q;
        return;
    }

    double inv_p = 1.0 / p;
    double b00 = da * inv_p, b01 = b * inv_p, b02 = c * inv_p;
    double b11 = dd * inv_p, b12 = e * inv_p;
    double b22 = df * inv_p;

    double det_b = b00 * (b11 * b22 - b12 * b12)
                 - b01 * (b01 * b22 - b12 * b02)
                 + b02 * (b01 * b12 - b11 * b02);

    double r = det_b / 2.0;
    if (r < -1.0) {
        r = -1.0;
    } else if (r > 1.0) {
        r = 1.0;
    }
    double phi = acos(r) / 3.0;

    double l1 = q + 2.0 * p * cos(phi);
    double l3 = q + 2.0 * p * cos(phi + TWO_PI_OVER_3);
    double l2 = 3.0 * q - l1 - l3;

    out[0] = l1;
    out[1] = l2;
    out[2] = l3;
}

int pca_eigenvector_for_eigenvalue(const double m[9], double lambda, double out[3]) {
    // Rows of (A - λI). The matrix is symmetric so r0=row0, r1=row1, r2=row2.
    double r0x = m[0] - lambda, r0y = m[1],          r0z = m[2];
    double r1x = m[1],          r1y = m[4] - lambda, r1z = m[5];
    double r2x = m[2],          r2y = m[5],          r2z = m[8] - lambda;

    double bx = 0, by = 0, bz = 0;
    double best = 0.0;

    // r0 × r1
    double nx = r0y * r1z - r0z * r1y;
    double ny = r0z * r1x - r0x * r1z;
    double nz = r0x * r1y - r0y * r1x;
    double sq = nx * nx + ny * ny + nz * nz;
    if (sq > best) {
        best = sq;
        bx = nx;
        by = ny;
        bz = nz;
    }

    // r0 × r2
    nx = r0y * r2z - r0z * r2y;
    ny = r0z * r2x - r0x * r2z;
    nz = r0x * r2y - r0y * r2x;
    sq = nx * nx + ny * ny + nz * nz;
    if (sq > best) {
        best = sq;
        bx = nx;
        by = ny;
        bz = nz;
    }

    // r1 × r2
    nx = r1y * r2z - r1z * r2y;
    ny = r1z * r2x - r1x * r2z;
    nz = r1x * r2y - r1y * r2x;
    sq = nx * nx + ny * ny + nz * nz;
    if (sq > best) {
        best = sq;
        bx = nx;
        by = ny;
        bz = nz;
    }

    double len = sqrt(best);
    if (len < 1e-15) {
        return -1;
    }
    out[0] = bx / len;
    out[1] = by / len;
    out[2] = bz / len;
    return 0;
}

int pca_smallest_eigenvector(const double cov[9], double out[3]) {
    double evs[3];
    pca_cardano_eigenvalues(cov, evs);
    double l1 = evs[0], l2 = evs[1], l3 = evs[2];

    if (pca_eigenvector_for_eigenvalue(cov, l3, out) == 0) {
        return 0;
    }

    // Multiplicity at the smallest eigenvalue. Fall back to "any vector
    // perpendicular to the eigenvector of the largest eigenvalue".
    if (l1 - l2 < 1e-10 * fabs(l1) + 1e-30) {
        // Fully spherical — no meaningful normal.
        return -1;
    }
    double e1[3];
    if (pca_eigenvector_for_eigenvalue(cov, l1, e1) != 0) {
        return -1;
    }
    // Pick world axis least aligned to e1, project off e1.
    double ax = fabs(e1[0]), ay = fabs(e1[1]), az = fabs(e1[2]);
    double s[3] = {0, 0, 0};
    if (ax <= ay && ax <= az) {
        s[0] = 1.0;
    } else if (ay <= az) {
        s[1] = 1.0;
    } else {
        s[2] = 1.0;
    }
    double dot = s[0] * e1[0] + s[1] * e1[1] + s[2] * e1[2];
    double nx = s[0] - dot * e1[0];
    double ny = s[1] - dot * e1[1];
    double nz = s[2] - dot * e1[2];
    double len = sqrt(nx * nx + ny * ny + nz * nz);
    if (len < 1e-15) {
        return -1;
    }
    out[0] = nx / len;
    out[1] = ny / len;
    out[2] = nz / len;
    return 0;
}
