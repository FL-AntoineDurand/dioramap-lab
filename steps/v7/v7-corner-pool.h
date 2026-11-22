// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-corner-pool — shared corner-pool primitives.
//
// Extracted from v7-boundary-corners-impl.c so v7-boundary-merge (and
// later -classify / -regularize) can reuse the same pool model without
// duplicating types + super-merge logic. Holds:
//
//   - source-id enum + source priority comparator (matches v2's
//     POOL_SOURCE_PRIORITY)
//   - per-corner member struct (`v7p_member_t`)
//   - per-corner pool entry (`v7p_entry_t`) — array of members + dedup'd
//     plane_ids / ridge_ids / dirs_by_plane / com_bis_by_plane
//   - pool registry (`v7p_pool_t`) with redirect chains for super-merge
//   - upsert that does point-to-point merge within `merge_dist` AND
//     super-merge of any other entries the new point overlaps with
//   - dirs / com-bis add helpers (DIRS_REPLACE vs DIRS_IF_ABSENT)
//   - utility: extend_name (concat with token-dedup), push_uint_unique
//     (sorted-unique-ish), avg_members, vlen3
//
// Step-specific scaffolding (anchor / planeridge / name counters) stays
// in the consumer .c. The pool model itself is shared.
//
// Naming: `v7p_` prefix for new public symbols. Older corners-impl
// callers use these via thin static `static inline` aliases inside the
// .c so the existing call sites stay readable.

#ifndef V3_STEPS_V7_CORNER_POOL_H
#define V3_STEPS_V7_CORNER_POOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Source IDs (mirror ROOF-GRAPH-FORMAT.md §5.3) ──────────────────

enum {
    V7P_SRC_RIDGE_INTERSECTION = 1,
    V7P_SRC_RIDGE_END          = 2,
    V7P_SRC_POLYGON_MERGE      = 3,
    V7P_SRC_CONCAVE            = 4,
    V7P_SRC_CONVEX             = 5,
    // 6/7 are mr-group / mr-nearby in the spec but unused in v3.
    V7P_SRC_COPLANAR           = 8,
};

// Source-priority comparator. Total, pure: RI=4, RE=3, PM=2,
// concave/convex=1, coplanar/unknown=0.
/*@ assigns \nothing;
    ensures 0 <= \result <= 4; */
int v7p_src_priority(uint8_t s);

// ── Member authority (mirrors v7_bc_authority_t but lives here so the
//     pool doesn't depend on the corners-impl header). The two enums
//     hold identical values — corners-impl casts between them. ──

typedef enum {
    V7P_AUTHORITY_FULL  = 1,
    V7P_AUTHORITY_AXIAL = 2,
    V7P_AUTHORITY_NONE  = 3,
} v7p_authority_t;

// ── Pool-side member. members[] inside an entry are this type. ──

typedef struct v7p_member {
    double   x, y, z;
    uint8_t  source;
    v7p_authority_t authority;
    bool     has_axis;
    float    axis[3];
    uint32_t plane_id; bool has_plane_id;
    bool     has_ridge_id; uint32_t ridge_id;
    bool     has_dirs;
    float    dir_a[3], dir_b[3];
    bool     has_com_bisector;
    float    com_bisector[3];
    char*    name;                   // owned (deep-copied on push)
} v7p_member_t;

typedef struct v7p_dirs_entry {
    uint32_t plane_id;
    float    dir_a[3];
    float    dir_b[3];
} v7p_dirs_entry_t;

typedef struct v7p_com_bis_entry {
    uint32_t plane_id;
    float    com_bisector[3];
} v7p_com_bis_entry_t;

typedef struct v7p_entry {
    uint32_t id;                     // pool id (assigned at creation)
    double   x, y, z;
    uint8_t  source;                 // cached: max-priority(source_per_plane[])
    uint8_t  plane_type;             // rg_plane_type_t of this (single-type) corner —
                                     // set at creation; used to REFUSE cross-type merges
    char*    name;                   // owned
    uint32_t* plane_ids; uint32_t n_plane_ids; uint32_t plane_ids_cap;
    // Per-plane source label, parallel to plane_ids[] (same length, same cap).
    // Lets a single pool entry carry a different source label in each plane
    // — e.g. concave in its origin plane, coplanar in a plane it was imported
    // into. See ROOF-GRAPH-FORMAT.md §4.6.
    uint8_t*  source_per_plane;
    uint32_t* ridge_ids; uint32_t n_ridge_ids; uint32_t ridge_ids_cap;
    v7p_dirs_entry_t*    dirs;    uint32_t n_dirs;    uint32_t dirs_cap;
    v7p_com_bis_entry_t* com_bis; uint32_t n_com_bis; uint32_t com_bis_cap;
    v7p_member_t* members; uint32_t n_members; uint32_t members_cap;
    uint16_t n_merged;
    bool     consumed;               // super-merged into another entry
} v7p_entry_t;

