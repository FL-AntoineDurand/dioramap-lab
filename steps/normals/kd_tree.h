// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// 3D k-d tree for k-NN queries. Median-split, axis cycle (0/1/2), recursive
// build. Operates on caller-supplied `positions` (interleaved x,y,z doubles)
// — the tree borrows the pointer; lifetime is the caller's responsibility.
//
// This is the v3 port of bench-v2 lib/geometry/kd-tree.js. Same algorithm,
// same numerical behaviour: build sorts by axis coord and picks the median
// element at each level; kNN traversal descends toward the query first,
// then prunes the other side by squared split distance.

#ifndef V3_STEPS_NORMALS_KD_TREE_H
#define V3_STEPS_NORMALS_KD_TREE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kd_node {
    int32_t point_idx;       // index into positions[] (range 0..n_points-1)
    int32_t left;            // child node index, -1 for leaf
    int32_t right;           // child node index, -1 for leaf
    uint8_t axis;            // 0=x, 1=y, 2=z
    double split_val;        // positions[point_idx*3 + axis], cached for prune
} kd_node_t;

typedef struct kd_tree {
    const double* positions; // borrowed, interleaved x,y,z
    size_t n_points;         // length of `nodes` (and number of points in tree)
    kd_node_t* nodes;        // n_points entries
    int32_t root;            // -1 if empty
    bool nodes_owned;        // true: kd_free() releases nodes; false: borrowed
                             // (zero-copy from a deserialised artifact)
} kd_tree_t;

// Build a tree over all points in `positions` (length 3*n_points).
// Returns 0 on success, -1 on alloc failure.
int kd_build(kd_tree_t* out, const double* positions, size_t n_points);

// Serialise the node array as a flat byte buffer (n_points × sizeof(kd_node_t)).
// Caller frees `*out_bytes` with `free()`. Returns 0 on success.
int kd_serialize(const kd_tree_t* tree, uint8_t** out_bytes, size_t* out_len);

// Deserialise a tree from a node-array byte buffer (zero-copy: `out->nodes`
// points into `nodes_bytes`, which must outlive `*out`). Sets
// `out->nodes_owned = false` so kd_free() does not release the borrowed
// memory. The `positions` pointer is borrowed too — its lifetime must
// match the tree's.
//
// `nodes_len` must equal `n_points × sizeof(kd_node_t)`. Returns 0 on
// success, -1 on size mismatch or NULL inputs.
int kd_deserialize(kd_tree_t* out, const double* positions, size_t n_points,
                   const uint8_t* nodes_bytes, size_t nodes_len);

void kd_free(kd_tree_t* t);

typedef struct kd_neighbor {
    int32_t idx;
    double dist_sq;
} kd_neighbor_t;

// Find the k nearest neighbors of `query` (length-3 doubles).
// `out` is caller-allocated, capacity at least k.
// Returns the actual neighbor count via *out_count (≤ k; may be less if
// the tree has fewer than k points). Result is sorted ascending by dist_sq.
// Returns 0 on success, -1 on bad args.
int kd_knn(const kd_tree_t* t, const double query[3], int k,
           kd_neighbor_t* out, int* out_count);

// Find all points within `radius` of `query` (length-3 doubles).
// Caller passes `*cap` = capacity of `out`; on return `*out_count`
// is the actual number of neighbours found. If `*out_count > *cap`,
// only the first `*cap` neighbours are written but the count
// reflects the true total (caller can detect and re-call with a
// larger buffer).
//
// Visit order: pre-order DFS, near-side-first (matches bench-v2
// `radiusSearch` exactly so any algorithm relying on neighbour
// iteration order — grow-refit-merge's diffusion-limited grow,
// boundary scan, etc. — is byte-id'l). Returns 0 on success.
int kd_radius_search(const kd_tree_t* t, const double query[3], double radius,
                     kd_neighbor_t* out, int cap, int* out_count);

#ifdef __cplusplus
}
#endif

#endif
