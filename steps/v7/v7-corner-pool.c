// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-corner-pool — implementation of the shared corner-pool primitives.
// All bodies were moved verbatim out of v7-boundary-corners-impl.c and
// renamed with the v7p_ prefix; corners-impl now calls these and the
// step-specific anchor/ridge/name-counter code stays in that file.

#define _POSIX_C_SOURCE 200809L

#include "v7-corner-pool.h"
#include "v7-roof-graph-format.h"   // rg_plane_type_t + rg_plane_types_mergeable (type gate)

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// ── Source priority ────────────────────────────────────────────────

int v7p_src_priority(uint8_t s) {
    switch (s) {
        case V7P_SRC_RIDGE_INTERSECTION: return 4;
        case V7P_SRC_RIDGE_END:          return 3;
        case V7P_SRC_POLYGON_MERGE:      return 2;
        case V7P_SRC_CONCAVE:
        case V7P_SRC_CONVEX:             return 1;
        case V7P_SRC_COPLANAR:           return 0;
        default:                         return 0;
    }
}

// ── Lifecycle ──────────────────────────────────────────────────────

void v7p_entry_free(v7p_entry_t* p) {
    if (!p) return;
    free(p->plane_ids);
    free(p->source_per_plane);
    free(p->ridge_ids);
    free(p->dirs);
    free(p->com_bis);
    if (p->members) {
        for (uint32_t i = 0; i < p->n_members; i++) free(p->members[i].name);
        free(p->members);
    }
    free(p->name);
    memset(p, 0, sizeof(*p));
}

void v7p_pool_free(v7p_pool_t* p) {
    for (uint32_t i = 0; i < p->n_entries; i++) {
        if (p->entries[i]) { v7p_entry_free(p->entries[i]); free(p->entries[i]); }
    }
    free(p->entries);
    free(p->redirects);
    memset(p, 0, sizeof(*p));
}

// ── Helpers ────────────────────────────────────────────────────────