typedef struct v7p_pool {
    v7p_entry_t** entries;
    uint32_t      n_entries;
    uint32_t      cap;
    uint32_t*     redirects;         // length redirects_cap (UINT32_MAX = none)
    uint32_t      next_id;
    uint32_t      redirects_cap;
} v7p_pool_t;

typedef enum { V7P_DIRS_REPLACE, V7P_DIRS_IF_ABSENT } v7p_dirs_mode_t;

// ── Lifecycle ──

void v7p_entry_free(v7p_entry_t* p);
void v7p_pool_free(v7p_pool_t* p);

// ── Helpers ──

// Length of a 3D vector. Inlined.
static inline double v7p_vlen3(double x, double y, double z) {
    // sqrt is in math.h — caller must #include <math.h> before this header
    // or before invocation. Kept inline to avoid a function-call hop in
    // the inner merge-distance loops.
    extern double sqrt(double);
    return sqrt(x*x + y*y + z*z);
}

// Concat 'B+A' style. Tokens (split on '+') are deduped, preserving
// order. Returns a fresh heap string or NULL on alloc-fail. NULL+NULL
// returns NULL; existing+NULL clones existing; NULL+incoming clones
// incoming.
char* v7p_extend_name(const char* existing, const char* incoming);

// Append uint32_t to a dynamic array iff not present. Linear scan
// (N small in practice).
int v7p_push_uint_unique(uint32_t** arr, uint32_t* n, uint32_t* cap, uint32_t v);

// Register plane_id on entry e with a per-plane source label. Single
// primitive used everywhere a plane gets attached to a corner.
//   - If plane_id already in e->plane_ids[], keep the higher-priority
//     source for that slot (priority defined by v7p_src_priority).
//   - Else parallel-append to plane_ids[] AND source_per_plane[].
//   - Updates cached e->source = max-priority(source_per_plane[]).
//   - In debug builds, asserts the invariant after the update.
// Returns 0 on success, -1 on alloc-fail.
//
// CANONICAL-SOURCE INVARIANT (the #254-263 contract): on success the
// cached e->source sits at the maximum per-plane priority —
//   v7p_src_priority(e->source) == max_k v7p_src_priority(e->source_per_plane[k])
// — and source_per_plane[] stays parallel to plane_ids[]. This is the
// single source of truth that v7_boundary_merge's eave-merge reads; the
// v7_corner_pool audit harness drives this mutator then calls
// v7p_entry_validate, so Eva checks the invariant assert holds. (Stated
// in prose, not as an Eva-checked `ensures`: the relation needs WP, not
// the EVA-only audit, to discharge.)
/*@ requires \valid(e);
    assigns *e;
    ensures \result == 0 || \result == -1; */
int v7p_entry_register_plane(v7p_entry_t* e, uint32_t plane_id, uint8_t src);

// In debug builds, asserts that priority(e->source) ==
// max(priority(e->source_per_plane[k])) and that source_per_plane
// length equals n_plane_ids. No-op in release. (Asserts ARE live under
// the Frama-C audit — NDEBUG is not defined — so driving this from a
// harness turns the invariant into an Eva-checked assertion.)
/*@ requires e == \null || \valid_read(e);
    requires e == \null || e->n_plane_ids == 0
                        || \valid_read(e->source_per_plane + (0 .. e->n_plane_ids - 1));
    assigns \nothing; */
void v7p_entry_validate(const v7p_entry_t* e);

// Average of an array of member positions; (0,0,0) if n == 0.
void v7p_avg_members(const v7p_member_t* arr, uint32_t n, double out[3]);

// Solve small symmetric system Mλ = r, k ∈ {1, 2, 3}. Returns 0 on
// success with λ in lambda[], -1 on degenerate matrix.
int v7p_solve_sym(double M[3][3], const double r[3], int k, double lambda[3]);

