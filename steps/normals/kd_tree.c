// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#include "kd_tree.h"
#include "v3/safe_alloc.h"

#include <stdlib.h>
#include <string.h>

// ----------------------------------------------------------------------------
// Build: median-split with full sort by axis coordinate, recursive.
// Matches bench-v2 lib/geometry/kd-tree.js behaviour exactly: at each node
// the indices are sorted by the axis coord, and the index at len/2 is picked.
// ----------------------------------------------------------------------------

typedef struct build_ctx {
    const double* positions;
    kd_node_t* nodes;
    int32_t next_node;       // next free slot in nodes[]
} build_ctx_t;

// Per-call sort key: cache the axis coordinate so qsort doesn't follow
// pointers through positions[] for every comparison.
typedef struct sort_entry {
    int32_t idx;
    double key;
} sort_entry_t;

static int cmp_sort_entry(const void* a, const void* b) {
    double ka = ((const sort_entry_t*)a)->key;
    double kb = ((const sort_entry_t*)b)->key;
    if (ka < kb) {
        return -1;
    }
    if (ka > kb) {
        return 1;
    }
    return 0;
}

// Frama-C 32+ requires an explicit `assigns` clause on recursive
// functions to interpret the recursive call without warning "Generating
// probably incomplete assigns to interpret the call" + aborting EVA.
// build_node mutates: ctx->next_node, ctx->nodes[] (writes new nodes),
// and reorders the indices[] slice it owns. It does not read or assign
// global state beyond what's reachable through ctx.
/*@ requires \valid(ctx);
    requires n == 0 || \valid(indices + (0 .. n - 1));
    assigns ctx->next_node, ctx->nodes[..], indices[0 .. n - 1]
        \from ctx->next_node, ctx->positions, indices[0 .. n - 1], n, depth;
*/
static int32_t build_node(build_ctx_t* ctx, int32_t* indices, size_t n, int depth) {
    if (n == 0) {
        return -1;
    }
    int32_t node_id = ctx->next_node++;
    kd_node_t* node = &ctx->nodes[node_id];
    node->axis = (uint8_t)(depth % 3);

    if (n == 1) {
        node->point_idx = indices[0];
        node->left = -1;
        node->right = -1;
        node->split_val = ctx->positions[indices[0] * 3 + node->axis];
        return node_id;
    }

    // Sort indices by axis coordinate.
    sort_entry_t* tmp = safe_malloc_array(n, sizeof(sort_entry_t));
    if (!tmp) {
        return -1;
    }
    int axis = node->axis;
    for (size_t i = 0; i < n; i++) {
        tmp[i].idx = indices[i];
        tmp[i].key = ctx->positions[(size_t)indices[i] * 3 + axis];
    }
    qsort(tmp, n, sizeof(sort_entry_t), cmp_sort_entry);
    for (size_t i = 0; i < n; i++) {
        indices[i] = tmp[i].idx;
    }
    free(tmp);

    size_t mid = n >> 1;
    node->point_idx = indices[mid];
    node->split_val = ctx->positions[(size_t)indices[mid] * 3 + axis];

    node->left = build_node(ctx, indices, mid, depth + 1);
    node->right = build_node(ctx, indices + mid + 1, n - mid - 1, depth + 1);
    return node_id;
}

