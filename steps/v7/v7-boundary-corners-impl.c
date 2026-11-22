// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-boundary-corners — pure compute. See header for phase ordering.

#define _POSIX_C_SOURCE 200809L

#include "v7-boundary-corners-impl.h"
#include "v3/safe_alloc.h"
#include "v7-corner-pool.h"
#include "v7-roof-graph-format.h"   // rg_plane_type_t + rg_plane_types_mergeable (decouple gate)

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef NAN
#  define NAN (0.0/0.0)
#endif

// strdup is POSIX; declared via string.h with _POSIX_C_SOURCE above.

// ── Source IDs — aliases over the shared pool's enum so the per-step
// code below reads the same as before the v7-corner-pool extraction. ──
enum {
    SRC_RIDGE_INTERSECTION = V7P_SRC_RIDGE_INTERSECTION,
    SRC_RIDGE_END          = V7P_SRC_RIDGE_END,
    SRC_POLYGON_MERGE      = V7P_SRC_POLYGON_MERGE,
    SRC_CONCAVE            = V7P_SRC_CONCAVE,
    SRC_CONVEX             = V7P_SRC_CONVEX,
    SRC_COPLANAR           = V7P_SRC_COPLANAR,
};

// Thin aliases for the shared pool API so existing call sites in this
// file read with their original short names. The shared module owns
// the bodies; the .c here only ports the corners-specific anchor /
// planeridge / name-counter code.
typedef v7p_member_t           pmember_t;
typedef v7p_dirs_entry_t       dirs_by_plane_entry_t;
typedef v7p_com_bis_entry_t    com_bis_by_plane_entry_t;
typedef v7p_entry_t            pentry_t;
typedef v7p_pool_t             ppool_t;
typedef v7p_dirs_mode_t        dirs_add_mode_t;
#define DIRS_REPLACE           V7P_DIRS_REPLACE
#define DIRS_IF_ABSENT         V7P_DIRS_IF_ABSENT
#define pentry_free            v7p_entry_free
#define pool_free              v7p_pool_free
#define pool_set_redirect      v7p_pool_set_redirect
#define pool_resolve           v7p_pool_resolve
#define pool_find_by_id        v7p_pool_find_by_id
#define pool_push_entry        v7p_pool_push_entry
#define pentry_push_member     v7p_entry_push_member
#define pentry_add_dirs        v7p_entry_add_dirs
#define pentry_add_com_bis     v7p_entry_add_com_bis
#define push_uint_unique       v7p_push_uint_unique
#define entry_register_plane   v7p_entry_register_plane
#define avg_members            v7p_avg_members
#define recompute_pool_entry   v7p_recompute_entry
#define solve_sym              v7p_solve_sym
#define extend_name            v7p_extend_name
#define src_priority           v7p_src_priority
#define vlen3                  v7p_vlen3

// Authority enum used by the pool module — v7_bc_authority_t lives in
// the corners header and has matching values; pass-through via cast at
// the upsert call site below.

// Local thin wrapper around v7p_upsert preserving the legacy parameter
// order this file's callers use. Casts the corners-impl authority enum
// to the pool's identical-valued enum.
static pentry_t* upsert_pool_member(ppool_t* pool,
                                     double mx, double my, double mz,
                                     uint8_t source,
                                     const char* name,
                                     uint32_t plane_id, bool has_plane_id,
                                     bool has_dirs, const float dir_a[3], const float dir_b[3],
                                     bool has_com_bisector, const float com_bisector[3],
                                     v7_bc_authority_t authority,
                                     bool has_axis, const float axis[3],
                                     bool has_ridge_id, uint32_t ridge_id,
                                     uint8_t plane_type,
                                     double pool_merge_dist) {
    return v7p_upsert(pool, mx, my, mz, source, name,
                       plane_id, has_plane_id,
                       has_dirs, dir_a, dir_b,
                       has_com_bisector, com_bisector,
                       (v7p_authority_t)authority,
                       has_axis, axis,
                       has_ridge_id, ridge_id,
                       plane_type,
                       pool_merge_dist);
}

// ── Params ──────────────────────────────────────────────────────────

void v7_bc_params_default(v7_bc_params_t* p) {
    p->snap_radius             = 0.30;
    p->wall_tilt_min_deg       = 72.0;
    p->wall_side_margin        = 0.30;
    p->wall_min_h_extent       = 0.50;
    p->pool_merge_dist         = 0.20;
    p->coplanar_dist_threshold = 0.12;
    p->coplanar_balance_min    = 0.30;
    p->ridge_side_max_dist     = 1.5;
    p->up_axis                 = 2;
}

// ── Internal scratch types ──────────────────────────────────────────

// An anchor — pre-pool candidate, per-plane. Mirrors v2's planeAnchors[g][i].
typedef struct anchor {
    double   x, y, z;
    bool     has_dirs;
    float    dir_a[3], dir_b[3];
    bool     has_com_bisector;
    float    com_bisector[3];
    uint8_t  source;                 // 1 (RI) / 2 (ridge-end) / 4 / 5 / 8
    uint32_t plane_id;
    char*    name;                   // owned (we re-allocate later); NULL if unnamed
    // meta — minimal subset we actually use:
    bool     has_pool_id;
    uint32_t pool_id;                // back-link to upstream pool corner
    bool     wall_bbox;              // D.0 flag (informational; kept off the wire)
    // `snapped_to_ridge` was deleted 2026-05-25: after the on-ridge property
    // refactor, D.2b reads on_ridge_ids[] instead of the snap-flag gate. The
    // bool had no other readers.
    bool     has_ridge_id;
    uint32_t ridge_id;
    // Ridges this anchor sits ON BY CONSTRUCTION (not "near"). Populated by
    // the source that creates the anchor: RI inherits the pool entry's
    // intersection set; RE = [endpoint ridge]; D.2 snap appends snap-target;
    // CC/CV/wall-bbox start empty. Consumed by D.2b to gate out the
    // FP-coin-flip side test (perp ≈ 0 by construction → sign(side_d) is
    // last-bit noise). Owned by the anchor; freed in anchor_free.
    uint32_t* on_ridge_ids;
    uint32_t  n_on_ridge_ids;
    bool     killed;                 // D.2b filter / dedup
} anchor_t;

// A per-plane RidgeRecord — derived from RIDGE_LINES + plane id.
typedef struct planeridge {
    double   from[3], to[3];
    double   point[3];               // line_point
    double   direction[3];
    double   t_min, t_max;
    uint32_t g_a, g_b;
    uint32_t ridge_line_idx;
} planeridge_t;

// ── Step-local lifecycle helpers ──

static void anchor_free(anchor_t* a) {
    free(a->name);
    free(a->on_ridge_ids);
    memset(a, 0, sizeof(*a));
}

// Test whether this anchor sits on `ridge_line_idx` by construction. Property-
// based — no FP-fragile geometric check. Returns true iff the ridge is in the
// anchor's on_ridge_ids[] set.
static inline bool anchor_on_ridge(const anchor_t* a, uint32_t ridge_line_idx) {
    for (uint32_t i = 0; i < a->n_on_ridge_ids; i++)
        if (a->on_ridge_ids[i] == ridge_line_idx) return true;
    return false;
}

// Append `ridge_line_idx` to anchor's on-ridge set, skipping duplicates.
// No-op on alloc failure (set stays consistent).
static inline void anchor_push_on_ridge(anchor_t* a, uint32_t ridge_line_idx) {
    for (uint32_t i = 0; i < a->n_on_ridge_ids; i++)
        if (a->on_ridge_ids[i] == ridge_line_idx) return;
    uint32_t* p = (uint32_t*)safe_realloc_array(a->on_ridge_ids, a->n_on_ridge_ids + 1, sizeof(uint32_t));
    if (!p) return;
    p[a->n_on_ridge_ids] = ridge_line_idx;
    a->on_ridge_ids = p;
    a->n_on_ridge_ids++;
}

#if 0
// Moved to v7-corner-pool.{c,h} — kept as comment markers for the
// reader. Originals (pmember_t, dirs_by_plane_entry_t, com_bis_*,
// pentry_t, pentry_free, vlen3, extend_name, solve_sym,
// push_uint_unique, ppool_t, pool_*, avg_members, recompute_pool_entry,
// pentry_push_member, pentry_add_dirs, pentry_add_com_bis,
// upsert_pool_member) are now in the shared pool module — accessed
// here through the `pmember_t`/`pentry_t`/`v7p_*` aliases established
// above.
static char* extend_name(const char* existing, const char* incoming) {
    if (!incoming) return existing ? strdup(existing) : (char*)NULL;
    if (!existing) return strdup(incoming);
    // Tokenize existing by '+', keep unique tokens, then add tokens from
    // incoming that aren't already present.
    size_t cap = strlen(existing) + 1 + strlen(incoming) + 2;
    char* buf = (char*)malloc(cap);
    if (!buf) return NULL;
    strcpy(buf, existing);
    size_t buf_len = strlen(buf);
    // Now scan incoming tokens.
    const char* p = incoming;
    while (*p) {
        const char* q = strchr(p, '+');
        size_t tok_len = q ? (size_t)(q - p) : strlen(p);
        if (tok_len > 0) {
            // is this token in buf?
            bool found = false;
            const char* s = buf;
            while (*s) {
                size_t t_len;
                const char* nx = strchr(s, '+');
                t_len = nx ? (size_t)(nx - s) : strlen(s);
                if (t_len == tok_len && memcmp(s, p, tok_len) == 0) {
                    found = true; break;
                }
                if (!nx) break;
                s = nx + 1;
            }
            if (!found) {
                if (buf_len + 1 + tok_len + 1 > cap) {
                    cap = (buf_len + tok_len + 8) * 2;
                    char* nb = (char*)realloc(buf, cap);
                    if (!nb) { free(buf); return NULL; }
                    buf = nb;
                }
                buf[buf_len++] = '+';
                memcpy(buf + buf_len, p, tok_len);
                buf_len += tok_len;
                buf[buf_len] = '\0';
            }
        }
        p = q ? q + 1 : p + tok_len;
    }
    return buf;
}

// Solve small symmetric system Mλ = r, k ∈ {1, 2, 3}. Returns 0 on
// success with λ in lambda[], -1 on degenerate.
static int solve_sym(double M[3][3], const double r[3], int k, double lambda[3]) {
    if (k == 1) {
        if (fabs(M[0][0]) < 1e-9) return -1;
        lambda[0] = r[0] / M[0][0];
        return 0;
    }
    if (k == 2) {
        double a = M[0][0], b = M[0][1], c = M[1][0], dd = M[1][1];
        double det = a*dd - b*c;
        if (fabs(det) < 1e-9) return -1;
        lambda[0] = (dd*r[0] - b*r[1]) / det;
        lambda[1] = (-c*r[0] + a*r[1]) / det;
        return 0;
    }
    // k == 3 via cofactor.
    double co00 = M[1][1]*M[2][2] - M[1][2]*M[2][1];
    double co01 = M[1][0]*M[2][2] - M[1][2]*M[2][0];
    double co02 = M[1][0]*M[2][1] - M[1][1]*M[2][0];
    double det  = M[0][0]*co00 - M[0][1]*co01 + M[0][2]*co02;
    if (fabs(det) < 1e-9) return -1;
    double inv = 1.0 / det;
    double co10 = M[0][1]*M[2][2] - M[0][2]*M[2][1];
    double co11 = M[0][0]*M[2][2] - M[0][2]*M[2][0];
    double co12 = M[0][0]*M[2][1] - M[0][1]*M[2][0];
    double co20 = M[0][1]*M[1][2] - M[0][2]*M[1][1];
    double co21 = M[0][0]*M[1][2] - M[0][2]*M[1][0];
    double co22 = M[0][0]*M[1][1] - M[0][1]*M[1][0];
    lambda[0] = (co00*r[0] - co10*r[1] + co20*r[2]) * inv;
    lambda[1] = (-co01*r[0] + co11*r[1] - co21*r[2]) * inv;
    lambda[2] = (co02*r[0] - co12*r[1] + co22*r[2]) * inv;
    return 0;
}