// Solve the authority-ranked member position into out[3]: FULL members
// (if any) → their centroid; else if no AXIAL members → centroid of all;
// else a constrained least-squares projecting the NONE/AXIAL target onto
// the intersection of up to 3 axial constraints (via v7p_solve_sym). Pure:
// reads `members[0..n-1]`, writes only out[]; no entry/pool state touched.
// n <= 0 → out = (0,0,0). This is the constrained-meet primitive shared by
// the corner-pool merge and (later) the regularize-loop MERGE_CORNER — the
// position math is identical to what v7p_recompute_entry used to inline.
/*@ requires n <= 0 || \valid_read(members + (0 .. n - 1));
    requires \valid(out + (0 .. 2));
    assigns out[0 .. 2]; */
void v7p_solve_member_position(const v7p_member_t* members, int n, double out[3]);

// Recompute the entry's authority-weighted centroid + source priority +
// nMerged from its members. Derives the cached e->source from the
// canonical source_per_plane[] store (NOT members[].source) — re-establishing
// the canonical-source invariant after member churn. The position is solved
// by v7p_solve_member_position over e->members. Tolerates e == NULL.
/*@ requires e == \null || \valid(e);
    assigns *e; */
void v7p_recompute_entry(v7p_entry_t* e);

// ── Mutators ──

// Push a member into entry->members. Deep-copies `src->name`.
int v7p_entry_push_member(v7p_entry_t* e, const v7p_member_t* src);

// dirsByPlane[plane_id] mutation with mode:
//   V7P_DIRS_REPLACE — overwrite if exists; insert if absent.
//   V7P_DIRS_IF_ABSENT — only insert if absent; existing untouched.
int v7p_entry_add_dirs(v7p_entry_t* e, uint32_t plane_id,
                        const float dir_a[3], const float dir_b[3],
                        v7p_dirs_mode_t mode);

// Same semantics for the CoM-bisector cache.
int v7p_entry_add_com_bis(v7p_entry_t* e, uint32_t plane_id,
                           const float com_bisector[3],
                           v7p_dirs_mode_t mode);

// ── Pool registry ──

int v7p_pool_ensure_redirects(v7p_pool_t* p, uint32_t need);
void v7p_pool_set_redirect(v7p_pool_t* p, uint32_t from_id, uint32_t to_id);
uint32_t v7p_pool_resolve(v7p_pool_t* p, uint32_t id);
v7p_entry_t* v7p_pool_find_by_id(v7p_pool_t* p, uint32_t id);
int v7p_pool_push_entry(v7p_pool_t* p, v7p_entry_t* e);

// ── Upsert with super-merge ──
//
// Walks every live entry. For each entry where ANY member lies within
// `merge_dist` of (mx, my, mz), include it as a "match".
//
//   matches == 0 → create new entry, push the implied member.
//   matches >= 1 → fold matches[1..] into matches[0] (super-merge),
//                  redirect the absorbed ids, then push the new member
//                  onto the primary.
//
// Always returns the primary entry on success; NULL on alloc-fail.
//
// The optional float[3] inputs are read only when their `has_*` flag is
// set; the preconditions below match that.
/*@ requires \valid(pool);
    requires name == \null || \valid_read(name);
    requires !has_dirs || (\valid_read(dir_a + (0 .. 2)) && \valid_read(dir_b + (0 .. 2)));
    requires !has_com_bisector || \valid_read(com_bisector + (0 .. 2));
    requires !has_axis || \valid_read(axis + (0 .. 2));
    assigns *pool;
    ensures \result == \null || \valid(\result); */
v7p_entry_t* v7p_upsert(v7p_pool_t* pool,
                         double mx, double my, double mz,
                         uint8_t source,
                         const char* name,
                         uint32_t plane_id, bool has_plane_id,
                         bool has_dirs, const float dir_a[3], const float dir_b[3],
                         bool has_com_bisector, const float com_bisector[3],
                         v7p_authority_t authority,
                         bool has_axis, const float axis[3],
                         bool has_ridge_id, uint32_t ridge_id,
                         uint8_t plane_type,   // rg_plane_type_t — refuse fusing a match of a different type
                         double merge_dist);

#ifdef __cplusplus
}
#endif
#endif