int kd_build(kd_tree_t* out, const double* positions, size_t n_points) {
    if (!out || (!positions && n_points > 0)) {
        return -1;
    }
    // Frama-C audit finding NORM-1: indices are int32_t but `i` is size_t.
    // For n_points > INT32_MAX the (int32_t)i cast in the index-fill loop
    // wraps to negative and the later `(size_t)indices[i]` reads OOB.
    // Cap up-front so the loop is provably safe.
    if (n_points > INT32_MAX) {
        return -1;
    }
    out->positions = positions;
    out->n_points = n_points;
    out->nodes = NULL;
    out->root = -1;
    out->nodes_owned = false;
    if (n_points == 0) {
        return 0;
    }
    out->nodes = safe_calloc_array(n_points, sizeof(kd_node_t));
    if (!out->nodes) {
        return -1;
    }
    out->nodes_owned = true;
    // NORM-2: overflow guard on malloc. n_points ≤ INT32_MAX after the
    // cap above, so n_points * sizeof(int32_t) ≤ 2^31 * 4 = 2^33 — safely
    // within size_t on any platform Frama-C audits. Keep the guard for
    // defense-in-depth so future cap changes don't reintroduce the wrap.
    if (n_points > SIZE_MAX / sizeof(int32_t)) {
        free(out->nodes);
        out->nodes = NULL;
        out->nodes_owned = false;
        return -1;
    }
    int32_t* indices = safe_malloc_array(n_points, sizeof(int32_t));
    if (!indices) {
        free(out->nodes);
        out->nodes = NULL;
        out->nodes_owned = false;
        return -1;
    }
    for (size_t i = 0; i < n_points; i++) {
        indices[i] = (int32_t)i;
    }
    build_ctx_t ctx = {.positions = positions, .nodes = out->nodes, .next_node = 0};
    out->root = build_node(&ctx, indices, n_points, 0);
    free(indices);
    return out->root >= 0 ? 0 : -1;
}

// ----------------------------------------------------------------------------
// Serialise / deserialise. The node array is a flat POD blob (no pointers),
// so we just memcpy out / point in. The root is always 0 for a non-empty tree
// (build_node writes the root first); no separate root field needs to ride
// along in the artifact.
// ----------------------------------------------------------------------------

int kd_serialize(const kd_tree_t* tree, uint8_t** out_bytes, size_t* out_len) {
    if (!tree || !out_bytes || !out_len) {
        return -1;
    }
    if (tree->n_points == 0 || !tree->nodes) {
        *out_bytes = NULL;
        *out_len = 0;
        return 0;
    }
    size_t bytes = tree->n_points * sizeof(kd_node_t);
    uint8_t* buf = malloc(bytes);
    if (!buf) {
        return -1;
    }
    memcpy(buf, tree->nodes, bytes);
    *out_bytes = buf;
    *out_len = bytes;
    return 0;
}

int kd_deserialize(kd_tree_t* out, const double* positions, size_t n_points,
                   const uint8_t* nodes_bytes, size_t nodes_len) {
    if (!out) {
        return -1;
    }
    out->positions = positions;
    out->n_points = n_points;
    out->nodes = NULL;
    out->root = -1;
    out->nodes_owned = false;
    if (n_points == 0) {
        return (nodes_len == 0) ? 0 : -1;
    }
    if (!nodes_bytes || nodes_len != n_points * sizeof(kd_node_t)) {
        return -1;
    }
    // Zero-copy: ARRAY_INLINE record payloads are 8-byte aligned by the
    // runner (Phase 5.1d format), so a direct typed-pointer cast through
    // the borrowed mmap'd bytes is safe. The cast away from `const` is
    // for symmetry with kd_build's writable nodes pointer; consumers
    // (kd_knn / radiusSearch) never mutate the tree.
    out->nodes = (kd_node_t*)(uintptr_t)nodes_bytes;
    out->nodes_owned = false;
    out->root = 0; // build_node always assigns the root id 0
    return 0;
}

void kd_free(kd_tree_t* t) {
    if (!t) {
        return;
    }
    if (t->nodes_owned) {
        free(t->nodes);
    }
    t->nodes = NULL;
    t->positions = NULL;
    t->n_points = 0;
    t->root = -1;
    t->nodes_owned = false;
}

// ----------------------------------------------------------------------------
// kNN: descend toward query, maintain a fixed-size max-heap of best (k smallest
// dist_sq) candidates. After a full descent we have the k nearest. Match the
// bench-v2 traversal exactly: visit the near subtree first, then prune the far
// subtree by squared split distance.
// ----------------------------------------------------------------------------

