// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-roof-graph-inspect — pretty-prints a roof-graph TLV file's
// section directory + per-section summary.
//
// Usage:
//   v7-roof-graph-inspect <file.bin>
//
// Output is human-readable; pipes well through `less`. For machine-
// readable dumps see the JS decoder + node script.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v7-roof-graph-format.h"

static const char* section_name(uint32_t type) {
    switch (type) {
        case RG_SECT_PLANES:              return "PLANES";
        case RG_SECT_RIDGES:              return "RIDGES";
        case RG_SECT_RIDGE_LINES:         return "RIDGE_LINES";
        case RG_SECT_NEARBY_PROJECTIONS:  return "NEARBY_PROJECTIONS";
        case RG_SECT_PLANE_GRIDS:         return "PLANE_GRIDS";
        case RG_SECT_CORNERS:             return "CORNERS";
        case RG_SECT__RESERVED_07:        return "<reserved-0x07>";
        case RG_SECT__RESERVED_08:        return "<reserved-0x08>";
        case RG_SECT_MR_POINTS_BY_PLANE:  return "MR_POINTS_BY_PLANE";
        case RG_SECT_EDGES:               return "EDGES";
        case RG_SECT_POLYGON_PATHS:       return "POLYGON_PATHS";
        case RG_SECT_PERIMETER_TAGS:      return "PERIMETER_TAGS";
        case RG_SECT_NEW_ADJACENCIES:     return "NEW_ADJACENCIES";
        case RG_SECT_DOF_TAGS:            return "DOF_TAGS";
        case RG_SECT_NUDGE_TRAJECTORIES:  return "NUDGE_TRAJECTORIES";
        case RG_SECT_RESIDUALS_OVERLAY:   return "RESIDUALS_OVERLAY";
        default:                          return type >= 0x80 ? "<private>" : "<reserved>";
    }
}

static const char* ridge_type_name(uint8_t t) {
    static const char* tbl[] = {
        "unknown", "ridge", "hip", "valley", "coplanar", "step", "eave",
    };
    return (t < (sizeof(tbl)/sizeof(tbl[0]))) ? tbl[t] : "?";
}
static const char* corner_source_name(uint8_t s) {
    static const char* tbl[] = {
        "0", "ridge-intersection", "ridge-end", "polygon-merge",
        "concave", "convex", "mr-group", "mr-nearby", "coplanar",
    };
    return (s < (sizeof(tbl)/sizeof(tbl[0]))) ? tbl[s] : "?";
}
static const char* perim_tag_name(uint8_t t) {
    static const char* tbl[] = {
        "untagged", "perimeter", "shared", "overhang-top",
        "overhang-bottom", "interior-shared", "wall", "level-transition",
    };
    return (t < (sizeof(tbl)/sizeof(tbl[0]))) ? tbl[t] : "?";
}

static int slurp(const char* path, uint8_t** out, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    uint8_t* buf = (uint8_t*)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return -1; }
    if ((long)fread(buf, 1, (size_t)n, f) != n) { fclose(f); free(buf); return -1; }
    fclose(f);
    *out = buf;
    *out_len = (size_t)n;
    return 0;
}

// Section-specific summaries. Keep terse — one line per record where
// possible, then show 5 records max for orientation.

// Plane id→type map (filled by summarize_planes), for the corner cross-type check.
static uint32_t g_pids[1024]; static uint8_t g_ptypes[1024]; static int g_np = 0;
static uint8_t plane_type_of(uint32_t id) {
    for (int i = 0; i < g_np; i++) if (g_pids[i] == id) return g_ptypes[i];
    return 0;  // unknown → unclassified (treated as roof)
}

static void summarize_planes(const uint8_t* sect, size_t sect_len) {
    rg_record_iter_t it;
    if (rg_record_iter_init(&it, sect, sect_len) != 0) return;
    const uint8_t* rec; uint32_t rl;
    int shown = 0, n_rejected = 0, n_total = 0, max_show = 5;
    while (rg_record_iter_next(&it, &rec, &rl)) {
        const rg_plane_record_t* p = (const rg_plane_record_t*)rec;
        n_total++;
        if (g_np < 1024) { g_pids[g_np] = p->id; g_ptypes[g_np] = p->plane_type; g_np++; }
        if (p->flags & RG_PLANE_REJECTED) n_rejected++;
        if (shown < max_show) {
            static const char* _pt[] = {"unclassified","roof","wall","ground"};
            printf("    [%2u] tilt=%2u° n=%-5u  centroid=(%.2f,%.2f,%.2f) "
                   "n=(%.3f,%.3f,%.3f)  flags=0x%04x  type=%s\n",
                   p->id, p->tilt_deg, p->n_pts,
                   p->centroid[0], p->centroid[1], p->centroid[2],
                   p->normal[0], p->normal[1], p->normal[2],
                   p->flags, p->plane_type < 4 ? _pt[p->plane_type] : "?");
            shown++;
        }
    }
    if (n_total > shown) printf("    ... %d more (total %d, %d rejected)\n", n_total - shown, n_total, n_rejected);
    else if (n_rejected) printf("    (%d rejected)\n", n_rejected);
}