// Append uint32_t to dynamic array (sorted-unique-ish via linear scan).
static int push_uint_unique(uint32_t** arr, uint32_t* n, uint32_t* cap, uint32_t v) {
    for (uint32_t i = 0; i < *n; i++) if ((*arr)[i] == v) return 0;
    if (*n == *cap) {
        uint32_t nc = *cap ? *cap * 2 : 4;
        uint32_t* p = (uint32_t*)safe_realloc_array(*arr, nc, sizeof(uint32_t));
        if (!p) return -1;
        *arr = p; *cap = nc;
    }
    (*arr)[(*n)++] = v;
    return 0;
}
#endif  // moved-to-v7-corner-pool block

// ── Name counters (port of v2 nameCounters) ──────────────────────────

typedef struct nctrs {
    uint32_t ri;
    uint32_t re;
    uint32_t mrg;
    // Per-plane keyed counters. We use parallel u32 arrays keyed by
    // plane_id (linear scan — N small).
    uint32_t* concave_pid;  uint32_t* concave_c;  uint32_t concave_n,  concave_cap;
    uint32_t* convex_pid;   uint32_t* convex_c;   uint32_t convex_n,   convex_cap;
    uint32_t* coplanar_pid; uint32_t* coplanar_c; uint32_t coplanar_n, coplanar_cap;
} nctrs_t;

static void nctrs_free(nctrs_t* n) {
    free(n->concave_pid);  free(n->concave_c);
    free(n->convex_pid);   free(n->convex_c);
    free(n->coplanar_pid); free(n->coplanar_c);
    memset(n, 0, sizeof(*n));
}

static void seed_nctrs_from_corners(nctrs_t* n,
                                      const v7_bc_input_corner_t* seeds, size_t n_seeds) {
    for (size_t i = 0; i < n_seeds; i++) {
        const v7_bc_input_corner_t* c = &seeds[i];
        if (!c->name) continue;
        // RI<n>
        if (c->name[0] == 'R' && c->name[1] == 'I') {
            unsigned u = 0;
            if (sscanf(c->name + 2, "%u", &u) == 1 && u + 1 > n->ri) n->ri = u + 1;
            continue;
        }
        // RE<n>
        if (c->name[0] == 'R' && c->name[1] == 'E') {
            unsigned u = 0;
            if (sscanf(c->name + 2, "%u", &u) == 1 && u + 1 > n->re) n->re = u + 1;
            continue;
        }
        // MRG<n>
        if (c->name[0] == 'M' && c->name[1] == 'R' && c->name[2] == 'G') {
            unsigned u = 0;
            if (sscanf(c->name + 3, "%u", &u) == 1 && u + 1 > n->mrg) n->mrg = u + 1;
            continue;
        }
        // P<pid>(C|X|CP)<n>
        if (c->name[0] == 'P') {
            unsigned pid = 0, idx = 0;
            char tag[3] = {0};
            if (sscanf(c->name + 1, "%u", &pid) != 1) continue;
            const char* p = c->name + 1;
            while (*p >= '0' && *p <= '9') p++;
            if (p[0] == 'C' && p[1] == 'P') {
                tag[0] = 'C'; tag[1] = 'P';
                if (sscanf(p + 2, "%u", &idx) != 1) continue;
            } else if (*p == 'C' || *p == 'X') {
                tag[0] = *p;
                if (sscanf(p + 1, "%u", &idx) != 1) continue;
            } else continue;
            uint32_t** pid_arr; uint32_t** c_arr; uint32_t* nptr; uint32_t* capptr;
            if (tag[0] == 'C' && tag[1] == 'P') {
                pid_arr = &n->coplanar_pid; c_arr = &n->coplanar_c;
                nptr = &n->coplanar_n; capptr = &n->coplanar_cap;
            } else if (tag[0] == 'C') {
                pid_arr = &n->concave_pid; c_arr = &n->concave_c;
                nptr = &n->concave_n; capptr = &n->concave_cap;
            } else {
                pid_arr = &n->convex_pid; c_arr = &n->convex_c;
                nptr = &n->convex_n; capptr = &n->convex_cap;
            }
            // Set this counter to max(current, idx+1).
            bool found = false;
            for (uint32_t k = 0; k < *nptr; k++) {
                if ((*pid_arr)[k] == pid) {
                    if (idx + 1 > (*c_arr)[k]) (*c_arr)[k] = idx + 1;
                    found = true; break;
                }
            }
            if (!found) {
                if (*nptr == *capptr) {
                    uint32_t nc = *capptr ? *capptr * 2 : 8;
                    uint32_t* p1 = (uint32_t*)safe_realloc_array(*pid_arr, nc, sizeof(uint32_t));
                    uint32_t* p2 = (uint32_t*)safe_realloc_array(*c_arr, nc, sizeof(uint32_t));
                    if (!p1 || !p2) continue;
                    *pid_arr = p1; *c_arr = p2; *capptr = nc;
                }
                (*pid_arr)[*nptr] = pid;
                (*c_arr)[*nptr]   = idx + 1;
                (*nptr)++;
            }
        }
    }
}

static char* name_for(nctrs_t* n, uint8_t source, uint32_t plane_id) {
    char buf[32];
    switch (source) {
        case SRC_RIDGE_INTERSECTION:
            snprintf(buf, sizeof(buf), "RI%u", n->ri++);
            return strdup(buf);
        case SRC_RIDGE_END:
            snprintf(buf, sizeof(buf), "RE%u", n->re++);
            return strdup(buf);
        case SRC_POLYGON_MERGE:
            snprintf(buf, sizeof(buf), "MRG%u", n->mrg++);
            return strdup(buf);
        case SRC_CONCAVE: {
            uint32_t pre = 0;
            bool found = false;
            for (uint32_t i = 0; i < n->concave_n; i++) {
                if (n->concave_pid[i] == plane_id) { pre = n->concave_c[i]++; found = true; break; }
            }
            if (!found) {
                if (n->concave_n == n->concave_cap) {
                    uint32_t nc = n->concave_cap ? n->concave_cap * 2 : 8;
                    uint32_t* p1 = (uint32_t*)safe_realloc_array(n->concave_pid, nc, sizeof(uint32_t));
                    uint32_t* p2 = (uint32_t*)safe_realloc_array(n->concave_c, nc, sizeof(uint32_t));
                    if (!p1 || !p2) return NULL;
                    n->concave_pid = p1; n->concave_c = p2; n->concave_cap = nc;
                }
                n->concave_pid[n->concave_n] = plane_id;
                n->concave_c[n->concave_n]   = 1;
                pre = 0;
                n->concave_n++;
            }
            snprintf(buf, sizeof(buf), "P%uC%u", plane_id, pre);
            return strdup(buf);
        }
        case SRC_CONVEX: {
            uint32_t pre = 0;
            bool found = false;
            for (uint32_t i = 0; i < n->convex_n; i++) {
                if (n->convex_pid[i] == plane_id) { pre = n->convex_c[i]++; found = true; break; }
            }
            if (!found) {
                if (n->convex_n == n->convex_cap) {
                    uint32_t nc = n->convex_cap ? n->convex_cap * 2 : 8;
                    uint32_t* p1 = (uint32_t*)safe_realloc_array(n->convex_pid, nc, sizeof(uint32_t));
                    uint32_t* p2 = (uint32_t*)safe_realloc_array(n->convex_c, nc, sizeof(uint32_t));
                    if (!p1 || !p2) return NULL;
                    n->convex_pid = p1; n->convex_c = p2; n->convex_cap = nc;
                }
                n->convex_pid[n->convex_n] = plane_id;
                n->convex_c[n->convex_n]   = 1;
                pre = 0;
                n->convex_n++;
            }
            snprintf(buf, sizeof(buf), "P%uX%u", plane_id, pre);
            return strdup(buf);
        }
        case SRC_COPLANAR: {
            uint32_t pre = 0;
            bool found = false;
            for (uint32_t i = 0; i < n->coplanar_n; i++) {
                if (n->coplanar_pid[i] == plane_id) { pre = n->coplanar_c[i]++; found = true; break; }
            }
            if (!found) {
                if (n->coplanar_n == n->coplanar_cap) {
                    uint32_t nc = n->coplanar_cap ? n->coplanar_cap * 2 : 8;
                    uint32_t* p1 = (uint32_t*)safe_realloc_array(n->coplanar_pid, nc, sizeof(uint32_t));
                    uint32_t* p2 = (uint32_t*)safe_realloc_array(n->coplanar_c, nc, sizeof(uint32_t));
                    if (!p1 || !p2) return NULL;
                    n->coplanar_pid = p1; n->coplanar_c = p2; n->coplanar_cap = nc;
                }
                n->coplanar_pid[n->coplanar_n] = plane_id;
                n->coplanar_c[n->coplanar_n]   = 1;
                pre = 0;
                n->coplanar_n++;
            }
            snprintf(buf, sizeof(buf), "P%uCP%u", plane_id, pre);
            return strdup(buf);
        }
        default: return NULL;
    }
}

// ── Outputs init/free ──

void v7_bc_outputs_init(v7_bc_outputs_t* o) {
    memset(o, 0, sizeof(*o));
}

static void corner_free(v7_bc_corner_t* c) {
    free(c->name);
    free(c->plane_ids);
    free(c->source_per_plane);
    free(c->ridge_ids);
    free(c->dirs);
    free(c->com_bisectors);
    free(c->members);
    memset(c, 0, sizeof(*c));
}

void v7_bc_outputs_free(v7_bc_outputs_t* o) {
    if (!o) return;
    for (size_t i = 0; i < o->n_corners; i++) corner_free(&o->corners[i]);
    free(o->corners);
    if (o->plane_corner_ids) {
        for (size_t i = 0; i < o->n_planes; i++) free(o->plane_corner_ids[i]);
        free(o->plane_corner_ids);
    }
    free(o->plane_corner_ids_count);
    memset(o, 0, sizeof(*o));
}

// ── Pool registry — MOVED to v7-corner-pool.{c,h}. Bodies below are
//    dead-coded (#if 0) so the diff stays reviewable; can be deleted
//    in a cleanup pass after merge lands and the refactor is stable. ──

#if 0
typedef struct ppool {
    pentry_t** entries;
    uint32_t   n_entries;
    uint32_t   cap;
    // Pool id → entry index map, including redirect chains for
    // super-merged ids. We store the next-redirect or self.
    uint32_t* redirects;             // length next_id (sparse-ish)
    uint32_t  next_id;
    uint32_t  redirects_cap;
} ppool_t;

static int pool_ensure_redirects(ppool_t* p, uint32_t need) {
    if (need <= p->redirects_cap) return 0;
    uint32_t nc = p->redirects_cap ? p->redirects_cap * 2 : 32;
    while (nc < need) nc *= 2;
    uint32_t* arr = (uint32_t*)safe_realloc_array(p->redirects, nc, sizeof(uint32_t));
    if (!arr) return -1;
    for (uint32_t i = p->redirects_cap; i < nc; i++) arr[i] = UINT32_MAX;
    p->redirects = arr;
    p->redirects_cap = nc;
    return 0;
}

static void pool_set_redirect(ppool_t* p, uint32_t from_id, uint32_t to_id) {
    if (pool_ensure_redirects(p, from_id + 1) != 0) return;
    p->redirects[from_id] = to_id;
}

static uint32_t pool_resolve(ppool_t* p, uint32_t id) {
    while (id < p->redirects_cap && p->redirects[id] != UINT32_MAX) {
        id = p->redirects[id];
    }
    return id;
}

static pentry_t* pool_find_by_id(ppool_t* p, uint32_t id) {
    uint32_t r = pool_resolve(p, id);
    for (uint32_t i = 0; i < p->n_entries; i++) {
        if (p->entries[i] && p->entries[i]->id == r) return p->entries[i];
    }
    return NULL;
}

static int pool_push_entry(ppool_t* p, pentry_t* e) {
    if (p->n_entries == p->cap) {
        uint32_t nc = p->cap ? p->cap * 2 : 16;
        pentry_t** arr = (pentry_t**)safe_realloc_array(p->entries, nc, sizeof(pentry_t*));
        if (!arr) return -1;
        p->entries = arr; p->cap = nc;
    }
    p->entries[p->n_entries++] = e;
    return 0;
}

static void pool_free(ppool_t* p) {
    for (uint32_t i = 0; i < p->n_entries; i++) {
        if (p->entries[i]) { pentry_free(p->entries[i]); free(p->entries[i]); }
    }
    free(p->entries);
    free(p->redirects);
    memset(p, 0, sizeof(*p));
}