// Max-heap of size up to k. Heap root = worst-so-far. We replace root when we
// find a better candidate. Indexed via 0-based array indexing:
// parent(i) = (i-1)/2;  children = 2i+1, 2i+2.
static void heap_sift_down(kd_neighbor_t* heap, int n, int i) {
    for (;;) {
        int l = 2 * i + 1;
        int r = 2 * i + 2;
        int worst = i;
        if (l < n && heap[l].dist_sq > heap[worst].dist_sq) {
            worst = l;
        }
        if (r < n && heap[r].dist_sq > heap[worst].dist_sq) {
            worst = r;
        }
        if (worst == i) {
            return;
        }
        kd_neighbor_t tmp = heap[i];
        heap[i] = heap[worst];
        heap[worst] = tmp;
        i = worst;
    }
}

static void heap_sift_up(kd_neighbor_t* heap, int i) {
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (heap[parent].dist_sq >= heap[i].dist_sq) {
            return;
        }
        kd_neighbor_t tmp = heap[i];
        heap[i] = heap[parent];
        heap[parent] = tmp;
        i = parent;
    }
}

typedef struct knn_state {
    const kd_tree_t* tree;
    double qx, qy, qz;
    int k;
    kd_neighbor_t* heap; // size at most k
    int heap_n;
    double worst_dist_sq; // heap[0].dist_sq when heap is full, else +inf
} knn_state_t;

static void knn_consider(knn_state_t* s, int32_t point_idx, double dist_sq) {
    if (s->heap_n < s->k) {
        s->heap[s->heap_n].idx = point_idx;
        s->heap[s->heap_n].dist_sq = dist_sq;
        s->heap_n++;
        heap_sift_up(s->heap, s->heap_n - 1);
        if (s->heap_n == s->k) {
            s->worst_dist_sq = s->heap[0].dist_sq;
        }
    } else if (dist_sq < s->worst_dist_sq) {
        s->heap[0].idx = point_idx;
        s->heap[0].dist_sq = dist_sq;
        heap_sift_down(s->heap, s->heap_n, 0);
        s->worst_dist_sq = s->heap[0].dist_sq;
    }
}

// Second recursive function in this TU; Frama-C 32 needs the same
// explicit assigns clause build_node has (see above). knn_search mutates
// the heap state inside *s and reads through s->tree's nodes + positions.
/*@ requires \valid(s);
    requires \valid(s->tree);
    assigns *s, s->heap[..]
        \from *s, *(s->tree), node_id;
*/
static void knn_search(knn_state_t* s, int32_t node_id) {
    if (node_id < 0) {
        return;
    }
    const kd_node_t* node = &s->tree->nodes[node_id];
    const double* p = &s->tree->positions[node->point_idx * 3];
    double dx = p[0] - s->qx;
    double dy = p[1] - s->qy;
    double dz = p[2] - s->qz;
    double dist_sq = dx * dx + dy * dy + dz * dz;
    knn_consider(s, node->point_idx, dist_sq);

    double q_axis;
    switch (node->axis) {
        case 0: q_axis = s->qx; break;
        case 1: q_axis = s->qy; break;
        default: q_axis = s->qz; break;
    }
    double diff = q_axis - node->split_val;
    int32_t near = (diff < 0) ? node->left : node->right;
    int32_t far = (diff < 0) ? node->right : node->left;
    knn_search(s, near);
    if (s->heap_n < s->k || diff * diff < s->worst_dist_sq) {
        knn_search(s, far);
    }
}

static int cmp_neighbor_asc(const void* a, const void* b) {
    double da = ((const kd_neighbor_t*)a)->dist_sq;
    double db = ((const kd_neighbor_t*)b)->dist_sq;
    if (da < db) {
        return -1;
    }
    if (da > db) {
        return 1;
    }
    return 0;
}

