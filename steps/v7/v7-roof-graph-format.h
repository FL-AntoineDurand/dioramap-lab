// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-roof-graph-format — TLV reader + writer for the boundary chain's
// shared roof-graph artifact. Spec: v3/docs/plan/ROOF-GRAPH-FORMAT.md.
//
// The TLV is a single binary blob with:
//   [32-byte file header] [n_sections × 24-byte directory entry] [payloads]
//
// Each section has a fixed type ID (RG_SECT_*); payload starts with an
// 8-byte section header (n_records, record_stride) and contains either
// fixed-stride records or variable-length records with an 8-byte
// per-record length prefix. All multi-byte fields little-endian, all
// sub-arrays 8-byte aligned. ALL POINTERS RETURNED BY THIS API ALIAS
// THE INPUT BUFFER — caller owns the memory.
//
// Reader:
//   rg_reader_t r; rg_reader_open(&r, bytes, len);
//   const uint8_t* sect; size_t sect_len;
//   if (rg_reader_section(&r, RG_SECT_PLANES, &sect, &sect_len) == 0) {
//     // typed accessors:
//     rg_plane_iter_t it; rg_plane_iter_init(&it, sect, sect_len);
//     rg_plane_record_t p;
//     while (rg_plane_iter_next(&it, &p)) { /* ... */ }
//   }
//
// Writer:
//   rg_writer_t w; rg_writer_init(&w);
//   rg_writer_section_begin(&w, RG_SECT_PLANES, sizeof(rg_plane_record_t));
//   rg_writer_record(&w, &plane, sizeof(plane));
//   rg_writer_section_end(&w);
//   ...
//   uint8_t* bytes; size_t len;
//   rg_writer_finalize(&w, &bytes, &len);  // caller free(bytes)
//   rg_writer_free(&w);
//
// Pass-through (in a step wrapper that wants to keep upstream's section
// X verbatim while writing its own section Y):
//   rg_reader_t r;
//   rg_reader_open(&r, upstream_bytes, upstream_len);
//   rg_writer_t w; rg_writer_init(&w);
//   rg_writer_passthrough(&w, &r, /*skip*/ RG_SECT_NONE);
//   rg_writer_section_begin(&w, RG_SECT_DOF_TAGS, sizeof(rg_dof_record_t));
//   ...

#ifndef V3_STEPS_V7_V7_ROOF_GRAPH_FORMAT_H
#define V3_STEPS_V7_V7_ROOF_GRAPH_FORMAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Magic + sizes ─────────────────────────────────────────────────────
#define RG_MAGIC          "RGRAPH\0\0"   // 8 bytes
#define RG_FILE_HEADER_BYTES   32
#define RG_DIR_ENTRY_BYTES     24
#define RG_SECTION_HEADER_BYTES 8

// Section type IDs (frozen — see ROOF-GRAPH-FORMAT.md §3) ──────────
typedef enum {
    RG_SECT_NONE                 = 0x00,  // sentinel
    RG_SECT_PLANES               = 0x01,
    RG_SECT_RIDGES               = 0x02,
    RG_SECT_RIDGE_LINES          = 0x03,
    RG_SECT_NEARBY_PROJECTIONS   = 0x04,
    RG_SECT_PLANE_GRIDS          = 0x05,
    RG_SECT_CORNERS              = 0x06,
    RG_SECT__RESERVED_07         = 0x07,  // dropped 2026-05-19 (was CORNER_MEMBERS; folded into CORNERS via flag 0x40)
    RG_SECT__RESERVED_08         = 0x08,  // dropped 2026-05-11 (was PLANE_ANCHORS)
    RG_SECT_MR_POINTS_BY_PLANE   = 0x09,
    RG_SECT_EDGES                = 0x0A,
    RG_SECT_POLYGON_PATHS        = 0x0B,
    RG_SECT_PERIMETER_TAGS       = 0x0C,
    RG_SECT_NEW_ADJACENCIES      = 0x0D,
    RG_SECT_DOF_TAGS             = 0x0E,
    RG_SECT_NUDGE_TRAJECTORIES   = 0x0F,
    RG_SECT_RESIDUALS_OVERLAY    = 0x10,
    RG_SECT_PLANE_CONTOURS       = 0x11,  // per-plane grid-fill topology (marching-squares contours + inspection grids)
    RG_SECT_PLANE_VOIDS          = 0x12,  // per-plane dv2 void_id membership (which voids each plane absorbed)
    // 0x13–0x7F reserved for future. ≥0x80 producer-private; readers MUST
    // pass them through unchanged.
} rg_section_type_t;