static void summarize_ridges(const uint8_t* sect, size_t sect_len) {
    rg_record_iter_t it;
    if (rg_record_iter_init(&it, sect, sect_len) != 0) return;
    const uint8_t* rec; uint32_t rl;
    int shown = 0, n_total = 0, max_show = 5;
    int counts[7] = {0};
    while (rg_record_iter_next(&it, &rec, &rl)) {
        const rg_ridge_record_t* r = (const rg_ridge_record_t*)rec;
        n_total++;
        if (r->type < 7) counts[r->type]++;
        if (shown < max_show) {
            printf("    [%2u] %-7s p=%u↔%u  L=%.2fm  contacts=%u  overlap=%.2f\n",
                   r->id, ridge_type_name(r->type),
                   r->plane_a, r->plane_b, r->length, r->contact_count, r->overlap_ratio);
            shown++;
        }
    }
    if (n_total > shown) printf("    ... %d more\n", n_total - shown);
    printf("    by type:");
    for (int i = 0; i < 7; i++) {
        if (counts[i]) printf("  %s=%d", ridge_type_name(i), counts[i]);
    }
    printf("\n");
}

static void summarize_corners(const uint8_t* sect, size_t sect_len) {
    rg_record_iter_t it;
    if (rg_record_iter_init(&it, sect, sect_len) != 0) return;
    const uint8_t* rec; uint32_t rl;
    int shown = 0, n_total = 0, max_show = 5;
    int counts[9] = {0};
    while (rg_record_iter_next(&it, &rec, &rl)) {
        if (rl < 32) continue;
        uint32_t id;        memcpy(&id,  rec + 0, 4);
        uint8_t  source   = rec[4];
        uint8_t  flags    = rec[5];
        uint16_t n_merged; memcpy(&n_merged, rec + 6, 2);
        double pt[3];       memcpy(pt, rec + 8, 24);
        n_total++;
        if (source < 9) counts[source]++;
        if (shown < max_show) {
            printf("    [%4u] %-18s nM=%-2u  pt=(%.2f,%.2f,%.2f)  flags=0x%02x\n",
                   id, corner_source_name(source), n_merged, pt[0], pt[1], pt[2], flags);
            shown++;
        }
    }
    if (n_total > shown) printf("    ... %d more\n", n_total - shown);
    printf("    by source:");
    for (int i = 1; i < 9; i++) {
        if (counts[i]) printf("  %s=%d", corner_source_name(i), counts[i]);
    }
    printf("\n");

    // Plane-count histogram (n_plane_ids at offset 32, 4 bytes). Counts how
    // many corners belong to >=2 planes ("shared" corners = unified ridges).
    {
        rg_record_iter_t it2;
        if (rg_record_iter_init(&it2, sect, sect_len) == 0) {
            const uint8_t* rec2; uint32_t rl2;
            int by_np[8] = {0}, shared2plus = 0, np_total = 0, cross_type = 0;
            while (rg_record_iter_next(&it2, &rec2, &rl2)) {
                if (rl2 < 36) continue;
                uint32_t np; memcpy(&np, rec2 + 32, 4);
                np_total++;
                if (np < 8) by_np[np]++;
                if (np >= 2) {
                    shared2plus++;
                    // Cross-type shared corner: its plane_ids (offset 40) span >1 type.
                    if (rl2 >= 40 + np * 4) {
                        uint8_t t0 = 0; bool first = true, mixed = false;
                        for (uint32_t k = 0; k < np; k++) {
                            uint32_t pid; memcpy(&pid, rec2 + 40 + k * 4, 4);
                            uint8_t t = plane_type_of(pid); if (t == 0) t = 1;  // unclassified→roof
                            if (first) { t0 = t; first = false; }
                            else if (t != t0) mixed = true;
                        }
                        if (mixed) cross_type++;
                    }
                }
            }
            printf("    by n_planes:");
            for (int i = 0; i < 8; i++) if (by_np[i]) printf("  %d→%d", i, by_np[i]);
            printf("   (shared n_planes>=2: %d of %d;  CROSS-TYPE(roof/wall/ground): %d)\n",
                   shared2plus, np_total, cross_type);
        }
    }
}

// EDGES record: id(4) _pad(4) corner_a(4) corner_b(4) dir(12) len(4) then
// at offset 32: n_plane_ids(4), _pad(4), plane_ids[]. Mirrors parse_edges.
static void summarize_edges(const uint8_t* sect, size_t sect_len) {
    rg_record_iter_t it;
    if (rg_record_iter_init(&it, sect, sect_len) != 0) return;
    const uint8_t* rec; uint32_t rl;
    int by_np[8] = {0}, shared2plus = 0, n_total = 0;
    while (rg_record_iter_next(&it, &rec, &rl)) {
        if (rl < 36) { n_total++; continue; }
        uint32_t np; memcpy(&np, rec + 32, 4);
        n_total++;
        if (np < 8) by_np[np]++;
        if (np >= 2) shared2plus++;
    }
    printf("    by n_planes:");
    for (int i = 0; i < 8; i++) if (by_np[i]) printf("  %d→%d", i, by_np[i]);
    printf("   (shared n_planes>=2: %d of %d)\n", shared2plus, n_total);
}

