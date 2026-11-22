---
id: "2026-03-12-cardano-eigensolver"
date: 2026-03-12
title: "Closed-form Cardano eigensolver replaces power iteration PCA"
tags: [pca, eigenvalues, eigensolver, cardano, numerical-stability, bugfix]
category: bugfix
status: concluded
refs: ["2026-03-12-tile-signals-unified-pca", "2026-03-12-spherical-kde-scan-line-compensation"]
---

## Problem

The PCA eigensolver (`lib/geometry/pca.js`) used power iteration with a
fixed initial guess `(1,0,0)` and double deflation to find eigenvalues and
the plane normal.  Testing against 10 diverse covariance matrices revealed:

- **6/10 returned NULL**: diagonal matrices, flat surfaces (λ3=0), near-isotropic
  (λ1≈λ2≈λ3), tiny eigenvalues — all perfectly valid inputs.
- **2/10 returned WRONG eigenvalue ordering**: tilted planes produced
  `eigenvalues[2] > eigenvalues[1]`, violating the descending-sort contract.
  Downstream `planarity = (l2-l3)/l1` computed negative values; `RMS = sqrt(l3)`
  was 200× too large for steep planes.
- **Only 2/10 fully correct**: matrices where all eigenvectors had non-zero
  X-components (the only case where the fixed `(1,0,0)` initial guess works).

### Root causes

1. **Fixed initial guess**: power iteration starts from `(1,0,0)`.  When the
   dominant eigenvector is orthogonal to X (e.g. `(0,1,0)` for vertical lines),
   the initial vector has zero projection — convergence fails.
2. **Deflation error**: after subtracting λ1·e1·e1ᵀ, any error in e1 propagates.
   The second power iteration then starts from `(1,0,0)` again — if this lies in
   the deflated null space, it returns the unmodified initial guess.
3. **Eigenvalue misordering**: eigenvalues were returned as `[first-found,
   second-found, trace-remainder]` which is only correct when power iteration
   happens to find them in descending order.
4. **Rank deficiency**: for perfectly planar inputs (λ3=0), the deflated matrix
   is near-zero → power iteration breaks at `len < 1e-12` → `cross(e1, e2) ≈ 0`
   → returns null.

### Impact on pipeline

All 20+ steps that call `normalFromCovariance` or `fitPlane` were affected:
- tile-signals: wrong per-point signals (planarity, linearity, curvatures)
- RANSAC/histogram/cascade seeding: wasted plane hypotheses
- strip-region-grow: legitimate regions dropped
- fitterFit (competitive-grow): growth stopped prematurely
- fitPlane RMS: wrong residuals for steep planes

## Solution: Cardano trigonometric eigensolver (Kopp 2008)

Replaced the power iteration + deflation internals with a closed-form solver:

1. **Eigenvalues** via Cardano's trigonometric method for the characteristic cubic
   of a symmetric 3×3 matrix.  Uses the Kopp shift-and-scale formulation:
   shift by trace/3, scale by p, compute `r = det(B)/2`, then three roots from
   `acos(r)/3 + k·2π/3`.  Always returns 3 real roots, sorted descending.

2. **Eigenvectors** via null-space cross products: for eigenvalue λ, form
   `B = A - λI`, compute cross products of all 3 row pairs, pick the one with
   largest magnitude.  This is stable for rank-2 matrices (the common case).

3. **Multiplicity-2 fallback**: when λ2 = λ3 (rank-1 null space, e.g. collinear
   points), the cross-product approach fails.  Falls back to finding e1 (the
   unique eigenvector of λ1) and picking a perpendicular via Gram-Schmidt.

4. **Triple degeneracy**: λ1 = λ2 = λ3 (spherical) → returns null (no meaningful
   normal).  This is the ONLY case that returns null now.

### Properties

- **Deterministic**: no iteration, no convergence concerns, no initial guess
- **Exact**: eigenvalues match analytical values to floating-point epsilon
- **Sorted**: eigenvalues always returned `l1 ≥ l2 ≥ l3`
- **Faster**: ~30 arithmetic ops + 1 acos + 2 cos, vs 100 matrix-vector products
- **All degeneracies handled**: rank 0, 1, 2, 3; equal eigenvalues; tiny/large magnitudes

## Test results

43 tests across 8 suites:
- cardanoEigenvalues: 14 tests (diagonal, degenerate, constructed eigenpairs, trace preservation)
- eigenvectorForEigenvalue: 4 tests
- normalFromCovariance geometric: 10 tests (flat, tilted, vertical, walls, lines)
- Previously-degenerate cases: 5 tests (was NULL, now correct)
- Previously-wrong ordering: 2 tests (eigenvalues now sorted correctly)
- Regression vs old solver: 2 tests (roof-like eigenvalues match within 3e-11)
- fitPlane end-to-end: 4 tests
- fitterFit incremental: 2 tests

All 142 tests pass (43 PCA + 22 tile-signals + 16 steps + 33 cache + 28 context).