// Reader ────────────────────────────────────────────────────────────

typedef struct rg_reader {
    const uint8_t* bytes;     // aliases caller buffer
    size_t         len;
    uint32_t       n_sections;
    const uint8_t* dir;       // points at first directory entry
} rg_reader_t;

// Open a reader on a buffer. Validates magic + directory bounds.
// Returns 0 on success, -1 on bad magic, -2 on truncated/inconsistent.
int rg_reader_open(rg_reader_t* r, const uint8_t* bytes, size_t len);

// Look up a section by type. Sets *out_bytes / *out_len to the section
// payload (excluding directory entry, including the 8-byte section
// header). Returns 0 on found, 1 on not present, -1 on inconsistent
// directory entry.
int rg_reader_section(const rg_reader_t* r, uint32_t type,
                      const uint8_t** out_bytes, size_t* out_len);

// Iterate the directory (for pass-through and inspect tooling).
// Caller passes *iter starting at 0; returns 0 + advances on each call;
// returns 1 when done. Sets *out_type, *out_bytes, *out_len each call.
int rg_reader_iter(const rg_reader_t* r, uint32_t* iter,
                   uint32_t* out_type,
                   const uint8_t** out_bytes, size_t* out_len);

// Read the section header (n_records, record_stride) from a section
// payload. Returns 0 on success, -1 on truncated.
int rg_section_header(const uint8_t* sect_bytes, size_t sect_len,
                      uint32_t* out_n_records, uint32_t* out_record_stride);

// Writer ────────────────────────────────────────────────────────────

typedef struct rg_writer {
    // Directory entries.
    struct {
        uint32_t type;
        uint64_t offset;
        uint64_t length;
    }*       dir;
    size_t   n_dir;
    size_t   dir_cap;

    // Payload arena (contains all section payloads back-to-back).
    uint8_t* payload;
    size_t   payload_len;
    size_t   payload_cap;

    // Currently-open section (between section_begin and section_end).
    bool     in_section;
    uint32_t cur_type;
    uint32_t cur_record_stride;
    uint64_t cur_section_start;   // offset into payload[]
    uint32_t cur_n_records;
} rg_writer_t;

void rg_writer_init(rg_writer_t* w);
void rg_writer_free(rg_writer_t* w);

// Begin a new section. record_stride==0 means variable-length records.
// Returns 0 on success, -1 if a section is already open or alloc fails,
// -2 if a section of this type was already finalized.
int rg_writer_section_begin(rg_writer_t* w, uint32_t type, uint32_t record_stride);

// Append one record. If record_stride was set != 0, len MUST equal
// record_stride. Returns 0 on success.
int rg_writer_record(rg_writer_t* w, const void* bytes, size_t len);

// Close the current section.
int rg_writer_section_end(rg_writer_t* w);

// Pass through every section from upstream that ISN'T `skip_type`.
// Call BEFORE writing your own sections to maintain section order
// stability. Pass RG_SECT_NONE as skip_type to forward everything.
// Returns 0 on success.
int rg_writer_passthrough(rg_writer_t* w, const rg_reader_t* upstream,
                          uint32_t skip_type);

// Like rg_writer_passthrough but skips multiple types. n_skip<=8 in
// practice. Pass NULL/0 to forward everything.
int rg_writer_passthrough_n(rg_writer_t* w, const rg_reader_t* upstream,
                            const uint32_t* skip_types, size_t n_skip);

// Emit the final TLV blob. Caller owns *out_bytes (free with free()).
// Resets writer state — caller can keep using it for another file or
// rg_writer_free().
int rg_writer_finalize(rg_writer_t* w, uint8_t** out_bytes, size_t* out_len);

// Section iterators — typed accessors ───────────────────────────────
//
// Each section's records have a public C struct with the on-wire layout.
// We provide iterators that step through records without copying.
// Variable-length sections require interpreting the per-record length
// prefix; we expose a generic iterator and per-section structured
// accessors where useful.

