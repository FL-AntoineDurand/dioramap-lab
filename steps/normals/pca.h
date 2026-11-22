// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// PCA (Principal Component Analysis) for symmetric 3×3 matrices.
// Closed-form Cardano trigonometric eigenvalue method (Kopp 2008) +
// null-space eigenvector via row cross-products.
//
// Direct C port of bench-v2 lib/geometry/pca.js. Same numerical formulas
// and degeneracy handling — used for k-NN PCA normal estimation.

#ifndef V3_STEPS_NORMALS_PCA_H
#define V3_STEPS_NORMALS_PCA_H

#ifdef __cplusplus
extern "C" {
#endif

// Eigenvalues of a symmetric 3×3 matrix (row-major, only [0],[1],[2],[4],[5],[8]
// consulted; m[3], m[6], m[7] expected to mirror the upper triangle).
// Output sorted descending: out[0] ≥ out[1] ≥ out[2].
void pca_cardano_eigenvalues(const double m[9], double out[3]);

// Eigenvector for a given eigenvalue. Returns 0 on success and writes a
// unit-length vector to `out`; returns -1 if all three row cross-products
// are degenerate (eigenvalue multiplicity ≥ 2 along this column).
int pca_eigenvector_for_eigenvalue(const double m[9], double lambda, double out[3]);

// Smallest-eigenvalue eigenvector (= surface normal for a covariance matrix).
// Handles eigenvalue multiplicity by falling back to a Gram-Schmidt
// perpendicular-to-largest-eigenvector. Returns 0 on success, -1 if the
// matrix is fully spherical (no meaningful normal).
int pca_smallest_eigenvector(const double cov[9], double out[3]);

#ifdef __cplusplus
}
#endif

#endif