// avg of an array of pmembers — returns centroid; returns 0,0,0 for empty.
static void avg_members(const pmember_t* arr, uint32_t n, double out[3]) {
    if (n == 0) { out[0] = out[1] = out[2] = 0; return; }
    double cx = 0, cy = 0, cz = 0;
    for (uint32_t i = 0; i < n; i++) { cx += arr[i].x; cy += arr[i].y; cz += arr[i].z; }
    out[0] = cx / n; out[1] = cy / n; out[2] = cz / n;
}

static void recompute_pool_entry(pentry_t* e) {
    if (!e) return;
    // Derive cached `e->source` from canonical `source_per_plane[]` — same fix
    // as v7p_recompute_entry. See that function's comment for the bug story.
    if (e->n_plane_ids > 0) {
        uint8_t best_src = e->source_per_plane[0];
        for (uint32_t k = 1; k < e->n_plane_ids; k++) {
            if (src_priority(e->source_per_plane[k]) > src_priority(best_src)) {
                best_src = e->source_per_plane[k];
            }
        }
        e->source = best_src;
    }
    e->n_merged = (uint16_t)e->n_members;
    if (e->n_members == 0) return;

    // 1) authority=full → simple average.
    pmember_t* full = (pmember_t*)safe_malloc_array(e->n_members, sizeof(pmember_t));
    pmember_t* axial = (pmember_t*)safe_malloc_array(e->n_members, sizeof(pmember_t));
    pmember_t* none  = (pmember_t*)safe_malloc_array(e->n_members, sizeof(pmember_t));
    uint32_t nf = 0, na = 0, nn = 0;
    if (!full || !axial || !none) { free(full); free(axial); free(none); return; }
    for (uint32_t i = 0; i < e->n_members; i++) {
        const pmember_t* m = &e->members[i];
        if (m->authority == V7_BC_AUTHORITY_FULL) full[nf++] = *m;
        else if (m->authority == V7_BC_AUTHORITY_AXIAL && m->has_axis) axial[na++] = *m;
        else none[nn++] = *m;
    }
    if (nf > 0) {
        double c[3]; avg_members(full, nf, c);
        e->x = c[0]; e->y = c[1]; e->z = c[2];
        free(full); free(axial); free(none); return;
    }
    if (na == 0) {
        double c[3]; avg_members(e->members, e->n_members, c);
        e->x = c[0]; e->y = c[1]; e->z = c[2];
        free(full); free(axial); free(none); return;
    }
    double target[3];
    if (nn > 0) avg_members(none, nn, target);
    else        avg_members(axial, na, target);

    int k = (na < 3) ? (int)na : 3;
    // Build A rows + b values from first k axial members.
    double A[3][3] = {{0}};
    double b[3] = {0};
    for (int i = 0; i < k; i++) {
        A[i][0] = axial[i].axis[0];
        A[i][1] = axial[i].axis[1];
        A[i][2] = axial[i].axis[2];
        b[i] = axial[i].x * A[i][0] + axial[i].y * A[i][1] + axial[i].z * A[i][2];
    }
    double Atarget[3];
    for (int i = 0; i < k; i++) {
        Atarget[i] = A[i][0]*target[0] + A[i][1]*target[1] + A[i][2]*target[2];
    }
    double r[3];
    for (int i = 0; i < k; i++) r[i] = b[i] - Atarget[i];
    // AA^T (k×k).
    double M[3][3] = {{0}};
    for (int i = 0; i < k; i++) {
        for (int j = 0; j < k; j++) {
            M[i][j] = A[i][0]*A[j][0] + A[i][1]*A[j][1] + A[i][2]*A[j][2];
        }
    }
    double lambda[3];
    if (solve_sym(M, r, k, lambda) != 0) {
        e->x = target[0]; e->y = target[1]; e->z = target[2];
        free(full); free(axial); free(none); return;
    }
    double px = target[0], py = target[1], pz = target[2];
    for (int i = 0; i < k; i++) {
        px += A[i][0] * lambda[i];
        py += A[i][1] * lambda[i];
        pz += A[i][2] * lambda[i];
    }
    e->x = px; e->y = py; e->z = pz;
    free(full); free(axial); free(none);
}

// Push a pmember into pentry->members (deep-copies name).
static int pentry_push_member(pentry_t* e, const pmember_t* src) {
    if (e->n_members == e->members_cap) {
        uint32_t nc = e->members_cap ? e->members_cap * 2 : 4;
        pmember_t* arr = (pmember_t*)safe_realloc_array(e->members, nc, sizeof(pmember_t));
        if (!arr) return -1;
        e->members = arr; e->members_cap = nc;
    }
    e->members[e->n_members] = *src;
    e->members[e->n_members].name = src->name ? strdup(src->name) : (char*)NULL;
    e->n_members++;
    return 0;
}

// Append/replace dirsByPlane entry. v2: primary.dirsByPlane[k] = v
// only if not already set (super-merge "if null"), but later on the
// straight add overwrites — we mirror exactly.
//   ADD_REPLACE — overwrite if exists.
//   ADD_IF_ABSENT — only set if missing.
typedef enum { DIRS_REPLACE, DIRS_IF_ABSENT } dirs_add_mode_t;

static int pentry_add_dirs(pentry_t* e, uint32_t plane_id,
                             const float dir_a[3], const float dir_b[3],
                             dirs_add_mode_t mode) {
    for (uint32_t i = 0; i < e->n_dirs; i++) {
        if (e->dirs[i].plane_id == plane_id) {
            if (mode == DIRS_REPLACE) {
                memcpy(e->dirs[i].dir_a, dir_a, 12);
                memcpy(e->dirs[i].dir_b, dir_b, 12);
            }
            return 0;
        }
    }
    if (e->n_dirs == e->dirs_cap) {
        uint32_t nc = e->dirs_cap ? e->dirs_cap * 2 : 4;
        dirs_by_plane_entry_t* arr = (dirs_by_plane_entry_t*)realloc(e->dirs, nc * sizeof(*arr));
        if (!arr) return -1;
        e->dirs = arr; e->dirs_cap = nc;
    }
    e->dirs[e->n_dirs].plane_id = plane_id;
    memcpy(e->dirs[e->n_dirs].dir_a, dir_a, 12);
    memcpy(e->dirs[e->n_dirs].dir_b, dir_b, 12);
    e->n_dirs++;
    return 0;
}

// Same semantics as pentry_add_dirs but for the CoM-bisector cache.
static int pentry_add_com_bis(pentry_t* e, uint32_t plane_id,
                                const float com_bisector[3],
                                dirs_add_mode_t mode) {
    for (uint32_t i = 0; i < e->n_com_bis; i++) {
        if (e->com_bis[i].plane_id == plane_id) {
            if (mode == DIRS_REPLACE) memcpy(e->com_bis[i].com_bisector, com_bisector, 12);
            return 0;
        }
    }
    if (e->n_com_bis == e->com_bis_cap) {
        uint32_t nc = e->com_bis_cap ? e->com_bis_cap * 2 : 4;
        com_bis_by_plane_entry_t* arr = (com_bis_by_plane_entry_t*)realloc(e->com_bis, nc * sizeof(*arr));
        if (!arr) return -1;
        e->com_bis = arr; e->com_bis_cap = nc;
    }
    e->com_bis[e->n_com_bis].plane_id = plane_id;
    memcpy(e->com_bis[e->n_com_bis].com_bisector, com_bisector, 12);
    e->n_com_bis++;
    return 0;
}

// Pool member upsert. Returns pointer to the entry that absorbed the
// member (NULL on alloc failure).
static pentry_t* upsert_pool_member(ppool_t* pool,
                                      double mx, double my, double mz,
                                      uint8_t source,
                                      const char* name,
                                      uint32_t plane_id, bool has_plane_id,
                                      bool has_dirs, const float dir_a[3], const float dir_b[3],
                                      bool has_com_bisector, const float com_bisector[3],
                                      v7_bc_authority_t authority,
                                      bool has_axis, const float axis[3],
                                      bool has_ridge_id, uint32_t ridge_id,
                                      double pool_merge_dist) {
    double merge_sq = pool_merge_dist * pool_merge_dist;
    // Find all entries with at least one member within merge radius.
    uint32_t* match_idx = NULL;
    uint32_t  n_match = 0, cap_match = 0;
    for (uint32_t i = 0; i < pool->n_entries; i++) {
        pentry_t* e = pool->entries[i];
        if (!e || e->consumed) continue;
        bool hit = false;
        for (uint32_t k = 0; k < e->n_members; k++) {
            double dx = e->members[k].x - mx;
            double dy = e->members[k].y - my;
            double dz = e->members[k].z - mz;
            if (dx*dx + dy*dy + dz*dz < merge_sq) { hit = true; break; }
        }
        if (hit) {
            if (n_match == cap_match) {
                uint32_t nc = cap_match ? cap_match * 2 : 4;
                uint32_t* p = (uint32_t*)safe_realloc_array(match_idx, nc, sizeof(uint32_t));
                if (!p) { free(match_idx); return NULL; }
                match_idx = p; cap_match = nc;
            }
            match_idx[n_match++] = i;
        }
    }

    pmember_t member = {0};
    member.x = mx; member.y = my; member.z = mz;
    member.source = source;
    member.authority = authority;
    member.has_axis = has_axis;
    if (has_axis) memcpy(member.axis, axis, 12);
    member.plane_id = plane_id; member.has_plane_id = has_plane_id;
    member.has_ridge_id = has_ridge_id; member.ridge_id = ridge_id;
    member.has_dirs = has_dirs;
    if (has_dirs) { memcpy(member.dir_a, dir_a, 12); memcpy(member.dir_b, dir_b, 12); }
    member.has_com_bisector = has_com_bisector;
    if (has_com_bisector) memcpy(member.com_bisector, com_bisector, 12);
    member.name = name ? (char*)name : NULL;  // by-pointer only; deep copy happens in pentry_push_member

    if (n_match == 0) {
        // New entry.
        pentry_t* e = (pentry_t*)calloc(1, sizeof(*e));
        if (!e) { free(match_idx); return NULL; }
        e->id = pool->next_id++;
        e->x = mx; e->y = my; e->z = mz;
        e->source = source;
        e->name = name ? strdup(name) : (char*)NULL;
        if (has_plane_id) {
            if (push_uint_unique(&e->plane_ids, &e->n_plane_ids, &e->plane_ids_cap, plane_id) != 0) {
                pentry_free(e); free(e); free(match_idx); return NULL;
            }
        }
        if (has_ridge_id) {
            if (push_uint_unique(&e->ridge_ids, &e->n_ridge_ids, &e->ridge_ids_cap, ridge_id) != 0) {
                pentry_free(e); free(e); free(match_idx); return NULL;
            }
        }
        if (has_plane_id && has_dirs) {
            if (pentry_add_dirs(e, plane_id, dir_a, dir_b, DIRS_REPLACE) != 0) {
                pentry_free(e); free(e); free(match_idx); return NULL;
            }
        }
        if (has_plane_id && has_com_bisector) {
            if (pentry_add_com_bis(e, plane_id, com_bisector, DIRS_REPLACE) != 0) {
                pentry_free(e); free(e); free(match_idx); return NULL;
            }
        }
        if (pentry_push_member(e, &member) != 0) {
            pentry_free(e); free(e); free(match_idx); return NULL;
        }
        recompute_pool_entry(e);
        if (pool_push_entry(pool, e) != 0) {
            pentry_free(e); free(e); free(match_idx); return NULL;
        }
        free(match_idx);
        return e;
    }

    // Super-merge: fold matches[1..] into matches[0].
    pentry_t* primary = pool->entries[match_idx[0]];
    for (uint32_t i = 1; i < n_match; i++) {
        pentry_t* other = pool->entries[match_idx[i]];
        if (!other || other->consumed) continue;
        for (uint32_t k = 0; k < other->n_members; k++) {
            if (pentry_push_member(primary, &other->members[k]) != 0) { free(match_idx); return NULL; }
        }
        for (uint32_t k = 0; k < other->n_plane_ids; k++) {
            push_uint_unique(&primary->plane_ids, &primary->n_plane_ids, &primary->plane_ids_cap, other->plane_ids[k]);
        }
        for (uint32_t k = 0; k < other->n_ridge_ids; k++) {
            push_uint_unique(&primary->ridge_ids, &primary->n_ridge_ids, &primary->ridge_ids_cap, other->ridge_ids[k]);
        }
        for (uint32_t k = 0; k < other->n_dirs; k++) {
            pentry_add_dirs(primary, other->dirs[k].plane_id, other->dirs[k].dir_a, other->dirs[k].dir_b, DIRS_IF_ABSENT);
        }
        for (uint32_t k = 0; k < other->n_com_bis; k++) {
            pentry_add_com_bis(primary, other->com_bis[k].plane_id, other->com_bis[k].com_bisector, DIRS_IF_ABSENT);
        }
        char* nm = extend_name(primary->name, other->name);
        free(primary->name); primary->name = nm;
        pool_set_redirect(pool, other->id, primary->id);
        other->consumed = true;
        pentry_free(other);
        free(other);
        pool->entries[match_idx[i]] = NULL;
    }
    if (pentry_push_member(primary, &member) != 0) { free(match_idx); return NULL; }
    if (has_plane_id) push_uint_unique(&primary->plane_ids, &primary->n_plane_ids, &primary->plane_ids_cap, plane_id);
    if (has_plane_id && has_dirs) pentry_add_dirs(primary, plane_id, dir_a, dir_b, DIRS_REPLACE);
    if (has_plane_id && has_com_bisector) pentry_add_com_bis(primary, plane_id, com_bisector, DIRS_REPLACE);
    if (has_ridge_id) push_uint_unique(&primary->ridge_ids, &primary->n_ridge_ids, &primary->ridge_ids_cap, ridge_id);
    char* nm = extend_name(primary->name, name);
    free(primary->name); primary->name = nm;
    recompute_pool_entry(primary);
    free(match_idx);
    return primary;
}
#endif  // moved-to-v7-corner-pool block (pool registry + upsert)

