// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Lambert Azimuthal Equal-Area Projection for unit normals.
// Direct port of bench-v2 lib/geometry/lambert.js (matches its numerical
// behaviour exactly so the byte-validation gate keeps working).
//
// Projects unit normals onto a 2D disc (radius √2) with equal-area
// preservation, then accumulates them into a 2D grid. After Gaussian
// smoothing, peaks in the grid identify dominant normal-orientation
// groups. Used by hemisphere-segment's S1.

#ifndef V3_STEPS_V7_LAMBERT_H
#define V3_STEPS_V7_LAMBERT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lambert_peak {
    double normal_x, normal_y, normal_z; // unit vector (canonical hemisphere)
    double tilt;       // degrees from up axis
    double azimuth;    // degrees compass bearing (0=N, CW)
    double weight_sum; // smoothed grid value at peak
    int32_t row, col;  // integer grid cell
} lambert_peak_t;

typedef struct lambert_grid {
    int32_t resolution;        // grid is N×N
    double R;                  // disc radius + smoothing margin
    double cell_size;          // 2R / resolution
    double* grid;              // F64 N×N density (row-major)
    uint8_t* disc_mask;        // 0/1, lazily built
    int16_t* labels;           // watershed labels (built on demand), -1 = unassigned
    int32_t up_axis;           // 0=x, 1=y, 2=z. v2 uses y-up; v3 LAZ pipeline z-up.
} lambert_grid_t;

// Construct a grid of given resolution. up_axis must be 0/1/2; 1 (y-up)
// preserves bench-v2 byte-identity for the bench harness, 2 (z-up) is
// what the LAZ-frame pipeline uses. Returns 0 on success.
//
// On success: g->resolution == resolution, 0 <= g->up_axis <= 2, and
// g->grid points to a calloc'd buffer of resolution*resolution doubles.
// We expose that as an explicit ensures clause so Eva can verify the
// downstream `g->grid + i` accesses (accumulate, smooth, find_peaks,
// and the `memcpy(out->lambert_grid, g->grid, sz)` in hemisphere-segment)
// without having to re-derive the relation from the body.
/*@ requires g == \null || \valid(g);
    assigns *g \from resolution, up_axis;
    assigns \result \from g, resolution, up_axis;
    ensures \result == 0 || \result == -1;
    ensures \result == 0 ==>
              g->resolution == resolution
           && 0 <= g->up_axis <= 2
           && g->grid != \null
           && \valid(g->grid + (0 .. (size_t)resolution * resolution - 1));
*/
int lambert_grid_init(lambert_grid_t* g, int32_t resolution, int32_t up_axis);

/*@ requires g == \null || \valid(g);
    assigns *g \from \nothing;
*/
void lambert_grid_free(lambert_grid_t* g);

// Project a single F32×3 unit normal onto the disc. Canonicalises to
// up-axis ≥ 0 hemisphere by flipping all 3 components if up < 0.
// Returns the (u, v) disc coords.
void lambert_project_typed(const float* normals_interleaved, size_t index,
                           int32_t up_axis, double* out_u, double* out_v);

// Accumulate weighted normals into the grid via bilinear splat.
//   normals_interleaved: F32×3N, interleaved (nx,ny,nz)
//   weights: F32×n_eligible, indexed by position in eligible_indices
//   eligible_indices: U32×n_eligible, global indices into normals_interleaved
//
// Without an `assigns` clause Eva defaults to "may write anything reachable
// through any parameter", which (transitively, via the global address
// space) invalidated the all_peaks tracking returned by find_peaks. We
// pin assigns to g's grid contents (the only thing this writes) so Eva
// preserves caller-side invariants across the call.
/*@ requires g == \null || \valid(g);
    assigns *g \from indirect:normals_interleaved, indirect:weights,
                     indirect:eligible_indices, indirect:n_eligible;
*/
void lambert_grid_accumulate(lambert_grid_t* g,
                             const float* normals_interleaved,
                             const float* weights,
                             const uint32_t* eligible_indices,
                             size_t n_eligible);

// Separable 2D Gaussian smoothing in place (sigma in grid cells).
// Cells outside the disc are zeroed.
/*@ requires g == \null || \valid(g);
    assigns *g \from indirect:sigma;
*/
void lambert_grid_gaussian_smooth(lambert_grid_t* g, double sigma);

// Find 8-neighbour local maxima above min_weight. Returns peaks sorted
// by weight_sum descending. Caller frees with `free()`.
//   *out_n_peaks: count of peaks returned
//   returns: malloc'd array of lambert_peak_t (NULL if 0 peaks)
//
// On a non-NULL result, the buffer has *out_n_peaks valid entries — we
// expose that so hemisphere-segment's `peaks[k] = all_peaks[pi]` (where
// `pi < n_all_peaks`) verifies without an explicit assertion.
/*@ requires g == \null || \valid_read(g);
    requires out_n_peaks == \null || \valid(out_n_peaks);
    assigns *out_n_peaks \from indirect:g, indirect:*g, indirect:min_weight;
    assigns \result \from indirect:g, indirect:*g, indirect:min_weight;
    ensures \result == \null
         || (out_n_peaks != \null
             && \valid(\result + (0 .. *out_n_peaks - 1)));
*/
lambert_peak_t* lambert_grid_find_peaks(const lambert_grid_t* g,
                                        double min_weight,
                                        size_t* out_n_peaks);

// Build watershed labels (steepest-ascent) for every cell in the disc.
// Labels are stored in g->labels (allocated lazily) — peak-index for
// each cell, -1 outside disc. all_peaks = the peaks `find_peaks(0)`
// returns; n_all_peaks = the count.
//
// `const lambert_peak_t* all_peaks` is read-only; the assigns clause is
// scoped to *g (specifically g->labels) so Eva doesn't widen and
// invalidate the caller's all_peaks tracking.
/*@ requires g == \null || \valid(g);
    requires n_all_peaks == 0 || \valid_read(all_peaks + (0 .. n_all_peaks - 1));
    assigns *g \from indirect:all_peaks, indirect:n_all_peaks;
    assigns \result \from indirect:g, indirect:all_peaks, indirect:n_all_peaks;
    ensures \result == 0 || \result == -1;
*/
int lambert_grid_build_watershed_labels(lambert_grid_t* g,
                                         const lambert_peak_t* all_peaks,
                                         size_t n_all_peaks);

// Assign each eligible point to a peak via watershed lookup at its
// projected (u,v). Output is Int16 (length n_eligible), value = peak
// index in `all_peaks`, or -1 if outside disc / unassigned.
// Caller-allocated out_assignment.
/*@ requires g == \null || \valid_read(g);
    requires n_eligible == 0 || \valid(out_assignment + (0 .. n_eligible - 1));
    assigns out_assignment[0 .. n_eligible - 1]
        \from indirect:g, indirect:normals_interleaved,
              indirect:eligible_indices, indirect:n_eligible;
*/
void lambert_grid_assign_watershed(const lambert_grid_t* g,
                                    const float* normals_interleaved,
                                    const uint32_t* eligible_indices,
                                    size_t n_eligible,
                                    int16_t* out_assignment);

#ifdef __cplusplus
}
#endif
#endif