// Generic record iterator. Works for both fixed and variable stride.
// For fixed stride, *out_record points at the record bytes,
// *out_record_len equals stride. For variable, points at payload bytes
// after the 8-byte length-prefix, *out_record_len = the length.
// Returns 1 on next-record-available, 0 on done.
typedef struct rg_record_iter {
    const uint8_t* sect_bytes;
    size_t         sect_len;
    uint32_t       n_records;
    uint32_t       record_stride;     // 0 = variable
    size_t         off;
    uint32_t       i;
} rg_record_iter_t;

int rg_record_iter_init(rg_record_iter_t* it,
                        const uint8_t* sect_bytes, size_t sect_len);
int rg_record_iter_next(rg_record_iter_t* it,
                        const uint8_t** out_record, uint32_t* out_record_len);

// On-wire structs. All are POD; offsets/sizes match ROOF-GRAPH-FORMAT.md.
// Compiler is asked to NOT pad these. We use static_assert below to
// verify.

#pragma pack(push, 1)

typedef struct rg_plane_record {       // RG_SECT_PLANES, fixed 128 B
    uint32_t id;
    uint16_t flags;
    uint16_t tilt_deg;
    double   centroid[3];
    double   normal[3];
    double   d;
    uint32_t n_pts;
    uint32_t rejection_reason;
    uint64_t slug_id;
    double   basis_e1[3];
    double   basis_e2[3];
    uint8_t  plane_type;      // rg_plane_type_t (exclusive: unclassified/roof/wall/ground)
    uint8_t  _reserved[7];    // pad 128->136 (8-aligned) + room for future per-plane bytes
} rg_plane_record_t;

typedef struct rg_ridge_record {       // RG_SECT_RIDGES, fixed 96 B
    uint32_t id;
    uint32_t plane_a;
    uint32_t plane_b;
    uint8_t  type;
    uint8_t  _pad0[3];
    double   from[3];
    double   to[3];
    double   length;
    uint32_t contact_count;
    float    overlap_ratio;
    uint8_t  _pad1[16];
} rg_ridge_record_t;

typedef struct rg_ridge_line_record {  // RG_SECT_RIDGE_LINES, fixed 96 B
    uint32_t ridge_id;
    uint32_t flags;            // bit0 has_line, bit1 has_side_a, bit2 has_side_b
    double   line_point[3];
    double   line_direction[3];
    float    t_min_a;
    float    t_max_a;
    float    t_min_b;
    float    t_max_b;
    // Per-plane side axes. side_vec_a is a unit vector perpendicular to the
    // ridge, lying in plane A's surface, oriented INTO plane A's body; ditto
    // side_vec_b for plane B. The old single `side_vec` (built from A's normal
    // only) + side_sign_a/b pair was replaced 2026-06-11: it judged plane B's
    // corners in plane A's frame, collapsing the side test on steep dihedrals.
    // The interior orientation folds the old sign in; validity is the flag bit.
    float    side_vec_a[3];
    float    side_vec_b[3];
} rg_ridge_line_record_t;

typedef struct rg_new_adj_record {     // RG_SECT_NEW_ADJACENCIES, fixed 24 B
    uint32_t plane_a;
    uint32_t plane_b;
    uint8_t  type;
    uint8_t  _pad0[3];
    uint32_t edge_id_a;
    uint32_t edge_id_b;
    uint32_t _pad1;
} rg_new_adj_record_t;

typedef struct rg_dof_record {         // RG_SECT_DOF_TAGS, fixed 64 B
    uint32_t corner_id;
    uint8_t  dof;
    uint8_t  flags;
    uint8_t  _pad0[2];
    float    axis_a[3];
    float    axis_b[3];
    uint8_t  _pad1[32];
} rg_dof_record_t;

typedef struct rg_residual_record {    // RG_SECT_RESIDUALS_OVERLAY, fixed 16 B
    uint32_t corner_id;
    uint32_t n_planes_used;
    float    rms;
    float    max_abs;
} rg_residual_record_t;

#pragma pack(pop)