// memberFromAnchor — translate anchor → pmember (filling authority,
// axis, ridgeId per v2 logic).
static void member_from_anchor(const anchor_t* a,
                                const v7_bc_input_ridge_line_t* ridge_lines, size_t n_rl,
                                pmember_t* out) {
    memset(out, 0, sizeof(*out));
    out->x = a->x; out->y = a->y; out->z = a->z;
    out->source = a->source;
    out->name = a->name;
    out->plane_id = a->plane_id; out->has_plane_id = true;
    if (a->has_dirs) {
        out->has_dirs = true;
        memcpy(out->dir_a, a->dir_a, 12);
        memcpy(out->dir_b, a->dir_b, 12);
    }
    if (a->has_com_bisector) {
        out->has_com_bisector = true;
        memcpy(out->com_bisector, a->com_bisector, 12);
    }
    // Casts: v7_bc_authority_t and v7p_authority_t share values but are
    // distinct enum types; pmember_t.authority is the pool type post-
    // refactor. Identical values so the cast is bit-exact.
    if (a->source == SRC_RIDGE_INTERSECTION) {
        out->authority = (v7p_authority_t)V7_BC_AUTHORITY_FULL;
    } else if (a->source == SRC_RIDGE_END && a->has_ridge_id && a->ridge_id < n_rl
               && ridge_lines[a->ridge_id].has_line) {
        out->authority = (v7p_authority_t)V7_BC_AUTHORITY_AXIAL;
        out->has_ridge_id = true; out->ridge_id = a->ridge_id;
        out->has_axis = true;
        out->axis[0] = (float)ridge_lines[a->ridge_id].line_direction[0];
        out->axis[1] = (float)ridge_lines[a->ridge_id].line_direction[1];
        out->axis[2] = (float)ridge_lines[a->ridge_id].line_direction[2];
    } else {
        out->authority = (v7p_authority_t)V7_BC_AUTHORITY_NONE;
    }
    if (!out->has_ridge_id && a->has_ridge_id) {
        out->has_ridge_id = true; out->ridge_id = a->ridge_id;
    }
}

// registerAnchorInPool — same semantics as v2: prefer existing
// pool_id (RI) → just enrich; else upsert by spatial merge.
static pentry_t* register_anchor_in_pool(ppool_t* pool, anchor_t* a,
                                          uint32_t plane_id, uint8_t plane_type,
                                          const v7_bc_input_ridge_line_t* ridge_lines, size_t n_rl,
                                          double pool_merge_dist) {
    if (a->has_pool_id) {
        pentry_t* e = pool_find_by_id(pool, a->pool_id);
        if (e) {
            // Decouple gate: refuse enriching a corner with an incompatible-type plane
            // (belt to the coplanar-source gate; also covers pre-assigned RI/RE pool ids).
            if (!rg_plane_types_mergeable((rg_plane_type_t)plane_type, (rg_plane_type_t)e->plane_type)) {
                return e;
            }
            // Coplanar-import path: the anchor reuses an existing pool entry
            // but carries its own per-plane source (typically SRC_COPLANAR).
            // entry_register_plane keeps the per-plane label distinct from
            // the entry's primary/global source.
            entry_register_plane(e, plane_id, a->source);
            if (a->has_dirs) pentry_add_dirs(e, plane_id, a->dir_a, a->dir_b, DIRS_REPLACE);
            if (a->has_com_bisector) pentry_add_com_bis(e, plane_id, a->com_bisector, DIRS_REPLACE);
            char* nm = extend_name(e->name, a->name);
            free(e->name); e->name = nm;
            a->pool_id = e->id;
            // Sync anchor name with pool (v2: a.name = p.name).
            if (e->name) {
                free(a->name);
                a->name = strdup(e->name);
            }
            // Maintain entry invariants (n_merged, cached e->source). The
            // coplanar-import path expands plane_ids[] / source_per_plane[]
            // without pushing a new member, so recompute is the single point
            // that re-derives e->source from source_per_plane[] (per #316).
            recompute_pool_entry(e);
            return e;
        }
    }
    pmember_t m;
    member_from_anchor(a, ridge_lines, n_rl, &m);
    pentry_t* e = upsert_pool_member(pool,
        m.x, m.y, m.z, m.source, m.name,
        plane_id, m.has_plane_id,
        m.has_dirs, m.dir_a, m.dir_b,
        m.has_com_bisector, m.com_bisector,
        (v7_bc_authority_t)m.authority, m.has_axis, m.axis,
        m.has_ridge_id, m.ridge_id,
        plane_type,
        pool_merge_dist);
    if (e) {
        a->has_pool_id = true;
        a->pool_id = e->id;
        if (e->name) { free(a->name); a->name = strdup(e->name); }
    }
    return e;
}

// ── Build per-plane group point lists ────────────────────────────────

static int build_group_points(const int16_t* owner, size_t n_pts,
                                size_t n_planes, uint32_t** out_lists_idx,
                                uint32_t* out_counts) {
    // owner[i] is the *plane index in the local planes array* (0..K).
    // Returns owned per-plane index arrays. Caller frees each entry of out_lists_idx.
    for (size_t i = 0; i < n_pts; i++) {
        int16_t o = owner[i];
        if (o >= 0 && (size_t)o < n_planes) out_counts[o]++;
    }
    for (size_t g = 0; g < n_planes; g++) {
        if (out_counts[g] > 0) {
            out_lists_idx[g] = (uint32_t*)malloc(out_counts[g] * sizeof(uint32_t));
            if (!out_lists_idx[g]) return -1;
        }
    }
    uint32_t* cursors = (uint32_t*)safe_calloc_array(n_planes, sizeof(uint32_t));
    if (!cursors) return -1;
    for (uint32_t i = 0; i < n_pts; i++) {
        int16_t o = owner[i];
        if (o >= 0 && (size_t)o < n_planes) {
            out_lists_idx[o][cursors[o]++] = i;
        }
    }
    free(cursors);
    return 0;
}

// ── Plane lookup by id (linear; small N) ────────────────────────────

static int find_plane_idx_by_id(const v7_bc_input_plane_t* planes, size_t n, uint32_t pid) {
    for (size_t i = 0; i < n; i++) if (planes[i].id == pid) return (int)i;
    return -1;
}

static int find_grid_idx_by_pid(const v7_bc_input_plane_grid_t* grids, size_t n, uint32_t pid) {
    for (size_t i = 0; i < n; i++) if (grids[i].plane_id == pid) return (int)i;
    return -1;
}

// ── Sample helpers (matches v2 D.1b sampleComDir) ───────────────────

// Sample (outU, outV) from a single grid field at (u,v). Returns true
// if the cell has a non-NaN oa.
static bool sample_outward(const v7_bc_input_grid_field_t* gf, float u, float v, float out[2]) {
    if (!gf || !gf->oa_field) return false;
    int col = (int)floorf((u - gf->g_min_u) / gf->cell_size);
    int row = (int)floorf((v - gf->g_min_v) / gf->cell_size);
    if (col < 0 || (uint32_t)col >= gf->n_cols || row < 0 || (uint32_t)row >= gf->n_rows) return false;
    size_t idx = (size_t)row * gf->n_cols + col;
    float oa = gf->oa_field[idx];
    if (isnan(oa)) return false;
    out[0] = gf->out_u[idx];
    out[1] = gf->out_v[idx];
    return true;
}

// ── Public compute ──────────────────────────────────────────────────

