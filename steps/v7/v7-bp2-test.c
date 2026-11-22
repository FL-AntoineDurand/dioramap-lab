// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-bp2-test — fixture-driven tests for v7-boundary-polygons-2.
//
// Usage: v7-bp2-test <fixtures-dir>
//
// Loads every *.json in <fixtures-dir>/ and <fixtures-dir>/unit/, runs
// the dispatched assertion (driven by fixture "category"), and reports
// pass/fail with a final summary. Exit 0 on all-pass, 1 on any failure.
//
// Fixture schema: see v3/docs/plan/BOUNDARY-POLYGONS-2-TEST-SCOPE.md.

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <jansson.h>

#include "v7-boundary-polygons-2-impl.h"

// ── Test result tracking ────────────────────────────────────────────

typedef struct test_ctx {
    int n_pass;
    int n_fail;
    int n_skip;
    const char* fixture_id;
    bool fixture_failed;
} test_ctx_t;

static void fail_fixture(test_ctx_t* tc, const char* fmt, ...) {
    if (!tc->fixture_failed) { tc->fixture_failed = true; tc->n_fail++; }
    fprintf(stderr, "  FAIL  [%s]: ", tc->fixture_id);
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

#define EXPECT(tc, cond, fmt, ...) do { \
    if (!(cond)) { fail_fixture((tc), fmt, ##__VA_ARGS__); } \
} while (0)

// ── JSON helpers ────────────────────────────────────────────────────

static double j_num_or(json_t* o, const char* key, double dflt) {
    json_t* v = json_object_get(o, key);
    if (!v || !json_is_number(v)) return dflt;
    return json_number_value(v);
}
static double j_real_at(json_t* arr, size_t i) {
    json_t* v = json_array_get(arr, i);
    if (!v) return 0;
    if (json_is_integer(v)) return (double)json_integer_value(v);
    return json_real_value(v);
}
static int j_int_or(json_t* o, const char* key, int dflt) {
    json_t* v = json_object_get(o, key);
    if (!v || !json_is_integer(v)) return dflt;
    return (int)json_integer_value(v);
}
static const char* j_str_or(json_t* o, const char* key, const char* dflt) {
    json_t* v = json_object_get(o, key);
    if (!v || !json_is_string(v)) return dflt;
    return json_string_value(v);
}
static bool j_bool_or(json_t* o, const char* key, bool dflt) {
    json_t* v = json_object_get(o, key);
    if (!v) return dflt;
    return json_is_true(v);
}
static void j_vec3(json_t* arr, double out[3]) {
    out[0] = j_real_at(arr, 0);
    out[1] = j_real_at(arr, 1);
    out[2] = j_real_at(arr, 2);
}
static uint8_t src_enum_from_str(const char* s) {
    if (!s) return 0;
    if (strcmp(s, "ridge-intersection") == 0) return 1;
    if (strcmp(s, "ridge-end") == 0)          return 2;
    if (strcmp(s, "polygon-merge") == 0)      return 3;
    if (strcmp(s, "concave") == 0)            return 4;
    if (strcmp(s, "convex") == 0)             return 5;
    if (strcmp(s, "coplanar") == 0)           return 8;
    return 0;
}

// ── Cycle equality (up to rotation + reflection) ─────────────────────

static bool cycle_equal(const uint32_t* a, int na, const uint32_t* b, int nb) {
    if (na != nb) return false;
    if (na == 0) return true;
    for (int start = 0; start < na; start++) {
        bool ok = true;
        for (int i = 0; i < na; i++) {
            if (a[i] != b[(start + i) % na]) { ok = false; break; }
        }
        if (ok) return true;
        ok = true;
        for (int i = 0; i < na; i++) {
            if (a[i] != b[(start - i + na) % na]) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

// ── Synthetic point cloud generator (Q4) ────────────────────────────
//
// `synth_cloud` — given a polygon (corners list in (u,v) space) and a
// density, produces group_pts uniformly inside the polygon and nearby_pts
// just outside each edge (a thin band).
//
// The generator is invoked by fixtures via:
//   "group_pts":  { "generator": "synth_cloud_inside",  "params": {...} }
//   "nearby_pts": { "generator": "synth_cloud_outside", "params": {...} }

typedef struct gen_pts {
    double* pts;     // owned: [n][3]
    size_t  n;
} gen_pts_t;

static void gen_pts_free(gen_pts_t* g) { free(g->pts); g->pts = NULL; g->n = 0; }

// Generate group points: regular grid inside polygon bounding box,
// keep those inside the polygon. Stamp z=0 (caller adjusts via plane basis).
static gen_pts_t gen_inside(const double* polyU, const double* polyV, int n,
                              double density_per_m2) {
    gen_pts_t g = {0};
    if (n < 3) return g;
    double min_u = polyU[0], max_u = polyU[0];
    double min_v = polyV[0], max_v = polyV[0];
    for (int i = 1; i < n; i++) {
        if (polyU[i] < min_u) min_u = polyU[i];
        if (polyU[i] > max_u) max_u = polyU[i];
        if (polyV[i] < min_v) min_v = polyV[i];
        if (polyV[i] > max_v) max_v = polyV[i];
    }
    double step = 1.0 / sqrt(density_per_m2);
    size_t cap = 0;
    for (double v = min_v + step*0.5; v < max_v; v += step) {
        for (double u = min_u + step*0.5; u < max_u; u += step) {
            if (v7_bp2_pt_in_poly(u, v, polyU, polyV, n)) cap++;
        }
    }
    if (cap == 0) return g;
    g.pts = (double*)malloc(cap * 3 * sizeof(double));
    if (!g.pts) return (gen_pts_t){0};
    size_t k = 0;
    for (double v = min_v + step*0.5; v < max_v; v += step) {
        for (double u = min_u + step*0.5; u < max_u; u += step) {
            if (v7_bp2_pt_in_poly(u, v, polyU, polyV, n)) {
                g.pts[k*3+0] = u; g.pts[k*3+1] = v; g.pts[k*3+2] = 0;
                k++;
            }
        }
    }
    g.n = k;
    return g;
}

// Nearby points: stamped just OUTSIDE each polygon edge, perpendicular
// to the edge by `offset` metres.
static gen_pts_t gen_outside(const double* polyU, const double* polyV, int n,
                              double offset, int per_edge) {
    gen_pts_t g = {0};
    if (n < 3 || per_edge < 1) return g;
    size_t cap = (size_t)n * per_edge;
    g.pts = (double*)malloc(cap * 3 * sizeof(double));
    if (!g.pts) return (gen_pts_t){0};
    size_t k = 0;
    for (int e = 0; e < n; e++) {
        int e1 = (e + 1) % n;
        double eu = polyU[e1] - polyU[e];
        double ev = polyV[e1] - polyV[e];
        double el = sqrt(eu*eu + ev*ev);
        if (el < 1e-9) continue;
        // Outward normal — assuming polygon is CCW, outward is (ev, -eu)/|.|
        // If polygon is CW the offset will end up inside instead — not
        // catastrophic for tests but document the assumption.
        double nu =  ev / el;
        double nv = -eu / el;
        for (int s = 0; s < per_edge; s++) {
            double t = (s + 1) / (double)(per_edge + 1);
            double pu = polyU[e] + t * eu + offset * nu;
            double pv = polyV[e] + t * ev + offset * nv;
            g.pts[k*3+0] = pu; g.pts[k*3+1] = pv; g.pts[k*3+2] = 0;
            k++;
        }
    }
    g.n = k;
    return g;
}

// ── Fixture loading + dispatch ──────────────────────────────────────

typedef struct fixture {
    char id[128];
    char category[64];
    json_t* root;
    json_t* inputs;
    json_t* expected;
} fixture_t;

// Load anchors from "inputs.anchors" array. Allocates and returns the array;
// caller frees.
static v7_bp2_anchor_t* load_anchors(json_t* inputs, size_t* out_n) {
    json_t* arr = json_object_get(inputs, "anchors");
    if (!json_is_array(arr)) { *out_n = 0; return NULL; }
    size_t n = json_array_size(arr);
    v7_bp2_anchor_t* anchors = (v7_bp2_anchor_t*)calloc(n, sizeof(*anchors));
    if (!anchors) { *out_n = 0; return NULL; }
    for (size_t i = 0; i < n; i++) {
        json_t* a = json_array_get(arr, i);
        anchors[i].id = (uint32_t)j_int_or(a, "id", (int)i);
        anchors[i].point[0] = j_num_or(a, "x", 0);
        anchors[i].point[1] = j_num_or(a, "y", 0);
        anchors[i].point[2] = j_num_or(a, "z", 0);
        anchors[i].source = src_enum_from_str(j_str_or(a, "source", "concave"));
    }
    *out_n = n;
    return anchors;
}

static void load_basis(json_t* inputs, v7_bp2_basis_t* out) {
    json_t* b = json_object_get(inputs, "plane_basis");
    if (!b) {
        out->e1[0] = 1; out->e1[1] = 0; out->e1[2] = 0;
        out->e2[0] = 0; out->e2[1] = 1; out->e2[2] = 0;
        out->normal[0] = 0; out->normal[1] = 0; out->normal[2] = 1;
        return;
    }
    j_vec3(json_object_get(b, "e1"), out->e1);
    j_vec3(json_object_get(b, "e2"), out->e2);
    json_t* n = json_object_get(b, "normal");
    if (n) j_vec3(n, out->normal);
    else { out->normal[0] = out->normal[1] = out->normal[2] = 0; out->normal[2] = 1; }
}

// Load group_pts or nearby_pts — either literal array of [x,y,z] OR
// generator spec.
static gen_pts_t load_pts_or_generate(json_t* inputs, const char* key,
                                        const v7_bp2_anchor_t* anchors, size_t n_anchors,
                                        const v7_bp2_basis_t* basis,
                                        bool inside) {
    gen_pts_t g = {0};
    json_t* v = json_object_get(inputs, key);
    if (!v) return g;
    if (json_is_array(v)) {
        size_t n = json_array_size(v);
        g.pts = (double*)malloc(n * 3 * sizeof(double));
        if (!g.pts) return (gen_pts_t){0};
        for (size_t i = 0; i < n; i++) {
            json_t* p = json_array_get(v, i);
            g.pts[i*3+0] = j_real_at(p, 0);
            g.pts[i*3+1] = j_real_at(p, 1);
            g.pts[i*3+2] = j_real_at(p, 2);
        }
        g.n = n;
        return g;
    }
    if (json_is_object(v)) {
        const char* gen = j_str_or(v, "generator", "");
        json_t* params = json_object_get(v, "params");
        double density = j_num_or(params, "density", 1.0);     // per m²
        double offset  = j_num_or(params, "offset",  0.5);     // m, for outside
        int per_edge   = j_int_or(params, "per_edge", 5);
        // Build polygon (anchor cycle in input order) in UV.
        if (n_anchors < 3) return g;
        double* polyU = (double*)malloc(n_anchors * sizeof(double));
        double* polyV = (double*)malloc(n_anchors * sizeof(double));
        if (!polyU || !polyV) { free(polyU); free(polyV); return g; }
        for (size_t i = 0; i < n_anchors; i++) {
            v7_bp2_project_uv(anchors[i].point, basis, &polyU[i], &polyV[i]);
        }
        if (strcmp(gen, "synth_cloud_inside") == 0 || inside) {
            g = gen_inside(polyU, polyV, (int)n_anchors, density);
        } else if (strcmp(gen, "synth_cloud_outside") == 0 || !inside) {
            g = gen_outside(polyU, polyV, (int)n_anchors, offset, per_edge);
        }
        // Project UV back to 3D positions on the plane (z=0 + basis offset
        // is fine since wsd_multi re-projects through basis anyway).
        // Reconstruct 3D: p = u*e1 + v*e2.
        for (size_t i = 0; i < g.n; i++) {
            double uu = g.pts[i*3+0], vv = g.pts[i*3+1];
            g.pts[i*3+0] = uu * basis->e1[0] + vv * basis->e2[0];
            g.pts[i*3+1] = uu * basis->e1[1] + vv * basis->e2[1];
            g.pts[i*3+2] = uu * basis->e1[2] + vv * basis->e2[2];
        }
        free(polyU); free(polyV);
        return g;
    }
    return g;
}

// Compare actual cycle (corner_ids) against expected.
static void check_polygon_expected(test_ctx_t* tc,
                                     const v7_bp2_polygon_t* actual,
                                     json_t* expected_polygons_arr) {
    if (!json_is_array(expected_polygons_arr)) {
        fail_fixture(tc, "expected.polygons must be array"); return;
    }
    size_t n_exp = json_array_size(expected_polygons_arr);
    if (n_exp == 0) return;
    // For K=1 we only emit one polygon; expect the first expected to be outer.
    json_t* exp0 = json_array_get(expected_polygons_arr, 0);
    json_t* ids_arr = json_object_get(exp0, "corner_ids");
    if (!json_is_array(ids_arr)) { fail_fixture(tc, "expected.polygons[0].corner_ids missing"); return; }
    size_t n = json_array_size(ids_arr);
    uint32_t* expected_ids = (uint32_t*)malloc(n * sizeof(uint32_t));
    if (!expected_ids) { fail_fixture(tc, "alloc fail"); return; }
    for (size_t i = 0; i < n; i++) {
        expected_ids[i] = (uint32_t)json_integer_value(json_array_get(ids_arr, i));
    }
    if (!cycle_equal(expected_ids, (int)n, actual->corner_ids, (int)actual->n_corners)) {
        // Build readable diff strings.
        char ebuf[512] = {0}; char abuf[512] = {0};
        char* ep = ebuf; char* ap = abuf;
        size_t er = sizeof(ebuf) - 1; size_t ar = sizeof(abuf) - 1;
        for (size_t i = 0; i < n && er > 8; i++) {
            int w = snprintf(ep, er, "%u%s", expected_ids[i], i+1<n?",":"");
            ep += w; er -= w;
        }
        for (size_t i = 0; i < actual->n_corners && ar > 8; i++) {
            int w = snprintf(ap, ar, "%u%s", actual->corner_ids[i], i+1<actual->n_corners?",":"");
            ap += w; ar -= w;
        }
        fail_fixture(tc, "polygon mismatch: expected=[%s] actual=[%s]", ebuf, abuf);
    }
    free(expected_ids);
}

// Mulberry32 PRNG + Box-Muller Gaussian (in metres) for stability runs.
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
static uint32_t stab_rng_state;
static uint32_t stab_next(void) {
    uint32_t z = (stab_rng_state += 0x6D2B79F5u);
    z = (z ^ (z >> 15)) * (z | 1u);
    z ^= z + (z ^ (z >> 7)) * (z | 61u);
    return z ^ (z >> 14);
}
static double stab_unif(void) {
    return (double)stab_next() / 4294967296.0;
}
static double stab_gauss(double sigma_m) {
    double u1 = stab_unif();
    double u2 = stab_unif();
    if (u1 < 1e-12) u1 = 1e-12;
    double r = sqrt(-2.0 * log(u1));
    return sigma_m * r * cos(2.0 * M_PI * u2);
}

// Process one end-to-end fixture (categories: smoke, topology, stability).
// Load `contours` array from a fixture's inputs block. Returns NULL when
// missing (caller's `*out_n` set to 0). Schema:
//   "contours": [
//     { "is_hole": false, "polyU": [...], "polyV": [...] },
//     { "is_hole": true,  "polyU": [...], "polyV": [...] }
//   ]
// Caller frees each contour's polyU/polyV + the outer array.
static v7_bp2_contour_t* load_contours(json_t* inputs, size_t* out_n) {
    *out_n = 0;
    json_t* arr = json_object_get(inputs, "contours");
    if (!arr || !json_is_array(arr)) return NULL;
    size_t n = json_array_size(arr);
    if (n == 0) return NULL;
    v7_bp2_contour_t* contours = (v7_bp2_contour_t*)calloc(n, sizeof(*contours));
    if (!contours) return NULL;
    for (size_t i = 0; i < n; i++) {
        json_t* c = json_array_get(arr, i);
        json_t* uArr = json_object_get(c, "polyU");
        json_t* vArr = json_object_get(c, "polyV");
        if (!uArr || !vArr || !json_is_array(uArr) || !json_is_array(vArr)) continue;
        size_t np = json_array_size(uArr);
        if (np < 3 || json_array_size(vArr) != np) continue;
        double* U = (double*)malloc(np * sizeof(double));
        double* V = (double*)malloc(np * sizeof(double));
        if (!U || !V) { free(U); free(V); continue; }
        for (size_t k = 0; k < np; k++) {
            U[k] = j_real_at(uArr, (int)k);
            V[k] = j_real_at(vArr, (int)k);
        }
        contours[i].polyU = U;
        contours[i].polyV = V;
        contours[i].n_pts = (int)np;
        contours[i].is_hole = j_bool_or(c, "is_hole", false);
    }
    *out_n = n;
    return contours;
}

static void free_contours(v7_bp2_contour_t* contours, size_t n) {
    if (!contours) return;
    for (size_t i = 0; i < n; i++) {
        free((void*)contours[i].polyU);
        free((void*)contours[i].polyV);
    }
    free(contours);
}

static void run_e2e_fixture(test_ctx_t* tc, fixture_t* f) {
    json_t* inputs = f->inputs;
    json_t* expected = f->expected;

    v7_bp2_basis_t basis; load_basis(inputs, &basis);
    size_t n_anchors;
    v7_bp2_anchor_t* anchors = load_anchors(inputs, &n_anchors);
    if (!anchors) { fail_fixture(tc, "anchors load failed"); return; }

    gen_pts_t group  = load_pts_or_generate(inputs, "group_pts",  anchors, n_anchors, &basis, true);
    gen_pts_t nearby = load_pts_or_generate(inputs, "nearby_pts", anchors, n_anchors, &basis, false);

    size_t n_contours = 0;
    v7_bp2_contour_t* contours = load_contours(inputs, &n_contours);

    // Stability path — when expected.stability is present, run N times
    // with corner perturbations and track match rate against expected.polygons[0].
    json_t* stab_block = json_object_get(expected, "stability");
    if (stab_block && json_is_object(stab_block)) {
        int n_runs = j_int_or(stab_block, "runs", 100);
        double sigma_mm = j_num_or(stab_block, "noise_sigma_mm", 1.0);
        double min_rate = j_num_or(stab_block, "min_match_rate", 0.99);
        double sigma_m = sigma_mm * 0.001;
        // Reference cycle from expected.polygons[0] (mandatory for stability).
        json_t* ep = json_object_get(expected, "polygons");
        if (!ep || !json_is_array(ep) || json_array_size(ep) == 0) {
            fail_fixture(tc, "stability fixture must provide expected.polygons[0]");
            gen_pts_free(&group); gen_pts_free(&nearby); free(anchors); return;
        }
        json_t* ep0 = json_array_get(ep, 0);
        json_t* ids_arr = json_object_get(ep0, "corner_ids");
        int n_exp = (int)json_array_size(ids_arr);
        uint32_t* expected_ids = (uint32_t*)malloc(n_exp * sizeof(uint32_t));
        for (int i = 0; i < n_exp; i++)
            expected_ids[i] = (uint32_t)json_integer_value(json_array_get(ids_arr, i));

        v7_bp2_anchor_t* perturbed = (v7_bp2_anchor_t*)malloc(n_anchors * sizeof(*perturbed));
        int n_match = 0;
        int n_ok    = 0;
        stab_rng_state = 0xC0FFEE01u;
        for (int r = 0; r < n_runs; r++) {
            memcpy(perturbed, anchors, n_anchors * sizeof(*perturbed));
            for (size_t i = 0; i < n_anchors; i++) {
                perturbed[i].point[0] += stab_gauss(sigma_m);
                perturbed[i].point[1] += stab_gauss(sigma_m);
                perturbed[i].point[2] += stab_gauss(sigma_m);
            }
            v7_bp2_inputs_t in = {0};
            in.anchors = perturbed;
            in.n_anchors = n_anchors;
            in.group_pts = group.pts; in.n_group_pts = group.n;
            in.nearby_pts = nearby.pts; in.n_nearby_pts = nearby.n;
            in.basis = basis;
            in.contours   = contours;
            in.n_contours = n_contours;
            in.is_wall = j_bool_or(inputs, "is_wall", false);
            v7_bp2_output_t out; v7_bp2_output_init(&out);
            int rc = v7_bp2_compute(&in, &out);
            if (rc == 0 && out.n_polygons > 0) {
                n_ok++;
                if (cycle_equal(expected_ids, n_exp,
                                out.polygons[0].corner_ids,
                                (int)out.polygons[0].n_corners)) {
                    n_match++;
                }
            }
            v7_bp2_output_free(&out);
        }
        double match_rate = (double)n_match / n_runs;
        double ok_rate = (double)n_ok / n_runs;
        EXPECT(tc, match_rate >= min_rate,
               "stability: %d runs σ=%.1fmm, ok=%.1f%% match=%.1f%% (need ≥%.1f%%)",
               n_runs, sigma_mm, 100*ok_rate, 100*match_rate, 100*min_rate);
        free(perturbed); free(expected_ids);
        free_contours(contours, n_contours);
        gen_pts_free(&group); gen_pts_free(&nearby); free(anchors); return;
    }

    v7_bp2_inputs_t in = {0};
    in.anchors = anchors;
    in.n_anchors = n_anchors;
    in.group_pts = group.pts; in.n_group_pts = group.n;
    in.nearby_pts = nearby.pts; in.n_nearby_pts = nearby.n;
    in.basis = basis;
    in.contours   = contours;
    in.n_contours = n_contours;
    in.is_wall = j_bool_or(inputs, "is_wall", false);

    v7_bp2_output_t out; v7_bp2_output_init(&out);
    int rc = v7_bp2_compute(&in, &out);
    if (rc < 0) { fail_fixture(tc, "compute returned error %d", rc); }
    else if (rc > 0) {
        // No polygon produced — only OK if expected.polygons is empty/null.
        json_t* ep = json_object_get(expected, "polygons");
        if (json_is_array(ep) && json_array_size(ep) > 0) {
            fail_fixture(tc, "compute produced no polygon but fixture expected one");
        }
    } else if (out.n_polygons < 1) {
        fail_fixture(tc, "compute returned 0 polygons");
    } else {
        check_polygon_expected(tc, &out.polygons[0], json_object_get(expected, "polygons"));
        double max_wsd_m = j_num_or(expected, "max_wsd_m", 1e9);
        if (out.polygons[0].wsd > max_wsd_m) {
            fail_fixture(tc, "wsd %.4f > max %.4f", out.polygons[0].wsd, max_wsd_m);
        }
    }

    v7_bp2_output_free(&out);
    gen_pts_free(&group);
    gen_pts_free(&nearby);
    free_contours(contours, n_contours);
    free(anchors);
}

// Process one unit-test fixture. The fixture's "category" disambiguates
// which primitive to call. Each sub-category schema is implicit.
static void run_unit_fixture(test_ctx_t* tc, fixture_t* f) {
    const char* cat = f->category;
    json_t* inputs = f->inputs;
    json_t* expected = f->expected;
    if (strcmp(cat, "unit-wsd") == 0) {
        v7_bp2_basis_t basis; load_basis(inputs, &basis);
        size_t n_anchors;
        v7_bp2_anchor_t* anchors = load_anchors(inputs, &n_anchors);
        gen_pts_t group  = load_pts_or_generate(inputs, "group_pts",  anchors, n_anchors, &basis, true);
        gen_pts_t nearby = load_pts_or_generate(inputs, "nearby_pts", anchors, n_anchors, &basis, false);
        // Build polygon from "cycle" array of anchor indices, or default to [0..n-1].
        json_t* cycle_arr = json_object_get(inputs, "cycle");
        int n_cycle = cycle_arr && json_is_array(cycle_arr) ? (int)json_array_size(cycle_arr) : (int)n_anchors;
        uint32_t* cycle = (uint32_t*)malloc(n_cycle * sizeof(uint32_t));
        if (cycle_arr && json_is_array(cycle_arr)) {
            for (int i = 0; i < n_cycle; i++) cycle[i] = (uint32_t)json_integer_value(json_array_get(cycle_arr, i));
        } else {
            for (int i = 0; i < n_cycle; i++) cycle[i] = (uint32_t)i;
        }
        double* uU = (double*)malloc(n_cycle * sizeof(double));
        double* uV = (double*)malloc(n_cycle * sizeof(double));
        for (int i = 0; i < n_cycle; i++) {
            v7_bp2_project_uv(anchors[cycle[i]].point, &basis, &uU[i], &uV[i]);
        }
        // Optional holes: array of objects each with "vertices_uv": [[u,v],...]
        json_t* holes_arr = json_object_get(inputs, "holes");
        int n_holes = holes_arr && json_is_array(holes_arr) ? (int)json_array_size(holes_arr) : 0;
        double** holesU = NULL; double** holesV = NULL; int* holes_n = NULL;
        if (n_holes > 0) {
            holesU = (double**)calloc(n_holes, sizeof(double*));
            holesV = (double**)calloc(n_holes, sizeof(double*));
            holes_n = (int*)calloc(n_holes, sizeof(int));
            for (int h = 0; h < n_holes; h++) {
                json_t* hole = json_array_get(holes_arr, h);
                json_t* verts = json_object_get(hole, "vertices_uv");
                int nh = (int)json_array_size(verts);
                holesU[h] = (double*)malloc(nh * sizeof(double));
                holesV[h] = (double*)malloc(nh * sizeof(double));
                holes_n[h] = nh;
                for (int i = 0; i < nh; i++) {
                    json_t* p = json_array_get(verts, i);
                    holesU[h][i] = j_real_at(p, 0);
                    holesV[h][i] = j_real_at(p, 1);
                }
            }
        }
        double wsd = v7_bp2_wsd_multi(group.pts, group.n, NULL,
                                       nearby.pts, nearby.n, NULL,
                                       &basis, uU, uV, n_cycle,
                                       (const double* const*)holesU, (const double* const*)holesV,
                                       holes_n, n_holes,
                                       !j_bool_or(inputs, "is_wall", false));
        double expected_wsd = j_num_or(expected, "wsd", 0);
        double tol = j_num_or(expected, "tol", 0.01);
        EXPECT(tc, fabs(wsd - expected_wsd) <= tol,
               "wsd: expected %.4f tol %.4f got %.4f", expected_wsd, tol, wsd);
        for (int h = 0; h < n_holes; h++) { free(holesU[h]); free(holesV[h]); }
        free(holesU); free(holesV); free(holes_n);
        free(uU); free(uV); free(cycle);
        gen_pts_free(&group); gen_pts_free(&nearby); free(anchors);
    } else if (strcmp(cat, "unit-spike") == 0) {
        // Inputs: polygon vertices as flat [u,v] pairs.
        json_t* verts = json_object_get(inputs, "vertices_uv");
        int n = (int)json_array_size(verts);
        double* uU = (double*)malloc(n * sizeof(double));
        double* uV = (double*)malloc(n * sizeof(double));
        for (int i = 0; i < n; i++) {
            json_t* p = json_array_get(verts, i);
            uU[i] = j_real_at(p, 0);
            uV[i] = j_real_at(p, 1);
        }
        double thresh = j_num_or(inputs, "spike_threshold_deg", 30.0);
        double penalty = v7_bp2_spike_penalty_uv(uU, uV, n, thresh);
        double exp_min = j_num_or(expected, "min", -1);
        double exp_max = j_num_or(expected, "max", 1e9);
        EXPECT(tc, penalty >= exp_min && penalty <= exp_max,
               "spike: expected in [%.4f, %.4f] got %.4f", exp_min, exp_max, penalty);
        free(uU); free(uV);
    } else if (strcmp(cat, "unit-self-intersect") == 0) {
        // Inputs: polygon vertices as flat [u,v] pairs (cyclic, i+1 wraps to 0).
        // Expected: { "self_intersects": true|false }.
        // Exercises v7_bp2_polygon_self_intersects_uv — the hard guard used by
        // both the (retired) WSD-TSP cost and the bp3 windowed-or-opt refinement.
        json_t* verts = json_object_get(inputs, "vertices_uv");
        int n = (int)json_array_size(verts);
        double* uU = (double*)malloc(n * sizeof(double));
        double* uV = (double*)malloc(n * sizeof(double));
        for (int i = 0; i < n; i++) {
            json_t* p = json_array_get(verts, i);
            uU[i] = j_real_at(p, 0);
            uV[i] = j_real_at(p, 1);
        }
        bool si = v7_bp2_polygon_self_intersects_uv(uU, uV, n);
        json_t* exp = json_object_get(expected, "self_intersects");
        bool want = (exp && json_is_true(exp));
        EXPECT(tc, si == want, "self_intersects: expected %s got %s",
               want ? "true" : "false", si ? "true" : "false");
        free(uU); free(uV);
    } else if (strcmp(cat, "unit-local-edges-cross") == 0) {
        // Inputs: polygon vertices as flat [u,v] pairs (cyclic), plus "at_idx".
        // Expected: { "cross": true|false }.
        // Exercises v7_local_edges_cross_uv — the O(n) local guard for the two
        // ring edges incident to a moved/inserted vertex (T2 of make-shared).
        json_t* verts = json_object_get(inputs, "vertices_uv");
        int n = (int)json_array_size(verts);
        double* uU = (double*)malloc(n * sizeof(double));
        double* uV = (double*)malloc(n * sizeof(double));
        for (int i = 0; i < n; i++) {
            json_t* p = json_array_get(verts, i);
            uU[i] = j_real_at(p, 0);
            uV[i] = j_real_at(p, 1);
        }
        int at_idx = j_int_or(inputs, "at_idx", 0);
        bool cr = v7_local_edges_cross_uv(uU, uV, n, at_idx);
        json_t* exp = json_object_get(expected, "cross");
        bool want = (exp && json_is_true(exp));
        EXPECT(tc, cr == want, "local_edges_cross at %d: expected %s got %s",
               at_idx, want ? "true" : "false", cr ? "true" : "false");
        free(uU); free(uV);
    } else if (strcmp(cat, "unit-local-edges-cross-crosscheck") == 0) {
        // Cross-check the O(n) local test against the whole-ring O(n²) test.
        // Method: take a clean (non-self-intersecting) base ring, move EXACTLY
        // one vertex. The only edges that changed are the two incident to it,
        // so any new self-intersection must involve them →
        //   whole_ring_self_intersects(moved) == local_edges_cross(moved, idx).
        // We sweep many random convex base rings × many random single-vertex
        // moves and assert exact agreement on every one. This is the spec's
        // case (c): the local test is consistent with the whole-ring test when
        // the only possible crossing involves the moved vertex's edges.
        unsigned seed = (unsigned)j_int_or(inputs, "seed", 12345);
        int trials   = j_int_or(inputs, "trials", 2000);
        int mismatches = 0, agreed_true = 0, agreed_false = 0;
        unsigned rng = seed ? seed : 1u;
        // xorshift32 — self-contained, deterministic.
        #define CC_RAND() (rng ^= rng << 13, rng ^= rng >> 17, rng ^= rng << 5, rng)
        for (int t = 0; t < trials && mismatches == 0; t++) {
            int n = 4 + (int)(CC_RAND() % 7);   // 4..10 vertices
            double bU[16], bV[16];
            // Build a clean convex ring: vertices on a circle in angle order
            // (strictly increasing angle → simple, convex, no self-int), with a
            // mild per-vertex radius jitter that preserves convexity ordering.
            for (int i = 0; i < n; i++) {
                double frac = (i + 0.5) / (double)n;       // strictly increasing
                double ang  = frac * 2.0 * M_PI;
                double r    = 8.0 + ((double)(CC_RAND() % 1000) / 1000.0) * 2.0; // 8..10
                bU[i] = r * cos(ang);
                bV[i] = r * sin(ang);
            }
            // Sanity: the base ring must be clean (else the equivalence premise
            // — "only the moved edges changed" — would not isolate the move).
            if (v7_bp2_polygon_self_intersects_uv(bU, bV, n)) continue;
            int idx = (int)(CC_RAND() % (unsigned)n);
            // Move that one vertex somewhere in a wide box (sometimes folding it
            // across the ring → a genuine self-intersection, sometimes not).
            double mU[16], mV[16];
            memcpy(mU, bU, n * sizeof(double));
            memcpy(mV, bV, n * sizeof(double));
            mU[idx] = -14.0 + ((double)(CC_RAND() % 2800) / 1000.0); // -14..14
            mV[idx] = -14.0 + ((double)(CC_RAND() % 2800) / 1000.0);
            bool whole = v7_bp2_polygon_self_intersects_uv(mU, mV, n);
            bool local = v7_local_edges_cross_uv(mU, mV, n, idx);
            if (whole != local) {
                mismatches++;
                fail_fixture(tc, "crosscheck n=%d idx=%d: whole=%s local=%s (move to %.3f,%.3f)",
                             n, idx, whole ? "T" : "F", local ? "T" : "F",
                             mU[idx], mV[idx]);
            } else if (whole) agreed_true++; else agreed_false++;
        }
        #undef CC_RAND
        EXPECT(tc, mismatches == 0,
               "local vs whole-ring disagreed on %d case(s)", mismatches);
        // Demand the sweep actually exercised BOTH outcomes, else it proves
        // nothing (e.g. all-false would pass a stub that returns false).
        EXPECT(tc, agreed_true > 0 && agreed_false > 0,
               "crosscheck not meaningful: agreed_true=%d agreed_false=%d",
               agreed_true, agreed_false);
        fprintf(stderr, "  ....  [%s]: crosscheck %d trials, agree(true)=%d agree(false)=%d\n",
                tc->fixture_id, trials, agreed_true, agreed_false);
    } else if (strcmp(cat, "unit-best-cycle") == 0) {
        // Inputs: anchors + optional subset_idx + mode ("geometric"|"functional")
        //         + optional group/nearby for functional mode.
        // Expected: "cycle" array of anchor IDs (compared mod rotation+reflection).
        v7_bp2_basis_t basis; load_basis(inputs, &basis);
        size_t n_anchors;
        v7_bp2_anchor_t* anchors = load_anchors(inputs, &n_anchors);
        gen_pts_t group  = load_pts_or_generate(inputs, "group_pts",  anchors, n_anchors, &basis, true);
        gen_pts_t nearby = load_pts_or_generate(inputs, "nearby_pts", anchors, n_anchors, &basis, false);
        json_t* subset_arr = json_object_get(inputs, "subset_idx");
        int n_idx = subset_arr ? (int)json_array_size(subset_arr) : (int)n_anchors;
        uint32_t* subset = (uint32_t*)malloc(n_idx * sizeof(uint32_t));
        if (subset_arr) {
            for (int i = 0; i < n_idx; i++) subset[i] = (uint32_t)json_integer_value(json_array_get(subset_arr, i));
        } else {
            for (int i = 0; i < n_idx; i++) subset[i] = (uint32_t)i;
        }
        const char* mode_s = j_str_or(inputs, "mode", "geometric");
        v7_bp2_mode_t mode = (strcmp(mode_s, "functional") == 0)
                              ? V7_BP2_MODE_FUNCTIONAL : V7_BP2_MODE_GEOMETRIC;
        v7_bp2_cost_ctx_t ctx = {0};
        ctx.anchors = anchors;
        ctx.basis = basis;
        ctx.group_pts = group.pts; ctx.n_group = group.n;
        ctx.nearby_pts = nearby.pts; ctx.n_nearby = nearby.n;
        ctx.weight_wsd = V7_BP2_WEIGHT_WSD;
        ctx.weight_compactness = V7_BP2_WEIGHT_COMPACTNESS;
        ctx.weight_spike = V7_BP2_WEIGHT_SPIKE;
        ctx.spike_threshold_deg = V7_BP2_SPIKE_THRESHOLD_DEG;
        ctx.include_foreign_in = !j_bool_or(inputs, "is_wall", false);
        uint32_t* out_cycle = (uint32_t*)malloc(n_idx * sizeof(uint32_t));
        double cost = 0;
        int rc = v7_bp2_best_cycle(anchors, subset, n_idx, mode, &ctx, out_cycle, &cost);
        if (rc != 0) { fail_fixture(tc, "best_cycle rc=%d", rc); free(out_cycle); free(subset); gen_pts_free(&group); gen_pts_free(&nearby); free(anchors); return; }
        // Translate out_cycle from anchor INDICES → anchor IDs for comparison.
        uint32_t* out_ids = (uint32_t*)malloc(n_idx * sizeof(uint32_t));
        for (int i = 0; i < n_idx; i++) out_ids[i] = anchors[out_cycle[i]].id;
        // Expected.
        json_t* exp_arr = json_object_get(expected, "cycle");
        if (exp_arr && json_is_array(exp_arr)) {
            int ne = (int)json_array_size(exp_arr);
            uint32_t* exp_ids = (uint32_t*)malloc(ne * sizeof(uint32_t));
            for (int i = 0; i < ne; i++) exp_ids[i] = (uint32_t)json_integer_value(json_array_get(exp_arr, i));
            if (!cycle_equal(exp_ids, ne, out_ids, n_idx)) {
                char ebuf[256]={0}, abuf[256]={0}; char* ep=ebuf; char* ap=abuf;
                size_t er=sizeof(ebuf)-1, ar=sizeof(abuf)-1;
                for (int i=0;i<ne && er>8;i++) { int w=snprintf(ep,er,"%u%s",exp_ids[i],i+1<ne?",":""); ep+=w; er-=w; }
                for (int i=0;i<n_idx && ar>8;i++) { int w=snprintf(ap,ar,"%u%s",out_ids[i],i+1<n_idx?",":""); ap+=w; ar-=w; }
                fail_fixture(tc, "best_cycle mismatch: expected=[%s] actual=[%s]", ebuf, abuf);
            }
            free(exp_ids);
        }
        json_t* exp_no_si = json_object_get(expected, "no_self_intersect");
        if (exp_no_si && json_is_true(exp_no_si)) {
            // Project to UV + check.
            double* uU = (double*)malloc(n_idx * sizeof(double));
            double* uV = (double*)malloc(n_idx * sizeof(double));
            for (int i = 0; i < n_idx; i++) {
                v7_bp2_project_uv(anchors[out_cycle[i]].point, &basis, &uU[i], &uV[i]);
            }
            bool si = v7_bp2_polygon_self_intersects_uv(uU, uV, n_idx);
            EXPECT(tc, !si, "best_cycle should not self-intersect%s", "");
            free(uU); free(uV);
        }
        free(out_ids); free(out_cycle); free(subset);
        gen_pts_free(&group); gen_pts_free(&nearby); free(anchors);
    } else if (strcmp(cat, "unit-cost-order") == 0) {
        // Two cycles A and B; expected A's cost (relation) B's cost.
        // Supports separate anchor sets via "anchors_a" / "anchors_b" so we
        // can compare cycles built on different shapes (rect vs square etc.).
        v7_bp2_basis_t basis; load_basis(inputs, &basis);
        json_t* tmp_a = json_object_get(inputs, "anchors_a");
        json_t* tmp_b = json_object_get(inputs, "anchors_b");
        size_t na_anch = 0, nb_anch = 0;
        v7_bp2_anchor_t *anch_a = NULL, *anch_b = NULL;
        if (tmp_a && tmp_b) {
            // Two distinct anchor sets — bounce each through a transient
            // wrapper object so load_anchors can find them under "anchors".
            json_t* wa = json_pack("{s:O}", "anchors", tmp_a);
            anch_a = load_anchors(wa, &na_anch);
            json_decref(wa);
            json_t* wb = json_pack("{s:O}", "anchors", tmp_b);
            anch_b = load_anchors(wb, &nb_anch);
            json_decref(wb);
        } else {
            anch_a = load_anchors(inputs, &na_anch);
            anch_b = load_anchors(inputs, &nb_anch);
        }
        json_t* a_arr = json_object_get(inputs, "cycle_a");
        json_t* b_arr = json_object_get(inputs, "cycle_b");
        int na = (int)json_array_size(a_arr); int nb = (int)json_array_size(b_arr);
        uint32_t* a = (uint32_t*)malloc(na * sizeof(uint32_t));
        uint32_t* b = (uint32_t*)malloc(nb * sizeof(uint32_t));
        for (int i = 0; i < na; i++) a[i] = (uint32_t)json_integer_value(json_array_get(a_arr, i));
        for (int i = 0; i < nb; i++) b[i] = (uint32_t)json_integer_value(json_array_get(b_arr, i));
        double ca = v7_bp2_cost_geometric(anch_a, a, na, &basis,
                                            V7_BP2_WEIGHT_COMPACTNESS, V7_BP2_WEIGHT_SPIKE,
                                            V7_BP2_SPIKE_THRESHOLD_DEG);
        double cb = v7_bp2_cost_geometric(anch_b, b, nb, &basis,
                                            V7_BP2_WEIGHT_COMPACTNESS, V7_BP2_WEIGHT_SPIKE,
                                            V7_BP2_SPIKE_THRESHOLD_DEG);
        const char* relation = j_str_or(expected, "relation", "lt");
        bool ok = false;
        if (strcmp(relation, "lt") == 0) ok = ca < cb;
        else if (strcmp(relation, "gt") == 0) ok = ca > cb;
        else if (strcmp(relation, "eq") == 0) ok = fabs(ca - cb) < 1e-6;
        else if (strcmp(relation, "le") == 0) ok = ca <= cb;
        EXPECT(tc, ok, "cost order: A=%.4f %s B=%.4f", ca, relation, cb);
        free(a); free(b); free(anch_a); free(anch_b);
    } else {
        // Unknown unit category — skip with warning.
        fprintf(stderr, "  SKIP  [%s]: unknown unit category '%s'\n", tc->fixture_id, cat);
        tc->n_skip++;
        // Don't mark as failed, but also not as passed.
        tc->fixture_failed = true;
        tc->n_fail--;  // undo the increment from fail_fixture style accounting
        // ...actually simpler: just return without affecting counters
        return;
    }
}

static int load_fixture(const char* path, fixture_t* f) {
    json_error_t err;
    json_t* root = json_load_file(path, 0, &err);
    if (!root) {
        fprintf(stderr, "  ERROR loading %s: %s (line %d)\n", path, err.text, err.line);
        return -1;
    }
    snprintf(f->id, sizeof(f->id), "%s", j_str_or(root, "id", "(unnamed)"));
    snprintf(f->category, sizeof(f->category), "%s", j_str_or(root, "category", "smoke"));
    f->root = root;
    f->inputs = json_object_get(root, "inputs");
    f->expected = json_object_get(root, "expected");
    return 0;
}

static int run_one(test_ctx_t* tc, const char* path) {
    fixture_t f = {0};
    if (load_fixture(path, &f) != 0) return -1;
    tc->fixture_id = f.id;
    tc->fixture_failed = false;
    if (strncmp(f.category, "unit-", 5) == 0) {
        run_unit_fixture(tc, &f);
    } else {
        run_e2e_fixture(tc, &f);
    }
    if (!tc->fixture_failed) {
        tc->n_pass++;
        fprintf(stderr, "  PASS  [%s]\n", f.id);
    }
    json_decref(f.root);
    return 0;
}

static int walk_dir(test_ctx_t* tc, const char* dir) {
    DIR* d = opendir(dir);
    if (!d) {
        fprintf(stderr, "Cannot open fixtures dir: %s\n", dir);
        return -1;
    }
    struct dirent* e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            walk_dir(tc, path);
        } else if (S_ISREG(st.st_mode)) {
            const char* dot = strrchr(e->d_name, '.');
            if (dot && strcmp(dot, ".json") == 0) {
                run_one(tc, path);
            }
        }
    }
    closedir(d);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <fixtures-dir>\n", argv[0]);
        return 1;
    }
    test_ctx_t tc = {0};
    fprintf(stderr, "v7-bp2-test — running fixtures in %s\n", argv[1]);
    walk_dir(&tc, argv[1]);
    fprintf(stderr, "\nSummary: %d passed, %d failed, %d skipped\n",
            tc.n_pass, tc.n_fail, tc.n_skip);
    return tc.n_fail == 0 ? 0 : 1;
}