static void summarize_new_adjacencies(const uint8_t* sect, size_t sect_len) {
    rg_record_iter_t it;
    if (rg_record_iter_init(&it, sect, sect_len) != 0) return;
    const uint8_t* rec; uint32_t rl;
    int shown = 0, n_total = 0;
    while (rg_record_iter_next(&it, &rec, &rl)) {
        const rg_new_adj_record_t* a = (const rg_new_adj_record_t*)rec;
        n_total++;
        if (shown < 5) {
            const char* tn = (a->type == 1) ? "eave-shared" :
                             (a->type == 2) ? "ridge-adjacent" : "?";
            printf("    p=%u↔%u  %s  edges=(%u,%u)\n",
                   a->plane_a, a->plane_b, tn, a->edge_id_a, a->edge_id_b);
            shown++;
        }
    }
    if (n_total > shown) printf("    ... %d more\n", n_total - shown);
}

static void summarize_perim_tags(const uint8_t* sect, size_t sect_len) {
    rg_record_iter_t it;
    if (rg_record_iter_init(&it, sect, sect_len) != 0) return;
    const uint8_t* rec; uint32_t rl;
    int counts[8] = {0};
    while (rg_record_iter_next(&it, &rec, &rl)) {
        if (rl < 8) continue;
        uint8_t tag = rec[4];
        if (tag < 8) counts[tag]++;
    }
    printf("    by tag:");
    for (int i = 0; i < 8; i++) {
        if (counts[i]) printf("  %s=%d", perim_tag_name(i), counts[i]);
    }
    printf("\n");
}

static void summarize_dof_tags(const uint8_t* sect, size_t sect_len) {
    rg_record_iter_t it;
    if (rg_record_iter_init(&it, sect, sect_len) != 0) return;
    const uint8_t* rec; uint32_t rl;
    int locked = 0, ridge_slide = 0, free_dof = 0;
    while (rg_record_iter_next(&it, &rec, &rl)) {
        const rg_dof_record_t* d = (const rg_dof_record_t*)rec;
        if (d->dof == 0) locked++;
        else if (d->dof == 1) ridge_slide++;
        else if (d->dof == 2) free_dof++;
    }
    printf("    locked=%d  ridge-slide=%d  free=%d\n", locked, ridge_slide, free_dof);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <roof-graph.bin>\n", argv[0]);
        return 2;
    }
    uint8_t* bytes = NULL;
    size_t   len = 0;
    if (slurp(argv[1], &bytes, &len) != 0) {
        fprintf(stderr, "%s: cannot read\n", argv[1]);
        return 1;
    }

    rg_reader_t r;
    int rc = rg_reader_open(&r, bytes, len);
    if (rc != 0) {
        fprintf(stderr, "%s: invalid roof-graph (rc=%d)\n", argv[1], rc);
        free(bytes);
        return 1;
    }

    printf("file:        %s\n", argv[1]);
    printf("size:        %zu bytes\n", len);
    printf("sections:    %u\n", r.n_sections);
    printf("\n");
    printf("  ID    type                length     name\n");
    printf("  ────  ──────────────────  ─────────  ──────────────────────\n");

    uint32_t iter = 0;
    uint32_t type;
    const uint8_t* sect_bytes;
    size_t sect_len;
    while (rg_reader_iter(&r, &iter, &type, &sect_bytes, &sect_len) == 0) {
        printf("  0x%02x  %-18s  %9zu  %s\n",
               type, "", sect_len, section_name(type));
    }
    printf("\n");

    // Per-section summaries.
    iter = 0;
    while (rg_reader_iter(&r, &iter, &type, &sect_bytes, &sect_len) == 0) {
        uint32_t n = 0, stride = 0;
        rg_section_header(sect_bytes, sect_len, &n, &stride);
        printf("─── %s (0x%02x) — %u records, stride=%u ───\n",
               section_name(type), type, n, stride);
        switch (type) {
            case RG_SECT_PLANES:           summarize_planes(sect_bytes, sect_len); break;
            case RG_SECT_RIDGES:           summarize_ridges(sect_bytes, sect_len); break;
            case RG_SECT_CORNERS:          summarize_corners(sect_bytes, sect_len); break;
            case RG_SECT_EDGES:            summarize_edges(sect_bytes, sect_len); break;
            case RG_SECT_NEW_ADJACENCIES:  summarize_new_adjacencies(sect_bytes, sect_len); break;
            case RG_SECT_PERIMETER_TAGS:   summarize_perim_tags(sect_bytes, sect_len); break;
            case RG_SECT_DOF_TAGS:         summarize_dof_tags(sect_bytes, sect_len); break;
            default:
                if (n > 0) printf("    (no per-section summary; %u records)\n", n);
                break;
        }
        printf("\n");
    }

    free(bytes);
    return 0;
}