// Sanity (ABI lock) — see ROOF-GRAPH-FORMAT.md §4.
_Static_assert(sizeof(rg_plane_record_t)      == 136, "PLANES record size");
_Static_assert(sizeof(rg_ridge_record_t)      ==  96, "RIDGES record size");
_Static_assert(sizeof(rg_ridge_line_record_t) ==  96, "RIDGE_LINES record size");
_Static_assert(sizeof(rg_new_adj_record_t)    ==  24, "NEW_ADJACENCIES record size");
_Static_assert(sizeof(rg_dof_record_t)        ==  64, "DOF_TAGS record size");
_Static_assert(sizeof(rg_residual_record_t)   ==  16, "RESIDUALS_OVERLAY record size");

// Plane flag bits. (ORTHOGONAL facts only — the exclusive roof/wall/ground
// classification is the `plane_type` FIELD below, not a flag.)
#define RG_PLANE_REJECTED        0x0001
#define RG_PLANE_HAS_BASIS       0x0002
#define RG_PLANE_HAS_CLASS_STATS 0x0004
#define RG_PLANE_HAS_POLYGON     0x0008
#define RG_PLANE_HAS_DOF         0x0010

// Plane TYPE — one mutually-exclusive classification per plane, stored in the
// `plane_type` field. WRITTEN ONCE by v7-boundary-topology (the sole writer; it
// resolves precedence), READ everywhere else — never recomputed. A field, not
// flags: wall/ground/roof are exclusive values of one property. See journal
// 2026-07-03-type-aware-regularization-plan §11.
typedef enum rg_plane_type {
    RG_PLANE_TYPE_UNCLASSIFIED = 0,  // classifier didn't decide — consumers treat as roof
    RG_PLANE_TYPE_ROOF         = 1,
    RG_PLANE_TYPE_WALL         = 2,
    RG_PLANE_TYPE_GROUND       = 3,
} rg_plane_type_t;

// Default wall-tilt threshold in degrees, used by the SOLE WRITER
// (v7-boundary-topology) to classify a plane as WALL. Readers never apply it —
// they read the stored plane_type. Kept here as the writer's one source of truth.
#define RG_WALL_TILT_MIN_DEG_DEFAULT 72

// ── Plane-type readers (read the stored field; do NOT recompute) ──
static inline rg_plane_type_t rg_plane_type(const rg_plane_record_t* p) {
    return (rg_plane_type_t)p->plane_type;
}
static inline bool rg_plane_is_wall(const rg_plane_record_t* p) {
    return p->plane_type == RG_PLANE_TYPE_WALL;
}
static inline bool rg_plane_is_ground(const rg_plane_record_t* p) {
    return p->plane_type == RG_PLANE_TYPE_GROUND;
}
// A live "roof candidate": not rejected, and typed roof (or unclassified, which
// consumers treat as roof). Now EXCLUDES ground by type — callers no longer need
// the old "centroid.z above the walls" secondary ground filter.
static inline bool rg_plane_is_roof_candidate(const rg_plane_record_t* p) {
    return (p->flags & RG_PLANE_REJECTED) == 0 &&
           (p->plane_type == RG_PLANE_TYPE_ROOF ||
            p->plane_type == RG_PLANE_TYPE_UNCLASSIFIED);
}

// Plane-type CLASSIFIER — the writer's logic. v7-boundary-topology is the SOLE
// caller in the pipeline (it writes plane_type once); readers must READ the field,
// never call this. Exposed here only so there is ONE canonical definition and it is
// unit-testable. Simple v1 (journal 2026-07-03 §5): WALL by tilt; GROUND by class-2
// majority; UNCLASSIFIED when class data is missing (consumers treat as roof); ROOF
// otherwise. S4/S5 votes + geometric S2/S3 are a later refinement.
static inline rg_plane_type_t rg_classify_plane_type(uint16_t tilt_deg,
                                                     bool has_class_stats,
                                                     uint64_t cs_ground,
                                                     uint64_t cs_total) {
    if (tilt_deg >= RG_WALL_TILT_MIN_DEG_DEFAULT)   return RG_PLANE_TYPE_WALL;
    if (!has_class_stats || cs_total == 0)          return RG_PLANE_TYPE_UNCLASSIFIED;
    if ((double)cs_ground > 0.5 * (double)cs_total) return RG_PLANE_TYPE_GROUND;
    return RG_PLANE_TYPE_ROOF;
}