char* v7p_extend_name(const char* existing, const char* incoming) {
    if (!incoming) return existing ? strdup(existing) : (char*)NULL;
    if (!existing) return strdup(incoming);
    // Tokenize existing by '+', keep unique tokens, then add tokens from
    // incoming that aren't already present.
    size_t cap = strlen(existing) + 1 + strlen(incoming) + 2;
    char* buf = (char*)malloc(cap);
    if (!buf) return NULL;
    strcpy(buf, existing);
    size_t buf_len = strlen(buf);
    const char* p = incoming;
    while (*p) {
        const char* q = strchr(p, '+');
        size_t tok_len = q ? (size_t)(q - p) : strlen(p);
        if (tok_len > 0) {
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

int v7p_push_uint_unique(uint32_t** arr, uint32_t* n, uint32_t* cap, uint32_t v) {
    for (uint32_t i = 0; i < *n; i++) if ((*arr)[i] == v) return 0;
    if (*n == *cap) {
        uint32_t nc = *cap ? *cap * 2 : 4;
        uint32_t* p = (uint32_t*)realloc(*arr, nc * sizeof(uint32_t));
        if (!p) return -1;
        *arr = p; *cap = nc;
    }
    (*arr)[(*n)++] = v;
    return 0;
}

int v7p_entry_register_plane(v7p_entry_t* e, uint32_t plane_id, uint8_t src) {
    // Same-plane collision → keep higher-priority source.
    for (uint32_t i = 0; i < e->n_plane_ids; i++) {
        if (e->plane_ids[i] == plane_id) {
            if (v7p_src_priority(src) > v7p_src_priority(e->source_per_plane[i])) {
                e->source_per_plane[i] = src;
            }
            // Maintain cached global.
            if (v7p_src_priority(e->source_per_plane[i]) > v7p_src_priority(e->source)) {
                e->source = e->source_per_plane[i];
            }
            v7p_entry_validate(e);
            return 0;
        }
    }
    // Fresh plane: parallel-append to both arrays. Share plane_ids_cap.
    if (e->n_plane_ids == e->plane_ids_cap) {
        uint32_t nc = e->plane_ids_cap ? e->plane_ids_cap * 2 : 4;
        uint32_t* p = (uint32_t*)realloc(e->plane_ids, nc * sizeof(uint32_t));
        if (!p) return -1;
        e->plane_ids = p;
        uint8_t* s = (uint8_t*)realloc(e->source_per_plane, nc * sizeof(uint8_t));
        if (!s) return -1;
        e->source_per_plane = s;
        e->plane_ids_cap = nc;
    }
    e->plane_ids[e->n_plane_ids]        = plane_id;
    e->source_per_plane[e->n_plane_ids] = src;
    e->n_plane_ids++;
    if (v7p_src_priority(src) > v7p_src_priority(e->source)) e->source = src;
    v7p_entry_validate(e);
    return 0;
}

void v7p_entry_validate(const v7p_entry_t* e) {
#ifndef NDEBUG
    if (!e || e->n_plane_ids == 0) return;
    uint8_t mx = e->source_per_plane[0];
    for (uint32_t k = 1; k < e->n_plane_ids; k++) {
        if (v7p_src_priority(e->source_per_plane[k]) > v7p_src_priority(mx)) {
            mx = e->source_per_plane[k];
        }
    }
    // Global source must be at the highest per-plane priority. Equal-priority
    // ties are accepted by comparing priorities (concave/convex share priority 1).
    assert(v7p_src_priority(e->source) == v7p_src_priority(mx));
#else
    (void)e;
#endif
}

void v7p_avg_members(const v7p_member_t* arr, uint32_t n, double out[3]) {
    if (n == 0) { out[0] = out[1] = out[2] = 0; return; }
    double cx = 0, cy = 0, cz = 0;
    for (uint32_t i = 0; i < n; i++) { cx += arr[i].x; cy += arr[i].y; cz += arr[i].z; }
    out[0] = cx / n; out[1] = cy / n; out[2] = cz / n;
}

int v7p_solve_sym(double M[3][3], const double r[3], int k, double lambda[3]) {
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

void v7p_solve_member_position(const v7p_member_t* members, int n, double out[3]) {
    if (n <= 0) { out[0] = out[1] = out[2] = 0; return; }
    uint32_t nm = (uint32_t)n;

    v7p_member_t* full  = (v7p_member_t*)malloc(nm * sizeof(v7p_member_t));
    v7p_member_t* axial = (v7p_member_t*)malloc(nm * sizeof(v7p_member_t));
    v7p_member_t* none  = (v7p_member_t*)malloc(nm * sizeof(v7p_member_t));
    uint32_t nf = 0, na = 0, nn = 0;
    if (!full || !axial || !none) { free(full); free(axial); free(none); return; }
    for (uint32_t i = 0; i < nm; i++) {
        const v7p_member_t* m = &members[i];
        if (m->authority == V7P_AUTHORITY_FULL) full[nf++] = *m;
        else if (m->authority == V7P_AUTHORITY_AXIAL && m->has_axis) axial[na++] = *m;
        else none[nn++] = *m;
    }
    if (nf > 0) {
        double c[3]; v7p_avg_members(full, nf, c);
        out[0] = c[0]; out[1] = c[1]; out[2] = c[2];
        free(full); free(axial); free(none); return;
    }
    if (na == 0) {
        double c[3]; v7p_avg_members(members, nm, c);
        out[0] = c[0]; out[1] = c[1]; out[2] = c[2];
        free(full); free(axial); free(none); return;
    }
    double target[3];
    if (nn > 0) v7p_avg_members(none, nn, target);
    else        v7p_avg_members(axial, na, target);

    int k = (na < 3) ? (int)na : 3;
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
    double M[3][3] = {{0}};
    for (int i = 0; i < k; i++) {
        for (int j = 0; j < k; j++) {
            M[i][j] = A[i][0]*A[j][0] + A[i][1]*A[j][1] + A[i][2]*A[j][2];
        }
    }
    double lambda[3];
    if (v7p_solve_sym(M, r, k, lambda) != 0) {
        out[0] = target[0]; out[1] = target[1]; out[2] = target[2];
        free(full); free(axial); free(none); return;
    }
    double px = target[0], py = target[1], pz = target[2];
    for (int i = 0; i < k; i++) {
        px += A[i][0] * lambda[i];
        py += A[i][1] * lambda[i];
        pz += A[i][2] * lambda[i];
    }
    out[0] = px; out[1] = py; out[2] = pz;
    free(full); free(axial); free(none);
}

void v7p_recompute_entry(v7p_entry_t* e) {
    if (!e) return;
    // Derive cached global `e->source` from the canonical per-plane store
    // (`source_per_plane[]`), NOT from `members[i].source`. The per-member
    // source is the source-of-the-anchor-that-contributed-this-position
    // (position provenance), which can lag behind per-plane labels updated
    // by register_plane after super-merge. The invariant the validator
    // checks is `pri(e->source) == pri(max(source_per_plane[]))` — that's
    // exactly the derivation here.
    //
    // History: pre-#254-263 unification, `source_per_plane` didn't exist
    // and `members[i].source` WAS the only authoritative store. This
    // function was never updated; it was the root cause of the BM
    // full-dahouet `v7p_entry_validate` assertion.
    if (e->n_plane_ids > 0) {
        uint8_t best_src = e->source_per_plane[0];
        for (uint32_t k = 1; k < e->n_plane_ids; k++) {
            if (v7p_src_priority(e->source_per_plane[k]) > v7p_src_priority(best_src)) {
                best_src = e->source_per_plane[k];
            }
        }
        e->source = best_src;
    }
    e->n_merged = (uint16_t)e->n_members;
    if (e->n_members == 0) return;

    // Position core: authority-ranked (FULL > AXIAL > NONE) constrained
    // least-squares — extracted to a shared pure function so the regularize
    // loop's MERGE_CORNER can reuse the same constrained-meet math (see
    // 2026-06-14 in-loop-make-shared §4.5(1)). The `source` derivation above
    // stays in this entry-level wrapper, out of the loop's path (that
    // derivation was the BM assertion bug).
    // Seed pos from the current entry position so the OOM path inside
    // v7p_solve_member_position (which leaves out[] untouched) preserves the
    // pre-existing e->x/y/z, byte-identical to the old inline early-return.
    double pos[3] = { e->x, e->y, e->z };
    v7p_solve_member_position(e->members, (int)e->n_members, pos);
    e->x = pos[0]; e->y = pos[1]; e->z = pos[2];
}

// ── Mutators ───────────────────────────────────────────────────────

int v7p_entry_push_member(v7p_entry_t* e, const v7p_member_t* src) {
    if (e->n_members == e->members_cap) {
        uint32_t nc = e->members_cap ? e->members_cap * 2 : 4;
        v7p_member_t* arr = (v7p_member_t*)realloc(e->members, nc * sizeof(v7p_member_t));
        if (!arr) return -1;
        e->members = arr; e->members_cap = nc;
    }
    e->members[e->n_members] = *src;
    e->members[e->n_members].name = src->name ? strdup(src->name) : (char*)NULL;
    e->n_members++;
    return 0;
}

int v7p_entry_add_dirs(v7p_entry_t* e, uint32_t plane_id,
                        const float dir_a[3], const float dir_b[3],
                        v7p_dirs_mode_t mode) {
    for (uint32_t i = 0; i < e->n_dirs; i++) {
        if (e->dirs[i].plane_id == plane_id) {
            if (mode == V7P_DIRS_REPLACE) {
                memcpy(e->dirs[i].dir_a, dir_a, 12);
                memcpy(e->dirs[i].dir_b, dir_b, 12);
            }
            return 0;
        }
    }
    if (e->n_dirs == e->dirs_cap) {
        uint32_t nc = e->dirs_cap ? e->dirs_cap * 2 : 4;
        v7p_dirs_entry_t* arr = (v7p_dirs_entry_t*)realloc(e->dirs, nc * sizeof(*arr));
        if (!arr) return -1;
        e->dirs = arr; e->dirs_cap = nc;
    }
    e->dirs[e->n_dirs].plane_id = plane_id;
    memcpy(e->dirs[e->n_dirs].dir_a, dir_a, 12);
    memcpy(e->dirs[e->n_dirs].dir_b, dir_b, 12);
    e->n_dirs++;
    return 0;
}

int v7p_entry_add_com_bis(v7p_entry_t* e, uint32_t plane_id,
                           const float com_bisector[3],
                           v7p_dirs_mode_t mode) {
    for (uint32_t i = 0; i < e->n_com_bis; i++) {
        if (e->com_bis[i].plane_id == plane_id) {
            if (mode == V7P_DIRS_REPLACE) memcpy(e->com_bis[i].com_bisector, com_bisector, 12);
            return 0;
        }
    }
    if (e->n_com_bis == e->com_bis_cap) {
        uint32_t nc = e->com_bis_cap ? e->com_bis_cap * 2 : 4;
        v7p_com_bis_entry_t* arr = (v7p_com_bis_entry_t*)realloc(e->com_bis, nc * sizeof(*arr));
        if (!arr) return -1;
        e->com_bis = arr; e->com_bis_cap = nc;
    }
    e->com_bis[e->n_com_bis].plane_id = plane_id;
    memcpy(e->com_bis[e->n_com_bis].com_bisector, com_bisector, 12);
    e->n_com_bis++;
    return 0;
}

// ── Pool registry ──────────────────────────────────────────────────

int v7p_pool_ensure_redirects(v7p_pool_t* p, uint32_t need) {
    if (need <= p->redirects_cap) return 0;
    uint32_t nc = p->redirects_cap ? p->redirects_cap * 2 : 32;
    while (nc < need) nc *= 2;
    uint32_t* arr = (uint32_t*)realloc(p->redirects, nc * sizeof(uint32_t));
    if (!arr) return -1;
    for (uint32_t i = p->redirects_cap; i < nc; i++) arr[i] = UINT32_MAX;
    p->redirects = arr;
    p->redirects_cap = nc;
    return 0;
}

void v7p_pool_set_redirect(v7p_pool_t* p, uint32_t from_id, uint32_t to_id) {
    if (v7p_pool_ensure_redirects(p, from_id + 1) != 0) return;
    p->redirects[from_id] = to_id;
}

uint32_t v7p_pool_resolve(v7p_pool_t* p, uint32_t id) {
    while (id < p->redirects_cap && p->redirects[id] != UINT32_MAX) {
        id = p->redirects[id];
    }
    return id;
}

v7p_entry_t* v7p_pool_find_by_id(v7p_pool_t* p, uint32_t id) {
    uint32_t r = v7p_pool_resolve(p, id);
    for (uint32_t i = 0; i < p->n_entries; i++) {
        if (p->entries[i] && p->entries[i]->id == r) return p->entries[i];
    }
    return NULL;
}

int v7p_pool_push_entry(v7p_pool_t* p, v7p_entry_t* e) {
    if (p->n_entries == p->cap) {
        uint32_t nc = p->cap ? p->cap * 2 : 16;
        v7p_entry_t** arr = (v7p_entry_t**)realloc(p->entries, nc * sizeof(v7p_entry_t*));
        if (!arr) return -1;
        p->entries = arr; p->cap = nc;
    }
    p->entries[p->n_entries++] = e;
    return 0;
}

// ── Upsert with super-merge ────────────────────────────────────────

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
                         uint8_t plane_type,
                         double merge_dist) {
    double merge_sq = merge_dist * merge_dist;
    uint32_t* match_idx = NULL;
    uint32_t  n_match = 0, cap_match = 0;
    for (uint32_t i = 0; i < pool->n_entries; i++) {
        v7p_entry_t* e = pool->entries[i];
        if (!e || e->consumed) continue;
        // Type gate: never fuse into an entry of an incompatible plane type —
        // keeps roof/wall/ground corners decoupled (unclassified≈roof). Skips the
        // check when this member has no plane (RI seeds) or the entry is untyped.
        if (has_plane_id &&
            !rg_plane_types_mergeable((rg_plane_type_t)plane_type, (rg_plane_type_t)e->plane_type)) {
            continue;
        }
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
                uint32_t* p = (uint32_t*)realloc(match_idx, nc * sizeof(uint32_t));
                if (!p) { free(match_idx); return NULL; }
                match_idx = p; cap_match = nc;
            }
            match_idx[n_match++] = i;
        }
    }

    v7p_member_t member = {0};
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
    member.name = name ? (char*)name : NULL;  // by-pointer only; deep copy on push

    if (n_match == 0) {
        v7p_entry_t* e = (v7p_entry_t*)calloc(1, sizeof(*e));
        if (!e) { free(match_idx); return NULL; }
        e->id = pool->next_id++;
        e->x = mx; e->y = my; e->z = mz;
        e->source = source;
        e->plane_type = plane_type;   // this corner's (single) type, for the merge gate
        e->name = name ? strdup(name) : (char*)NULL;
        if (has_plane_id) {
            if (v7p_entry_register_plane(e, plane_id, source) != 0) {
                v7p_entry_free(e); free(e); free(match_idx); return NULL;
            }
        }
        if (has_ridge_id) {
            if (v7p_push_uint_unique(&e->ridge_ids, &e->n_ridge_ids, &e->ridge_ids_cap, ridge_id) != 0) {
                v7p_entry_free(e); free(e); free(match_idx); return NULL;
            }
        }
        if (has_plane_id && has_dirs) {
            if (v7p_entry_add_dirs(e, plane_id, dir_a, dir_b, V7P_DIRS_REPLACE) != 0) {
                v7p_entry_free(e); free(e); free(match_idx); return NULL;
            }
        }
        if (has_plane_id && has_com_bisector) {
            if (v7p_entry_add_com_bis(e, plane_id, com_bisector, V7P_DIRS_REPLACE) != 0) {
                v7p_entry_free(e); free(e); free(match_idx); return NULL;
            }
        }
        if (v7p_entry_push_member(e, &member) != 0) {
            v7p_entry_free(e); free(e); free(match_idx); return NULL;
        }
        v7p_recompute_entry(e);
        if (v7p_pool_push_entry(pool, e) != 0) {
            v7p_entry_free(e); free(e); free(match_idx); return NULL;
        }
        free(match_idx);
        return e;
    }

    // Super-merge: fold matches[1..] into matches[0].
    v7p_entry_t* primary = pool->entries[match_idx[0]];
    for (uint32_t i = 1; i < n_match; i++) {
        v7p_entry_t* other = pool->entries[match_idx[i]];
        if (!other || other->consumed) continue;
        for (uint32_t k = 0; k < other->n_members; k++) {
            if (v7p_entry_push_member(primary, &other->members[k]) != 0) { free(match_idx); return NULL; }
        }
        for (uint32_t k = 0; k < other->n_plane_ids; k++) {
            // Carry the other entry's per-plane source over — same-plane
            // collisions get resolved by priority inside register_plane.
            v7p_entry_register_plane(primary, other->plane_ids[k], other->source_per_plane[k]);
        }
        for (uint32_t k = 0; k < other->n_ridge_ids; k++) {
            v7p_push_uint_unique(&primary->ridge_ids, &primary->n_ridge_ids, &primary->ridge_ids_cap, other->ridge_ids[k]);
        }
        for (uint32_t k = 0; k < other->n_dirs; k++) {
            v7p_entry_add_dirs(primary, other->dirs[k].plane_id, other->dirs[k].dir_a, other->dirs[k].dir_b, V7P_DIRS_IF_ABSENT);
        }
        for (uint32_t k = 0; k < other->n_com_bis; k++) {
            v7p_entry_add_com_bis(primary, other->com_bis[k].plane_id, other->com_bis[k].com_bisector, V7P_DIRS_IF_ABSENT);
        }
        char* nm = v7p_extend_name(primary->name, other->name);
        free(primary->name); primary->name = nm;
        v7p_pool_set_redirect(pool, other->id, primary->id);
        other->consumed = true;
        v7p_entry_free(other);
        free(other);
        pool->entries[match_idx[i]] = NULL;
    }
    if (v7p_entry_push_member(primary, &member) != 0) { free(match_idx); return NULL; }
    if (has_plane_id) v7p_entry_register_plane(primary, plane_id, source);
    if (has_plane_id && has_dirs) v7p_entry_add_dirs(primary, plane_id, dir_a, dir_b, V7P_DIRS_REPLACE);
    if (has_plane_id && has_com_bisector) v7p_entry_add_com_bis(primary, plane_id, com_bisector, V7P_DIRS_REPLACE);
    if (has_ridge_id) v7p_push_uint_unique(&primary->ridge_ids, &primary->n_ridge_ids, &primary->ridge_ids_cap, ridge_id);
    char* nm = v7p_extend_name(primary->name, name);
    free(primary->name); primary->name = nm;
    v7p_recompute_entry(primary);
    free(match_idx);
    (void)merge_sq;  // silence unused warning if no entries matched
    return primary;
}