int v7_bc_compute(const double* positions, size_t n_pts_local,
                   const int16_t* owner,
                   const v7_bc_input_plane_t* planes, size_t n_planes,
                   const v7_bc_input_ridge_line_t* ridge_lines, size_t n_ridge_lines,
                   const v7_bc_input_plane_grid_t* grids, size_t n_grids,
                   const v7_bc_input_corner_t* seed_corners, size_t n_seed_corners,
                   const v7_bc_params_t* params,
                   v7_bc_outputs_t* out) {
    if (!params || !out) return -1;
    v7_bc_outputs_init(out);
    if (n_planes == 0) return 0;

    // Build per-plane group points list (indexed by plane *array index*).
    uint32_t** group_idx = (uint32_t**)safe_calloc_array(n_planes, sizeof(uint32_t*));
    uint32_t* group_n    = (uint32_t*)safe_calloc_array(n_planes, sizeof(uint32_t));
    if (!group_idx || !group_n) { free(group_idx); free(group_n); return -1; }
    if (build_group_points(owner, n_pts_local, n_planes, group_idx, group_n) != 0) {
        for (size_t i = 0; i < n_planes; i++) free(group_idx[i]);
        free(group_idx); free(group_n);
        return -1;
    }

    // Build per-plane ridge list (planeridge_t).
    planeridge_t** prs = (planeridge_t**)safe_calloc_array(n_planes, sizeof(planeridge_t*));
    uint32_t* prs_n     = (uint32_t*)safe_calloc_array(n_planes, sizeof(uint32_t));
    if (!prs || !prs_n) {
        for (size_t i = 0; i < n_planes; i++) free(group_idx[i]);
        free(group_idx); free(group_n); free(prs); free(prs_n); return -1;
    }
    for (size_t rli = 0; rli < n_ridge_lines; rli++) {
        const v7_bc_input_ridge_line_t* rl = &ridge_lines[rli];
        assert(rl->has_line && "topology must set has_line on every emitted ridge — "
                                "see v7-boundary-topology-impl.c (the has_side bug pattern: "
                                "consumer silently skipped, producer never set the bit)");
        if (!rl->has_line) continue;  // release-build belt-and-suspenders
        double t_min = (rl->t_min_a > rl->t_min_b) ? rl->t_min_a : rl->t_min_b;
        double t_max = (rl->t_max_a < rl->t_max_b) ? rl->t_max_a : rl->t_max_b;
        if (t_max <= t_min) continue;
        planeridge_t pr = {0};
        pr.from[0] = rl->line_point[0] + t_min * rl->line_direction[0];
        pr.from[1] = rl->line_point[1] + t_min * rl->line_direction[1];
        pr.from[2] = rl->line_point[2] + t_min * rl->line_direction[2];
        pr.to[0]   = rl->line_point[0] + t_max * rl->line_direction[0];
        pr.to[1]   = rl->line_point[1] + t_max * rl->line_direction[1];
        pr.to[2]   = rl->line_point[2] + t_max * rl->line_direction[2];
        pr.point[0] = rl->line_point[0]; pr.point[1] = rl->line_point[1]; pr.point[2] = rl->line_point[2];
        pr.direction[0] = rl->line_direction[0]; pr.direction[1] = rl->line_direction[1]; pr.direction[2] = rl->line_direction[2];
        pr.t_min = t_min; pr.t_max = t_max;
        pr.g_a = rl->plane_a; pr.g_b = rl->plane_b;
        pr.ridge_line_idx = (uint32_t)rli;
        // Push to plane_a + plane_b (lookup by id).
        for (int side = 0; side < 2; side++) {
            uint32_t pid = (side == 0) ? rl->plane_a : rl->plane_b;
            int idx = find_plane_idx_by_id(planes, n_planes, pid);
            if (idx < 0) continue;
            if (prs_n[idx] == 0 || (prs_n[idx] & (prs_n[idx] - 1)) == 0) {
                // grow capacity at power-of-2 boundaries.
                size_t nc = prs_n[idx] ? prs_n[idx] * 2 : 4;
                planeridge_t* p = (planeridge_t*)safe_realloc_array(prs[idx], nc, sizeof(planeridge_t));
                if (!p) {
                    for (size_t i = 0; i < n_planes; i++) { free(group_idx[i]); free(prs[i]); }
                    free(group_idx); free(group_n); free(prs); free(prs_n);
                    return -1;
                }
                prs[idx] = p;
            }
            prs[idx][prs_n[idx]++] = pr;
        }
    }

    // Name counters bootstrapped from seed corners.
    nctrs_t nctrs = {0};
    seed_nctrs_from_corners(&nctrs, seed_corners, n_seed_corners);

    // ── Seed pool from non-candidate (RI) seed corners + harvest per-
    //    plane anchors from candidate corners' dirsByPlane.
    ppool_t pool = {0};

    // Per-plane anchor lists (we use a dynamic array per plane).
    // Audit hardening (safe_calloc_array) kept; main's BC_COUNTS/BC_SNAP
    // diagnostic accumulators retained below.
    anchor_t** anchors = (anchor_t**)safe_calloc_array(n_planes, sizeof(anchor_t*));
    uint32_t*  anchors_n = (uint32_t*)safe_calloc_array(n_planes, sizeof(uint32_t));
    uint32_t*  anchors_cap = (uint32_t*)safe_calloc_array(n_planes, sizeof(uint32_t));

    // BC_COUNTS / BC_SNAP diagnostic accumulators (per-plane; printed if env set).
    // Declared together with `anchors` so the fail/return paths can free them
    // unconditionally; allocated below.
    uint32_t* bc_snap_cc_tried   = NULL;
    uint32_t* bc_snap_cc_snapped = NULL;
    uint32_t* bc_snap_cv_tried   = NULL;
    uint32_t* bc_snap_cv_snapped = NULL;
    uint32_t* bc_kill_ri = NULL;
    uint32_t* bc_kill_re = NULL;
    uint32_t* bc_kill_pm = NULL;
    uint32_t* bc_kill_cc = NULL;
    uint32_t* bc_kill_cv = NULL;

    if (!anchors || !anchors_n || !anchors_cap) goto fail;

    bc_snap_cc_tried   = (uint32_t*)safe_calloc_array(n_planes, sizeof(uint32_t));
    bc_snap_cc_snapped = (uint32_t*)safe_calloc_array(n_planes, sizeof(uint32_t));
    bc_snap_cv_tried   = (uint32_t*)safe_calloc_array(n_planes, sizeof(uint32_t));
    bc_snap_cv_snapped = (uint32_t*)safe_calloc_array(n_planes, sizeof(uint32_t));
    bc_kill_ri = (uint32_t*)safe_calloc_array(n_planes, sizeof(uint32_t));
    bc_kill_re = (uint32_t*)safe_calloc_array(n_planes, sizeof(uint32_t));
    bc_kill_pm = (uint32_t*)safe_calloc_array(n_planes, sizeof(uint32_t));
    bc_kill_cc = (uint32_t*)safe_calloc_array(n_planes, sizeof(uint32_t));
    bc_kill_cv = (uint32_t*)safe_calloc_array(n_planes, sizeof(uint32_t));
    if (!bc_snap_cc_tried || !bc_snap_cc_snapped || !bc_snap_cv_tried || !bc_snap_cv_snapped
        || !bc_kill_ri || !bc_kill_re || !bc_kill_pm || !bc_kill_cc || !bc_kill_cv) goto fail;

#define PUSH_ANCHOR(_g, _a)                                                                            \
    do {                                                                                                \
        if (anchors_n[_g] == anchors_cap[_g]) {                                                         \
            uint32_t nc = anchors_cap[_g] ? anchors_cap[_g] * 2 : 4;                                    \
            anchor_t* p = (anchor_t*)safe_realloc_array(anchors[_g], nc, sizeof(anchor_t));                       \
            if (!p) goto fail;                                                                          \
            anchors[_g] = p; anchors_cap[_g] = nc;                                                      \
        }                                                                                               \
        anchors[_g][anchors_n[_g]++] = (_a);                                                            \
    } while (0)

    // Step 1: seed pool from non-candidate corners (RI from upstream).
    // The v2 input has 'ridge-intersection' source; we set authority=full.
    // For each seed, we ALSO want to remember its initial pool id → entry mapping
    // so the registerAnchorInPool path can find them.
    for (size_t i = 0; i < n_seed_corners; i++) {
        const v7_bc_input_corner_t* s = &seed_corners[i];
        if (s->source == SRC_CONCAVE || s->source == SRC_CONVEX) {
            // Harvest as per-plane anchor in candidate-of-plane buckets.
            // v2: anchors land on every planeId of the candidate (typically just one).
            for (uint32_t k = 0; k < s->n_plane_ids; k++) {
                int idx = find_plane_idx_by_id(planes, n_planes, s->plane_ids[k]);
                if (idx < 0) continue;
                anchor_t a = {0};
                a.x = s->point[0]; a.y = s->point[1]; a.z = s->point[2];
                a.source = s->source;
                a.plane_id = s->plane_ids[k];
                a.name = s->name ? strdup(s->name) : (char*)NULL;
                // Find dirsByPlane entry matching this plane.
                for (uint32_t d = 0; d < s->n_dirs; d++) {
                    if (s->dirs[d].plane_id == s->plane_ids[k]) {
                        a.has_dirs = true;
                        memcpy(a.dir_a, s->dirs[d].dir_a, 12);
                        memcpy(a.dir_b, s->dirs[d].dir_b, 12);
                        break;
                    }
                }
                // Find comBisectorByPlane entry matching this plane.
                for (uint32_t d = 0; d < s->n_com_bisectors; d++) {
                    if (s->com_bisectors[d].plane_id == s->plane_ids[k]) {
                        a.has_com_bisector = true;
                        memcpy(a.com_bisector, s->com_bisectors[d].com_bisector, 12);
                        break;
                    }
                }
                PUSH_ANCHOR((uint32_t)idx, a);
            }
        } else {
            // Seed pool: create entry directly (authority full for RI).
            pmember_t m = {0};
            m.x = s->point[0]; m.y = s->point[1]; m.z = s->point[2];
            m.source = s->source;
            m.authority = (s->source == SRC_RIDGE_INTERSECTION)
                            ? (v7p_authority_t)V7_BC_AUTHORITY_FULL
                            : (v7p_authority_t)V7_BC_AUTHORITY_NONE;
            m.has_ridge_id = (s->n_ridge_ids > 0);
            if (m.has_ridge_id) m.ridge_id = s->ridge_ids[0];
            m.name = s->name;
            // For RI authority=full, we don't have axis; that's fine.
            pentry_t* e = upsert_pool_member(&pool,
                m.x, m.y, m.z, m.source, m.name,
                0, false,
                false, NULL, NULL,
                false, NULL,
                (v7_bc_authority_t)m.authority, false, NULL,
                m.has_ridge_id, m.ridge_id,
                RG_PLANE_TYPE_UNCLASSIFIED,  // RI seed: no plane yet (gate skipped); roof crease
                params->pool_merge_dist);
            if (!e) goto fail;
            // Seed corners (RI) inherit the seed's source for every plane they touch.
            // Decouple: the FIRST plane sets the RI corner's type; a plane of an
            // incompatible type (an eave-meets-ridge junction) is NOT registered,
            // so the roof crease and the wall stay separate corners.
            uint8_t _ri_type = RG_PLANE_TYPE_UNCLASSIFIED;
            for (uint32_t k = 0; k < s->n_plane_ids; k++) {
                uint8_t kt = RG_PLANE_TYPE_UNCLASSIFIED;
                for (size_t _pi = 0; _pi < n_planes; _pi++)
                    if (planes[_pi].id == s->plane_ids[k]) { kt = planes[_pi].plane_type; break; }
                if (_ri_type == RG_PLANE_TYPE_UNCLASSIFIED) _ri_type = kt;
                if (!rg_plane_types_mergeable((rg_plane_type_t)kt, (rg_plane_type_t)_ri_type)) continue;
                entry_register_plane(e, s->plane_ids[k], s->source);
            }
            e->plane_type = _ri_type;
            for (uint32_t k = 0; k < s->n_ridge_ids; k++) {
                push_uint_unique(&e->ridge_ids, &e->n_ridge_ids, &e->ridge_ids_cap, s->ridge_ids[k]);
            }
        }
    }

    // ── Per-plane pass (D.0, D.1a, D.1b, D.2, D.2b) ────────────────────
    for (size_t g = 0; g < n_planes; g++) {
        const v7_bc_input_plane_t* P = &planes[g];
        if (P->rejected) continue;
        // Look up plane grid for this plane. NULL is allowed: a plane without
        // a grid (boundary-signals filtered it as too small/marginal) still
        // gets per-plane work — D.0 wall-bbox doesn't need a grid, and D.1b's
        // sampleComDir is null-safe via the PG/has_group guard below.
        int gi = find_grid_idx_by_pid(grids, n_grids, P->id);
        const v7_bc_input_plane_grid_t* PG = (gi >= 0) ? &grids[gi] : NULL;

        // ── D.0 Wall-bbox ──
        if (P->tilt_deg >= params->wall_tilt_min_deg) {
            double nx = P->normal[0], ny = P->normal[1], nz = P->normal[2];
            // v2 builds eh = (nz, 0, -nx) — only valid for y-up. For z-up,
            // analogous would be (ny, -nx, 0). Use the up_axis param.
            double ehx, ehy, ehz, evx, evy, evz;
            if (params->up_axis == 2) {
                // z-up: horizontal direction in xy plane perpendicular to n.
                ehx = ny; ehy = -nx; ehz = 0;
            } else {
                ehx = nz; ehy = 0; ehz = -nx;
            }
            double eh_len = vlen3(ehx, ehy, ehz); if (eh_len < 1e-9) eh_len = 1;
            ehx /= eh_len; ehy /= eh_len; ehz /= eh_len;
            // ev = vertical-axis basis pointing world-UP. The chirality
            // of eh×n vs n×eh depends on which axis is up (it's a
            // function of which `eh` we chose). Compute per up_axis so
            // ev.up_component is always positive — otherwise the corner
            // labels (BL/BR vs LT/RT) get mirrored and the "bottom"
            // anchors plant at the wall's TOP edge. Previously the code
            // used n×eh for both branches, which broke z-up silently.
            if (params->up_axis == 2) {
                // z-up: ev = eh × n  →  ev.z = nx² + ny² ≥ 0
                evx = ehy*nz - ehz*ny;  evy = ehz*nx - ehx*nz;  evz = ehx*ny - ehy*nx;
            } else {
                // y-up: ev = n × eh  →  ev.y = nx² + nz² ≥ 0
                evx = ny*ehz - nz*ehy;  evy = nz*ehx - nx*ehz;  evz = nx*ehy - ny*ehx;
            }
            double ev_len = vlen3(evx, evy, evz); if (ev_len < 1e-9) ev_len = 1;
            evx /= ev_len; evy /= ev_len; evz /= ev_len;
            const uint32_t n_pts = group_n[g];
            const uint32_t* pts = group_idx[g];
            if (n_pts >= 3) {
                double h_min = INFINITY, h_max = -INFINITY, v_min = INFINITY;
                float* ptH = (float*)safe_malloc_array(n_pts, sizeof(float));
                float* ptV = (float*)safe_malloc_array(n_pts, sizeof(float));
                if (!ptH || !ptV) { free(ptH); free(ptV); goto fail; }
                for (uint32_t i = 0; i < n_pts; i++) {
                    uint32_t pi = pts[i];
                    double px = positions[pi*3 + 0], py = positions[pi*3 + 1], pz = positions[pi*3 + 2];
                    double h = px*ehx + py*ehy + pz*ehz;
                    double v = px*evx + py*evy + pz*evz;
                    ptH[i] = (float)h; ptV[i] = (float)v;
                    if (h < h_min) h_min = h;
                    if (h > h_max) h_max = h;
                    if (v < v_min) v_min = v;
                }
                double left_top_v = v_min, right_top_v = v_min;
                for (uint32_t i = 0; i < n_pts; i++) {
                    if (ptH[i] < h_min + params->wall_side_margin && ptV[i] > left_top_v) left_top_v = ptV[i];
                    if (ptH[i] > h_max - params->wall_side_margin && ptV[i] > right_top_v) right_top_v = ptV[i];
                }
                if (h_max - h_min >= params->wall_min_h_extent && isfinite(v_min)) {
                    double cx = 0, cy = 0, cz = 0;
                    for (uint32_t i = 0; i < n_pts; i++) {
                        uint32_t pi = pts[i];
                        cx += positions[pi*3 + 0]; cy += positions[pi*3 + 1]; cz += positions[pi*3 + 2];
                    }
                    cx /= n_pts; cy /= n_pts; cz /= n_pts;
                    double cH = cx*ehx + cy*ehy + cz*ehz;
                    double cV = cx*evx + cy*evy + cz*evz;
                    // Corners: BL, BR, LT, RT.
                    float da_pos[3] = { (float)ehx, (float)ehy, (float)ehz };
                    float da_neg[3] = { -(float)ehx, -(float)ehy, -(float)ehz };
                    float db_pos[3] = { (float)evx, (float)evy, (float)evz };
                    float db_neg[3] = { -(float)evx, -(float)evy, -(float)evz };
                    struct wall_corner { double h, v; const float* da; const float* db; };
                    struct wall_corner corners4[4] = {
                        { h_min, v_min,       da_pos, db_pos },
                        { h_max, v_min,       da_neg, db_pos },
                        { h_min, left_top_v,  da_pos, db_neg },
                        { h_max, right_top_v, da_neg, db_neg },
                    };
                    for (int k = 0; k < 4; k++) {
                        double h = corners4[k].h, v = corners4[k].v;
                        anchor_t a = {0};
                        a.x = cx + (h - cH) * ehx + (v - cV) * evx;
                        a.y = cy + (h - cH) * ehy + (v - cV) * evy;
                        a.z = cz + (h - cH) * ehz + (v - cV) * evz;
                        a.source = SRC_CONCAVE;
                        a.plane_id = P->id;
                        a.has_dirs = true;
                        memcpy(a.dir_a, corners4[k].da, 12);
                        memcpy(a.dir_b, corners4[k].db, 12);
                        a.wall_bbox = true;
                        a.name = name_for(&nctrs, SRC_CONCAVE, P->id);
                        PUSH_ANCHOR((uint32_t)g, a);
                        out->wall_corners_added++;
                    }
                }
                free(ptH); free(ptV);
            }
        }

        // ── D.1a Ridge intersections — consume from existing pool ──
        // Track which endpoints are covered. ridgeCovered[ri].{from,to}_used.
        uint8_t* ridge_cov_from = (uint8_t*)calloc(prs_n[g], 1);
        uint8_t* ridge_cov_to   = (uint8_t*)calloc(prs_n[g], 1);
        if (prs_n[g] && (!ridge_cov_from || !ridge_cov_to)) { free(ridge_cov_from); free(ridge_cov_to); goto fail; }

        // Build a "ridge line index → local ridge slot" map.
        // (small N: linear scan acceptable)
        for (uint32_t i = 0; i < pool.n_entries; i++) {
            pentry_t* cn = pool.entries[i];
            if (!cn || cn->consumed) continue;
            if (cn->source != SRC_RIDGE_INTERSECTION) continue;
            // does this RI corner belong to plane g?
            bool belongs = false;
            for (uint32_t k = 0; k < cn->n_plane_ids; k++) {
                if (cn->plane_ids[k] == P->id) { belongs = true; break; }
            }
            if (!belongs) continue;
            // find which local ridges share this RI's ridge ids.
            uint32_t local_ri[8]; uint32_t n_local = 0;
            for (uint32_t k = 0; k < cn->n_ridge_ids && n_local < 8; k++) {
                for (uint32_t lr = 0; lr < prs_n[g]; lr++) {
                    if (prs[g][lr].ridge_line_idx == cn->ridge_ids[k]) {
                        local_ri[n_local++] = lr;
                        break;
                    }
                }
            }
            if (n_local < 2) continue;
            // Mark coverage on each local ridge — distance from cn to endpoint.
            for (uint32_t k = 0; k < n_local; k++) {
                const planeridge_t* r = &prs[g][local_ri[k]];
                double df = vlen3(r->from[0]-cn->x, r->from[1]-cn->y, r->from[2]-cn->z);
                double dt = vlen3(r->to[0]-cn->x,   r->to[1]-cn->y,   r->to[2]-cn->z);
                if (df < dt) ridge_cov_from[local_ri[k]] = 1;
                else         ridge_cov_to[local_ri[k]]   = 1;
            }
            // Compute dirs from first two local ridges' midpoints.
            const planeridge_t* rA = &prs[g][local_ri[0]];
            const planeridge_t* rB = &prs[g][local_ri[1]];
            double aMx = (rA->from[0] + rA->to[0]) * 0.5;
            double aMy = (rA->from[1] + rA->to[1]) * 0.5;
            double aMz = (rA->from[2] + rA->to[2]) * 0.5;
            double bMx = (rB->from[0] + rB->to[0]) * 0.5;
            double bMy = (rB->from[1] + rB->to[1]) * 0.5;
            double bMz = (rB->from[2] + rB->to[2]) * 0.5;
            double adx = aMx - cn->x, ady = aMy - cn->y, adz = aMz - cn->z;
            double bdx = bMx - cn->x, bdy = bMy - cn->y, bdz = bMz - cn->z;
            double aLen = vlen3(adx, ady, adz); if (aLen < 1e-9) aLen = 1;
            double bLen = vlen3(bdx, bdy, bdz); if (bLen < 1e-9) bLen = 1;
            anchor_t a = {0};
            a.x = cn->x; a.y = cn->y; a.z = cn->z;
            a.has_dirs = true;
            a.dir_a[0] = (float)(adx / aLen); a.dir_a[1] = (float)(ady / aLen); a.dir_a[2] = (float)(adz / aLen);
            a.dir_b[0] = (float)(bdx / bLen); a.dir_b[1] = (float)(bdy / bLen); a.dir_b[2] = (float)(bdz / bLen);
            a.source = SRC_RIDGE_INTERSECTION;
            a.plane_id = P->id;
            a.has_pool_id = true; a.pool_id = cn->id;
            a.name = cn->name ? strdup(cn->name) : (char*)NULL;
            // RI sits on every ridge whose intersection it represents.
            for (uint32_t k = 0; k < cn->n_ridge_ids; k++) {
                anchor_push_on_ridge(&a, cn->ridge_ids[k]);
            }
            PUSH_ANCHOR((uint32_t)g, a);
        }

        // ── D.1b Ridge-end corners (no intersection coverage → end corners) ──
        const v7_bc_input_grid_field_t* gField = (PG && PG->has_group) ? &PG->group_field : NULL;
        for (uint32_t ri = 0; ri < prs_n[g]; ri++) {
            const planeridge_t* r = &prs[g][ri];
            for (int end_side = 0; end_side < 2; end_side++) {
                const double* end_pt   = (end_side == 0) ? r->from : r->to;
                const double* other_pt = (end_side == 0) ? r->to   : r->from;
                bool used = (end_side == 0) ? ridge_cov_from[ri] : ridge_cov_to[ri];
                if (used) continue;
                // ridgeDir = end_pt → other_pt.
                double dx = other_pt[0] - end_pt[0];
                double dy = other_pt[1] - end_pt[1];
                double dz = other_pt[2] - end_pt[2];
                double dl = vlen3(dx, dy, dz); if (dl < 1e-9) dl = 1;
                float rd[3] = { (float)(dx/dl), (float)(dy/dl), (float)(dz/dl) };
                // Sample (u, v) at end_pt.
                float u = (float)(end_pt[0]*P->basis_e1[0] + end_pt[1]*P->basis_e1[1] + end_pt[2]*P->basis_e1[2]);
                float v = (float)(end_pt[0]*P->basis_e2[0] + end_pt[1]*P->basis_e2[1] + end_pt[2]*P->basis_e2[2]);
                // sampleComDir: gField outward minus nField outward (here we don't have nField — gField only).
                // v2: hasG && hasN → cU = gU - nU; only-g → cU = gU. We only have group.
                float comdir_uv[2] = {0, 0};
                bool has_com = sample_outward(gField, u, v, comdir_uv);
                float second_dir[3] = {0,0,0}; bool has_second = false;
                if (has_com) {
                    float l = sqrtf(comdir_uv[0]*comdir_uv[0] + comdir_uv[1]*comdir_uv[1]);
                    if (l >= 1e-6f) {
                        comdir_uv[0] /= l; comdir_uv[1] /= l;
                        // ridgeDir → (u,v).
                        float rU = (float)(rd[0]*P->basis_e1[0] + rd[1]*P->basis_e1[1] + rd[2]*P->basis_e1[2]);
                        float rV = (float)(rd[0]*P->basis_e2[0] + rd[1]*P->basis_e2[1] + rd[2]*P->basis_e2[2]);
                        float dot = rU*comdir_uv[0] + rV*comdir_uv[1];
                        float refU = 2 * dot * comdir_uv[0] - rU;
                        float refV = 2 * dot * comdir_uv[1] - rV;
                        float rl2 = sqrtf(refU*refU + refV*refV);
                        if (rl2 >= 1e-6f) {
                            refU /= rl2; refV /= rl2;
                            second_dir[0] = (float)(refU*P->basis_e1[0] + refV*P->basis_e2[0]);
                            second_dir[1] = (float)(refU*P->basis_e1[1] + refV*P->basis_e2[1]);
                            second_dir[2] = (float)(refU*P->basis_e1[2] + refV*P->basis_e2[2]);
                            has_second = true;
                        }
                    }
                }
                anchor_t a = {0};
                a.x = end_pt[0]; a.y = end_pt[1]; a.z = end_pt[2];
                a.source = SRC_RIDGE_END;
                a.plane_id = P->id;
                a.has_dirs = true;
                memcpy(a.dir_a, rd, 12);
                if (has_second) memcpy(a.dir_b, second_dir, 12);
                a.has_ridge_id = true; a.ridge_id = r->ridge_line_idx;
                // RE sits ON this ridge by construction (it's an endpoint).
                anchor_push_on_ridge(&a, r->ridge_line_idx);
                a.name = name_for(&nctrs, SRC_RIDGE_END, P->id);
                PUSH_ANCHOR((uint32_t)g, a);
            }
        }

        // ── D.2 Ridge snap ──
        const char* bct_d2 = getenv("BC_TRACE");
        bool trace_d2 = (bct_d2 && (uint32_t)atoi(bct_d2) == P->id);
        if (prs_n[g] > 0) {
            for (uint32_t i = 0; i < anchors_n[g]; i++) {
                anchor_t* a = &anchors[g][i];
                if (a->source == SRC_RIDGE_INTERSECTION || a->source == SRC_RIDGE_END) continue;
                double best = INFINITY, bx = 0, by = 0, bz = 0; uint32_t bri = UINT32_MAX;
                for (uint32_t ri = 0; ri < prs_n[g]; ri++) {
                    const planeridge_t* r = &prs[g][ri];
                    double dxr = a->x - r->point[0], dyr = a->y - r->point[1], dzr = a->z - r->point[2];
                    double t = dxr*r->direction[0] + dyr*r->direction[1] + dzr*r->direction[2];
                    if (t < r->t_min) t = r->t_min; else if (t > r->t_max) t = r->t_max;
                    double cx = r->point[0] + t*r->direction[0];
                    double cy = r->point[1] + t*r->direction[1];
                    double cz = r->point[2] + t*r->direction[2];
                    double dd = vlen3(a->x-cx, a->y-cy, a->z-cz);
                    if (dd < best) { best = dd; bx = cx; by = cy; bz = cz; bri = r->ridge_line_idx; }
                }
                if (trace_d2 && a->source == SRC_CONVEX) {
                    fprintf(stderr, "[V3-BC] D2 plane=%u %s pre=(%.4f,%.4f,%.4f) bestDist=%.4f bestRidgeIdx=%u → %s\n",
                            P->id, a->name ? a->name : "?", a->x, a->y, a->z, best, bri,
                            best <= params->snap_radius ? "SNAP" : "no-snap");
                }
                if (a->source == SRC_CONCAVE) {
                    bc_snap_cc_tried[g]++;
                    if (best <= params->snap_radius) bc_snap_cc_snapped[g]++;
                } else if (a->source == SRC_CONVEX) {
                    bc_snap_cv_tried[g]++;
                    if (best <= params->snap_radius) bc_snap_cv_snapped[g]++;
                }
                if (best <= params->snap_radius) {
                    if (trace_d2 && a->source == SRC_CONVEX) {
                        fprintf(stderr, "[V3-BC]   post=(%.4f,%.4f,%.4f)\n", bx, by, bz);
                    }
                    a->x = bx; a->y = by; a->z = bz;
                    a->has_ridge_id = true; a->ridge_id = bri;
                    // After snap, anchor sits ON this ridge by construction.
                    anchor_push_on_ridge(a, bri);
                }
            }
        }

        // ── D.2b Ridge-side filter — kill wrong-side convex ──
        // BC_TRACE=N enables per-anchor trace for plane N's D.2b checks
        // (env-gated; no effect when unset). Used to diagnose why a
        // convex anchor v3 keeps but v2 kills (or vice versa).
        const char* bct = getenv("BC_TRACE");
        int trace_plane = bct ? atoi(bct) : -1;
        bool trace = (trace_plane >= 0 && (uint32_t)trace_plane == P->id);
        if (prs_n[g] > 0) {
            double rmaxsq = params->ridge_side_max_dist * params->ridge_side_max_dist;
            // Defense-in-depth perp threshold: an anchor whose perpendicular
            // distance to a ridge line is below this is treated as "on the
            // ridge" and the side test is skipped. Catches any FP-zero case
            // that the property gate (`anchor_on_ridge`) misses. 1 µm² ≪
            // snap_radius (0.30 m) ≪ ridge_side_max_dist (1.5 m), so this
            // gate never affects well-defined kills.
            const double eps_perp_sq = 1e-12;
            for (uint32_t i = 0; i < anchors_n[g]; i++) {
                anchor_t* a = &anchors[g][i];
                // Experiment 2026-05-18 (permanent): D.2b side test applies to
                // anchors of ANY source. RI/RE/snapped CC/CV sit ON ridges by
                // construction → side test on those ridges is meaningless
                // (perp = 0, side_d FP-coin-flips). Gated below by
                // anchor_on_ridge(a, r->ridge_line_idx).
                if (trace) {
                    fprintf(stderr, "[BC] D2b plane=%u anchor i=%u name=%s pt=(%.2f,%.2f,%.2f) n_ridges=%u\n",
                            P->id, i, a->name ? a->name : "?", a->x, a->y, a->z, prs_n[g]);
                }
                for (uint32_t ri = 0; ri < prs_n[g]; ri++) {
                    const planeridge_t* r = &prs[g][ri];
                    const v7_bc_input_ridge_line_t* rl = &ridge_lines[r->ridge_line_idx];
                    // Property gate: anchor sits ON this ridge by construction
                    // (RI's intersection set, RE's endpoint ridge, or D.2 snap
                    // target). Side test is mathematically zero — skip.
                    if (anchor_on_ridge(a, r->ridge_line_idx)) {
                        if (trace) fprintf(stderr, "[BC]   ridge=%u skip: on-ridge by construction\n", ri);
                        continue;
                    }
                    assert(rl->has_line && "topology must set has_line on every emitted ridge");
                    // Select THIS plane's own interior-oriented side axis:
                    // side_vec_a if our plane is plane_a, else side_vec_b. The
                    // old code used one side_vec (always plane_a's frame) for
                    // both planes, so a plane_b corner was judged in plane_a's
                    // tangent — on a steep dihedral that axis is ~orthogonal to
                    // the corner's offset and side_d collapses to FP noise
                    // (journal 2026-06-11-d2b-side-vec-wrong-plane-frame).
                    //
                    // has_side_* can legitimately be false: the cross product
                    // line_dir × normal is degenerate (line ∥ normal), or no
                    // near-ridge points yielded a definite interior side. Then
                    // the side test is undefined — skip.
                    const bool ours_a = (rl->plane_a == P->id);
                    const float* sv = ours_a ? rl->side_vec_a : rl->side_vec_b;
                    const bool has_side = ours_a ? rl->has_side_a : rl->has_side_b;
                    if (!rl->has_line || !has_side) {
                        if (trace) fprintf(stderr, "[BC]   ridge=%u skip: has_line=%d has_side=%d\n",
                                            ri, rl->has_line, has_side);
                        continue;
                    }
                    double dxr = a->x - rl->line_point[0], dyr = a->y - rl->line_point[1], dzr = a->z - rl->line_point[2];
                    double t = dxr*rl->line_direction[0] + dyr*rl->line_direction[1] + dzr*rl->line_direction[2];
                    double px = dxr - t*rl->line_direction[0];
                    double py = dyr - t*rl->line_direction[1];
                    double pz = dzr - t*rl->line_direction[2];
                    double perp_sq = px*px + py*py + pz*pz;
                    if (perp_sq > rmaxsq) {
                        if (trace) fprintf(stderr, "[BC]   ridge=%u (pA=%u pB=%u) skip: perp=%.2fm > %.2fm\n",
                                            ri, rl->plane_a, rl->plane_b, sqrt(perp_sq), params->ridge_side_max_dist);
                        continue;
                    }
                    // Defense-in-depth: if perp is below FP-noise threshold,
                    // anchor is effectively on the ridge (the property gate
                    // should have caught this, but covers data-model drift).
                    if (perp_sq < eps_perp_sq) {
                        if (trace) fprintf(stderr, "[BC]   ridge=%u (pA=%u pB=%u) skip: perp²=%.3e < eps² (on-ridge by FP)\n",
                                            ri, rl->plane_a, rl->plane_b, perp_sq);
                        continue;
                    }
                    // side_vec points INTO this plane's body, so the body side
                    // is positive. A negative projection means the corner is on
                    // the wrong side of the ridge → kill.
                    double anchor_side_d = sv[0]*dxr + sv[1]*dyr + sv[2]*dzr;
                    if (trace) fprintf(stderr, "[BC]   ridge=%u (pA=%u pB=%u) perp=%.2fm side_d=%.3f sv=(%.2f,%.2f,%.2f) → %s\n",
                                        ri, rl->plane_a, rl->plane_b, sqrt(perp_sq), anchor_side_d,
                                        sv[0], sv[1], sv[2],
                                        (anchor_side_d < 0) ? "KILL" : "keep");
                    if (anchor_side_d < 0) {
                        a->killed = true;
                        switch (a->source) {
                            case SRC_RIDGE_INTERSECTION: bc_kill_ri[g]++; break;
                            case SRC_RIDGE_END:          bc_kill_re[g]++; break;
                            case SRC_POLYGON_MERGE:      bc_kill_pm[g]++; break;
                            case SRC_CONCAVE:            bc_kill_cc[g]++; break;
                            case SRC_CONVEX:             bc_kill_cv[g]++; break;
                            default: break;
                        }
                        break;
                    }
                }
            }
        }
        free(ridge_cov_from); free(ridge_cov_to);
    }

    // ── Pool registration — walk surviving anchors per plane ──
    // BC_COUNTS env: print per-plane anchor counts + survivor stats before
    // pool registration. Used to diagnose v3-vs-v2 anchor-generation drift
    // (TODO-1 in tests/ISO-EQUIVALENCE-STATE.md).
    const char* bc_counts = getenv("BC_COUNTS");
    if (bc_counts) {
        size_t total_alive = 0, total_killed = 0, total_per_src[10] = {0};
        size_t tKri = 0, tKre = 0, tKpm = 0, tKcc = 0, tKcv = 0;
        size_t tSnapCCt = 0, tSnapCCs = 0, tSnapCVt = 0, tSnapCVs = 0;
        for (size_t g = 0; g < n_planes; g++) {
            uint32_t alive = 0, killed = 0, src_count[10] = {0};
            for (uint32_t i = 0; i < anchors_n[g]; i++) {
                if (anchors[g][i].killed) killed++;
                else {
                    alive++;
                    uint8_t s = anchors[g][i].source;
                    if (s < 10) src_count[s]++;
                }
            }
            if (alive > 0 || killed > 0) {
                fprintf(stderr,
                    "[BC_COUNTS] plane=%u alive=%u killed=%u "
                    "src(RI=%u,RE=%u,PM=%u,CC=%u,CV=%u) "
                    "kill(RI=%u,RE=%u,PM=%u,CC=%u,CV=%u) "
                    "snap(CC=%u/%u,CV=%u/%u)\n",
                    planes[g].id, alive, killed,
                    src_count[1], src_count[2], src_count[3], src_count[4], src_count[5],
                    bc_kill_ri[g], bc_kill_re[g], bc_kill_pm[g], bc_kill_cc[g], bc_kill_cv[g],
                    bc_snap_cc_snapped[g], bc_snap_cc_tried[g],
                    bc_snap_cv_snapped[g], bc_snap_cv_tried[g]);
            }
            total_alive += alive; total_killed += killed;
            for (int s = 0; s < 10; s++) total_per_src[s] += src_count[s];
            tKri += bc_kill_ri[g]; tKre += bc_kill_re[g]; tKpm += bc_kill_pm[g];
            tKcc += bc_kill_cc[g]; tKcv += bc_kill_cv[g];
            tSnapCCt += bc_snap_cc_tried[g]; tSnapCCs += bc_snap_cc_snapped[g];
            tSnapCVt += bc_snap_cv_tried[g]; tSnapCVs += bc_snap_cv_snapped[g];
        }
        fprintf(stderr,
            "[BC_COUNTS] TOTAL alive=%zu killed=%zu  "
            "RI=%zu RE=%zu PM=%zu CC=%zu CV=%zu  "
            "kill(RI=%zu,RE=%zu,PM=%zu,CC=%zu,CV=%zu)  "
            "snap(CC=%zu/%zu,CV=%zu/%zu)\n",
            total_alive, total_killed,
            total_per_src[1], total_per_src[2], total_per_src[3], total_per_src[4], total_per_src[5],
            tKri, tKre, tKpm, tKcc, tKcv,
            tSnapCCs, tSnapCCt, tSnapCVs, tSnapCVt);
    }
    for (size_t g = 0; g < n_planes; g++) {
        for (uint32_t i = 0; i < anchors_n[g]; i++) {
            anchor_t* a = &anchors[g][i];
            if (a->killed) continue;
            register_anchor_in_pool(&pool, a, a->plane_id, planes[g].plane_type, ridge_lines, n_ridge_lines, params->pool_merge_dist);
        }
    }

    // ── Coplanar import ──
    for (size_t g = 0; g < n_planes; g++) {
        const v7_bc_input_plane_t* P = &planes[g];
        if (P->rejected) continue;
        int gi = find_grid_idx_by_pid(grids, n_grids, P->id);
        if (gi < 0) continue;
        const v7_bc_input_plane_grid_t* PG = &grids[gi];
        if (!PG->has_bal || !PG->has_group) continue;
        // Build a set of pool ids already touching this plane.
        // Linear scan over pool — fine for small N.
        for (uint32_t pi = 0; pi < pool.n_entries; pi++) {
            pentry_t* pe = pool.entries[pi];
            if (!pe || pe->consumed) continue;
            // Does pe already include plane g?
            bool already = false;
            for (uint32_t k = 0; k < pe->n_plane_ids; k++) {
                if (pe->plane_ids[k] == P->id) { already = true; break; }
            }
            if (already) continue;
            // Decouple: never import a plane of an incompatible type into an existing
            // (single-type) corner — keeps roof/wall/ground corners separate.
            if (!rg_plane_types_mergeable((rg_plane_type_t)P->plane_type, (rg_plane_type_t)pe->plane_type)) continue;
            // Distance to plane.
            double sd = P->normal[0]*pe->x + P->normal[1]*pe->y + P->normal[2]*pe->z + P->d;
            if (fabs(sd) > params->coplanar_dist_threshold) continue;
            double sx = pe->x - sd * P->normal[0];
            double sy = pe->y - sd * P->normal[1];
            double sz = pe->z - sd * P->normal[2];
            float u = (float)(sx*P->basis_e1[0] + sy*P->basis_e1[1] + sz*P->basis_e1[2]);
            float v = (float)(sx*P->basis_e2[0] + sy*P->basis_e2[1] + sz*P->basis_e2[2]);
            const v7_bc_input_grid_field_t* gf = &PG->group_field;
            int col = (int)floorf((u - gf->g_min_u) / gf->cell_size);
            int row = (int)floorf((v - gf->g_min_v) / gf->cell_size);
            if (col < 0 || (uint32_t)col >= gf->n_cols || row < 0 || (uint32_t)row >= gf->n_rows) continue;
            float bal = PG->bal_field[(size_t)row * gf->n_cols + col];
            if (isnan(bal) || bal < params->coplanar_balance_min) continue;
            // Push a coplanar anchor into plane g and register it.
            anchor_t a = {0};
            a.x = sx; a.y = sy; a.z = sz;
            a.source = SRC_COPLANAR;
            a.plane_id = P->id;
            a.has_pool_id = true; a.pool_id = pe->id;
            // Take first dirsByPlane entry if any.
            if (pe->n_dirs > 0) {
                a.has_dirs = true;
                memcpy(a.dir_a, pe->dirs[0].dir_a, 12);
                memcpy(a.dir_b, pe->dirs[0].dir_b, 12);
            }
            // Inherit CoM-bisector from the pool entry's first plane too.
            if (pe->n_com_bis > 0) {
                a.has_com_bisector = true;
                memcpy(a.com_bisector, pe->com_bis[0].com_bisector, 12);
            }
            a.name = name_for(&nctrs, SRC_COPLANAR, P->id);
            // Track in per-plane list for stats and use it as new anchor.
            register_anchor_in_pool(&pool, &a, P->id, P->plane_type, ridge_lines, n_ridge_lines, params->pool_merge_dist);
            anchor_free(&a);
            out->coplanar_imported++;
        }
    }

    // ── Serialize pool into out->corners ──
    // Walk non-consumed entries in id order.
    uint32_t* alive_idx = (uint32_t*)safe_malloc_array(pool.n_entries, sizeof(uint32_t));
    if (!alive_idx && pool.n_entries > 0) goto fail;
    uint32_t n_alive = 0;
    for (uint32_t i = 0; i < pool.n_entries; i++) {
        if (pool.entries[i] && !pool.entries[i]->consumed) alive_idx[n_alive++] = i;
    }
    out->corners = (v7_bc_corner_t*)safe_calloc_array(n_alive, sizeof(v7_bc_corner_t));
    if (!out->corners && n_alive > 0) { free(alive_idx); goto fail; }
    out->n_corners = n_alive;
    // Map pool entry id → output index for plane_corner_ids.
    uint32_t* id_to_out_idx = (uint32_t*)safe_malloc_array(pool.next_id, sizeof(uint32_t));
    if (!id_to_out_idx && pool.next_id > 0) { free(alive_idx); goto fail; }
    for (uint32_t i = 0; i < pool.next_id; i++) id_to_out_idx[i] = UINT32_MAX;

    for (uint32_t i = 0; i < n_alive; i++) {
        const pentry_t* pe = pool.entries[alive_idx[i]];
        v7_bc_corner_t* C = &out->corners[i];
        C->id = (uint32_t)i;
        C->source = pe->source;
        C->point[0] = pe->x; C->point[1] = pe->y; C->point[2] = pe->z;
        C->n_merged = pe->n_merged;
        C->name = pe->name ? strdup(pe->name) : (char*)NULL;
        if (getenv("BR_DUMP_POOL")) {
            fprintf(stderr, "[pool] id=%u pos=%.3f,%.3f,%.3f n_members=%u name=%s\n",
                (uint32_t)i, pe->x, pe->y, pe->z, pe->n_members, pe->name ? pe->name : "(null)");
            for (uint32_t _m = 0; _m < pe->n_members; _m++) {
                const v7p_member_t* _mm = &pe->members[_m];
                fprintf(stderr, "[pool]   member src=%u auth=%d pos=%.3f,%.3f,%.3f has_axis=%d axis=%.4f,%.4f,%.4f name=%s\n",
                    _mm->source, (int)_mm->authority, _mm->x, _mm->y, _mm->z,
                    _mm->has_axis, _mm->axis[0], _mm->axis[1], _mm->axis[2], _mm->name ? _mm->name : "(null)");
            }
        }
        if (pe->n_plane_ids > 0) {
            C->plane_ids = (uint32_t*)safe_malloc_array(pe->n_plane_ids, sizeof(uint32_t));
            if (!C->plane_ids) { free(alive_idx); free(id_to_out_idx); goto fail; }
            memcpy(C->plane_ids, pe->plane_ids, pe->n_plane_ids * sizeof(uint32_t));
            C->source_per_plane = (uint8_t*)safe_malloc_array(pe->n_plane_ids, sizeof(uint8_t));
            if (!C->source_per_plane) { free(alive_idx); free(id_to_out_idx); goto fail; }
            memcpy(C->source_per_plane, pe->source_per_plane, pe->n_plane_ids * sizeof(uint8_t));
            C->n_plane_ids = pe->n_plane_ids;
        }
        if (pe->n_ridge_ids > 0) {
            C->ridge_ids = (uint32_t*)safe_malloc_array(pe->n_ridge_ids, sizeof(uint32_t));
            if (!C->ridge_ids) { free(alive_idx); free(id_to_out_idx); goto fail; }
            memcpy(C->ridge_ids, pe->ridge_ids, pe->n_ridge_ids * sizeof(uint32_t));
            C->n_ridge_ids = pe->n_ridge_ids;
        }
        if (pe->n_dirs > 0) {
            C->dirs = (v7_bc_corner_dir_t*)safe_malloc_array(pe->n_dirs, sizeof(v7_bc_corner_dir_t));
            if (!C->dirs) { free(alive_idx); free(id_to_out_idx); goto fail; }
            for (uint32_t k = 0; k < pe->n_dirs; k++) {
                C->dirs[k].plane_id = pe->dirs[k].plane_id;
                memcpy(C->dirs[k].dir_a, pe->dirs[k].dir_a, 12);
                memcpy(C->dirs[k].dir_b, pe->dirs[k].dir_b, 12);
            }
            C->n_dirs = pe->n_dirs;
        }
        if (pe->n_com_bis > 0) {
            C->com_bisectors = (v7_bc_corner_com_bisector_t*)safe_malloc_array(pe->n_com_bis, sizeof(v7_bc_corner_com_bisector_t));
            if (!C->com_bisectors) { free(alive_idx); free(id_to_out_idx); goto fail; }
            for (uint32_t k = 0; k < pe->n_com_bis; k++) {
                C->com_bisectors[k].plane_id = pe->com_bis[k].plane_id;
                memcpy(C->com_bisectors[k].com_bisector, pe->com_bis[k].com_bisector, 12);
            }
            C->n_com_bisectors = pe->n_com_bis;
        }
        if (pe->n_members > 0) {
            C->members = (v7_bc_member_t*)safe_calloc_array(pe->n_members, sizeof(v7_bc_member_t));
            if (!C->members) { free(alive_idx); free(id_to_out_idx); goto fail; }
            for (uint32_t k = 0; k < pe->n_members; k++) {
                C->members[k].point[0] = pe->members[k].x;
                C->members[k].point[1] = pe->members[k].y;
                C->members[k].point[2] = pe->members[k].z;
                C->members[k].source = pe->members[k].source;
                C->members[k].authority = (v7_bc_authority_t)pe->members[k].authority;
                C->members[k].has_axis = pe->members[k].has_axis;
                if (pe->members[k].has_axis) memcpy(C->members[k].axis, pe->members[k].axis, 12);
                C->members[k].has_ridge_id = pe->members[k].has_ridge_id;
                C->members[k].ridge_id = pe->members[k].ridge_id;
                C->members[k].plane_id = pe->members[k].has_plane_id
                                             ? pe->members[k].plane_id
                                             : 0xFFFFFFFFu;
            }
            C->n_members = pe->n_members;
        }
        id_to_out_idx[pe->id] = i;
    }
    free(alive_idx);

    // plane_corner_ids: for each upstream plane (by index 0..n_planes-1),
    // list of output corner indices.
    out->n_planes = n_planes;
    out->plane_corner_ids = (uint32_t**)safe_calloc_array(n_planes, sizeof(uint32_t*));
    out->plane_corner_ids_count = (uint32_t*)safe_calloc_array(n_planes, sizeof(uint32_t));
    if ((!out->plane_corner_ids || !out->plane_corner_ids_count) && n_planes > 0) {
        free(id_to_out_idx); goto fail;
    }
    for (size_t g = 0; g < n_planes; g++) {
        uint32_t pid = planes[g].id;
        uint32_t cnt = 0;
        // First pass: count.
        for (size_t ci = 0; ci < out->n_corners; ci++) {
            for (uint32_t k = 0; k < out->corners[ci].n_plane_ids; k++) {
                if (out->corners[ci].plane_ids[k] == pid) { cnt++; break; }
            }
        }
        if (cnt > 0) {
            out->plane_corner_ids[g] = (uint32_t*)safe_malloc_array(cnt, sizeof(uint32_t));
            if (!out->plane_corner_ids[g]) { free(id_to_out_idx); goto fail; }
            uint32_t j = 0;
            for (size_t ci = 0; ci < out->n_corners; ci++) {
                for (uint32_t k = 0; k < out->corners[ci].n_plane_ids; k++) {
                    if (out->corners[ci].plane_ids[k] == pid) { out->plane_corner_ids[g][j++] = (uint32_t)ci; break; }
                }
            }
            out->plane_corner_ids_count[g] = cnt;
        }
    }
    free(id_to_out_idx);

    // ── Final invariant sweep (dev builds) ────────────────────────────
    // Confirm every emitted corner's cached `source` equals the highest-
    // priority entry in source_per_plane[]. Catches any code path that
    // forgets to update one of the two in lockstep.
#ifndef NDEBUG
    for (size_t i = 0; i < out->n_corners; i++) {
        const v7_bc_corner_t* C = &out->corners[i];
        if (C->n_plane_ids == 0) continue;
        uint8_t mx = C->source_per_plane[0];
        for (uint32_t k = 1; k < C->n_plane_ids; k++) {
            if (v7p_src_priority(C->source_per_plane[k]) > v7p_src_priority(mx)) {
                mx = C->source_per_plane[k];
            }
        }
        assert(v7p_src_priority(C->source) == v7p_src_priority(mx));
    }
#endif

    // ── Cleanup ──
    for (size_t g = 0; g < n_planes; g++) {
        for (uint32_t i = 0; i < anchors_n[g]; i++) anchor_free(&anchors[g][i]);
        free(anchors[g]);
    }
    free(anchors); free(anchors_n); free(anchors_cap);
    free(bc_snap_cc_tried); free(bc_snap_cc_snapped);
    free(bc_snap_cv_tried); free(bc_snap_cv_snapped);
    free(bc_kill_ri); free(bc_kill_re); free(bc_kill_pm); free(bc_kill_cc); free(bc_kill_cv);
    for (size_t i = 0; i < n_planes; i++) { free(group_idx[i]); free(prs[i]); }
    free(group_idx); free(group_n); free(prs); free(prs_n);
    nctrs_free(&nctrs);
    pool_free(&pool);
    return 0;

fail:
    if (anchors) {
        for (size_t g = 0; g < n_planes; g++) {
            for (uint32_t i = 0; i < anchors_n[g]; i++) anchor_free(&anchors[g][i]);
            free(anchors[g]);
        }
        free(anchors);
    }
    free(anchors_n); free(anchors_cap);
    free(bc_snap_cc_tried); free(bc_snap_cc_snapped);
    free(bc_snap_cv_tried); free(bc_snap_cv_snapped);
    free(bc_kill_ri); free(bc_kill_re); free(bc_kill_pm); free(bc_kill_cc); free(bc_kill_cv);
    for (size_t i = 0; i < n_planes; i++) { free(group_idx[i]); free(prs[i]); }
    free(group_idx); free(group_n); free(prs); free(prs_n);
    nctrs_free(&nctrs);
    pool_free(&pool);
    v7_bc_outputs_free(out);
    return -1;
}