int kd_knn(const kd_tree_t* t, const double query[3], int k, kd_neighbor_t* out,
           int* out_count) {
    if (!t || !query || k <= 0 || !out || !out_count) {
        return -1;
    }
    if (t->root < 0 || t->n_points == 0) {
        *out_count = 0;
        return 0;
    }
    knn_state_t s = {
        .tree = t,
        .qx = query[0],
        .qy = query[1],
        .qz = query[2],
        .k = k,
        .heap = out,
        .heap_n = 0,
        .worst_dist_sq = 1e308,
    };
    knn_search(&s, t->root);
    qsort(out, s.heap_n, sizeof(kd_neighbor_t), cmp_neighbor_asc);
    *out_count = s.heap_n;
    return 0;
}

// ---------------------------------------------------------------------------
// kd_radius_search — pre-order DFS, near-side first. Visit order matches
// bench-v2 lib/geometry/kd-tree.js radiusSearch byte-for-byte (consumed by
// grow-refit-merge's diffusion-limited grow + spatial-coherence majority
// vote, where the neighbour iteration order is load-bearing).
// ---------------------------------------------------------------------------

typedef struct radius_state {
    const kd_tree_t* tree;
    double qx, qy, qz;
    double radius_sq;
    kd_neighbor_t* out;
    int cap;
    int n;        // number written so far (≤ cap)
    int total;    // total in-radius matches (may exceed cap)
} radius_state_t;

// Same Frama-C-32 requirement as build_node / knn_search above: recursive
// functions need an explicit `assigns … \from …` clause or Eva bails with
// "Recursive call to radius_search_recurse without assigns clause" /
// "Generating probably incomplete assigns to interpret the call" and the
// analysis returns rc=1 instead of producing a clean alarm CSV (see
// v7_grm_impl rerun where this surfaced once seed_indices was wired up
// and Eva could finally reach the v7-normal-cascade-seed call into
// kd_radius_search). Pattern matches knn_search above.
/*@ requires \valid(s);
    requires \valid(s->tree);
    assigns *s, s->out[..]
        \from *s, *(s->tree), node_id;
*/
static void radius_search_recurse(radius_state_t* s, int32_t node_id) {
    if (node_id < 0) {
        return;
    }
    const kd_node_t* node = &s->tree->nodes[node_id];
    const double* p = &s->tree->positions[node->point_idx * 3];
    const double dx = p[0] - s->qx;
    const double dy = p[1] - s->qy;
    const double dz = p[2] - s->qz;
    const double dist_sq = dx * dx + dy * dy + dz * dz;

    if (dist_sq <= s->radius_sq) {
        if (s->n < s->cap) {
            s->out[s->n].idx = node->point_idx;
            s->out[s->n].dist_sq = dist_sq;
            s->n++;
        }
        s->total++;
    }

    double q_axis;
    switch (node->axis) {
        case 0: q_axis = s->qx; break;
        case 1: q_axis = s->qy; break;
        default: q_axis = s->qz; break;
    }
    const double diff = q_axis - node->split_val;
    const int32_t near = (diff < 0) ? node->left : node->right;
    const int32_t far  = (diff < 0) ? node->right : node->left;
    radius_search_recurse(s, near);
    if (diff * diff <= s->radius_sq) {
        radius_search_recurse(s, far);
    }
}

int kd_radius_search(const kd_tree_t* t, const double query[3], double radius,
                     kd_neighbor_t* out, int cap, int* out_count) {
    if (!t || !query || !out_count) {
        return -1;
    }
    if (t->root < 0 || t->n_points == 0 || radius < 0) {
        *out_count = 0;
        return 0;
    }
    radius_state_t s = {
        .tree = t,
        .qx = query[0], .qy = query[1], .qz = query[2],
        .radius_sq = radius * radius,
        .out = out, .cap = cap, .n = 0, .total = 0,
    };
    radius_search_recurse(&s, t->root);
    *out_count = s.total;   // caller may detect cap overflow when total > cap
    return 0;
}