// May two planes of these types share a corner? Used to REFUSE cross-type corner
// merges/sharing (roof↔wall, roof↔ground, wall↔ground) so roof/wall/ground polygons
// decouple. UNCLASSIFIED is treated as roof (same as consumers), so it merges with
// roof/unclassified. Same type → mergeable. (journal 2026-07-03 §10: three-site decouple)
static inline bool rg_plane_types_mergeable(rg_plane_type_t a, rg_plane_type_t b) {
    if (a == RG_PLANE_TYPE_UNCLASSIFIED) a = RG_PLANE_TYPE_ROOF;
    if (b == RG_PLANE_TYPE_UNCLASSIFIED) b = RG_PLANE_TYPE_ROOF;
    return a == b;
}

// Corner flag bits (within first byte of variable record).
#define RG_CORNER_HAS_NAME                  0x01
#define RG_CORNER_HAS_META_BLOB             0x02
#define RG_CORNER_HAS_DIRS_BY_PLANE         0x04
#define RG_CORNER_HAS_DOF                   0x08
#define RG_CORNER_HAS_PATH                  0x10
// 0x20: per-plane CoM-direction blended bisector (one unit vec3 per
// plane the corner belongs to). Cached at signal-emission time for
// concave/convex anchors so v7-boundary-polygons doesn't recompute it
// in D.4 and v7-boundary-regularize can reuse it for nudge direction.
// See ROOF-GRAPH-FORMAT.md §4.6.
#define RG_CORNER_HAS_COM_BISECTOR_BY_PLANE 0x20
// 0x40: inline per-corner pool-merge decomposition. When set, the
// CORNERS record ends with a {u32 count; u32 _pad; count × 48-B member}
// block. The old standalone CORNER_MEMBERS section (0x07) was folded
// into CORNERS (2026-05-19) — see ROOF-GRAPH-FORMAT.md §4.6.
#define RG_CORNER_HAS_MEMBERS               0x40

// Edge flag bits (within first byte of variable record).
#define RG_EDGE_HAS_META_BLOB     0x01
#define RG_EDGE_HAS_PERIM_TAG     0x02
#define RG_EDGE_HAS_PERIM_DETAIL  0x04

// DOF flag bits.
#define RG_DOF_HAS_AXIS_A 0x01
#define RG_DOF_HAS_AXIS_B 0x02

// Plane-grid flag bits.
#define RG_PG_HAS_GROUP_FIELD  0x01
#define RG_PG_HAS_NEARBY_FIELD 0x02
#define RG_PG_HAS_BAL_FIELD    0x04

// ── Variable-length record subsection parser ──────────────────────────
//
// Read an 8-byte-aligned padded u32 count followed by `count` u32
// entries from `rec` at `*off`. Used by the RG_SECT_CORNERS variable-
// record parser at multiple step-wrapper sites (boundary-classify,
// boundary-corners, boundary-polygons-2, boundary-regularize) for the
// ridge_ids variable-length subsection.
//
// Behaviour:
//   pre-aligns *off to the next multiple of 8 (redundant pre-alignment
//                                              by the caller is harmless)
//   reads count = u32 at rec[*off], advances *off by 8 (count is padded)
//   if count > 0:
//     allocates *out_arr via safe_malloc_array(count, sizeof(uint32_t))
//     memcpys count*4 bytes from rec[*off] into *out_arr
//     advances *off by count*4
//   else:
//     *out_arr = NULL
//   post-aligns *off to the next multiple of 8.
//   *out_n = count.
//
// Returns 0 on success, -1 on safe_malloc_array failure. The caller
// is responsible for free()-ing *out_arr if it ends up non-NULL.
//
// Extracted in task #72b — previously inlined identically at 4 sites.
/*@ requires \valid(off) && \valid(out_arr) && \valid(out_n);
    assigns *off, *out_arr, *out_n;
    ensures \result == 0 || \result == -1;
*/
int rg_parse_padded_u32_array(const uint8_t* rec, size_t* off,
                              uint32_t** out_arr, uint32_t* out_n);

#ifdef __cplusplus
}
#endif
#endif
