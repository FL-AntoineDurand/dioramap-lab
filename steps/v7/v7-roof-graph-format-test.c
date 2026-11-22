// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-roof-graph-format-test — unit tests for the TLV machinery.
// Covers:
//   1. round-trip — write 2 sections (one fixed-stride, one variable),
//      read them back, verify byte-equal records
//   2. pass-through — write A in producer-1, pass A through producer-2
//      while writing B; read back, A bytes are identical
//   3. unknown-section skip — pass through a private-extension section
//      (≥0x80) verbatim
//   4. empty / zero-record sections + zero-section file
//   5. duplicate-type rejection (writer + reader)
//   6. garbage/truncated input rejection
//
// Run: ./build-dev/bin/v7-roof-graph-format-test (exits 0 on pass).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v7-roof-graph-format.h"

#define ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "ASSERT FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

static void le_put_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v); p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void le_put_u64(uint8_t* p, uint64_t v) {
    le_put_u32(p, (uint32_t)v);
    le_put_u32(p + 4, (uint32_t)(v >> 32));
}

// ────────────────────────────────────────────────────────────────────
// Test 1: round-trip — fixed-stride PLANES + variable-stride CORNERS.
// ────────────────────────────────────────────────────────────────────
static void test_roundtrip_basic(void) {
    rg_writer_t w; rg_writer_init(&w);

    // PLANES section: 2 fixed-stride records.
    ASSERT(rg_writer_section_begin(&w, RG_SECT_PLANES, sizeof(rg_plane_record_t)) == 0);
    rg_plane_record_t p0 = {0};
    p0.id = 7; p0.flags = RG_PLANE_HAS_BASIS; p0.tilt_deg = 35;
    p0.centroid[0] = 1.0; p0.centroid[1] = 2.0; p0.centroid[2] = 3.0;
    p0.normal[0] = 0.0; p0.normal[1] = 1.0; p0.normal[2] = 0.0;
    p0.d = -1.5; p0.n_pts = 1234;
    p0.basis_e1[0] = 1.0; p0.basis_e2[1] = 1.0;
    p0.plane_type = RG_PLANE_TYPE_WALL;
    rg_plane_record_t p1 = {0};
    p1.id = 9; p1.tilt_deg = 0; p1.n_pts = 0;
    p1.plane_type = RG_PLANE_TYPE_GROUND;
    ASSERT(rg_writer_record(&w, &p0, sizeof(p0)) == 0);
    ASSERT(rg_writer_record(&w, &p1, sizeof(p1)) == 0);
    ASSERT(rg_writer_section_end(&w) == 0);

    // CORNERS section: 2 variable-length records (just bytes — we test the
    // outer var-len framing, not the corner sub-layout).
    ASSERT(rg_writer_section_begin(&w, RG_SECT_CORNERS, 0) == 0);
    const char* rec0 = "alpha-corner";
    const char* rec1 = "beta-with-different-length";
    ASSERT(rg_writer_record(&w, rec0, strlen(rec0)) == 0);
    ASSERT(rg_writer_record(&w, rec1, strlen(rec1)) == 0);
    ASSERT(rg_writer_section_end(&w) == 0);

    uint8_t* bytes; size_t len;
    ASSERT(rg_writer_finalize(&w, &bytes, &len) == 0);
    ASSERT(bytes != NULL);
    ASSERT(len > 0);

    // Read back.
    rg_reader_t r;
    ASSERT(rg_reader_open(&r, bytes, len) == 0);
    ASSERT(r.n_sections == 2);

    const uint8_t* sect; size_t sl;
    ASSERT(rg_reader_section(&r, RG_SECT_PLANES, &sect, &sl) == 0);
    uint32_t n, stride;
    ASSERT(rg_section_header(sect, sl, &n, &stride) == 0);
    ASSERT(n == 2);
    ASSERT(stride == sizeof(rg_plane_record_t));
    rg_record_iter_t it;
    ASSERT(rg_record_iter_init(&it, sect, sl) == 0);
    const uint8_t* rec; uint32_t rec_len;
    ASSERT(rg_record_iter_next(&it, &rec, &rec_len) == 1);
    ASSERT(rec_len == sizeof(rg_plane_record_t));
    ASSERT(memcmp(rec, &p0, sizeof(p0)) == 0);
    ASSERT(((const rg_plane_record_t*)rec)->plane_type == RG_PLANE_TYPE_WALL);
    ASSERT(rg_plane_is_wall((const rg_plane_record_t*)rec));
    ASSERT(!rg_plane_is_ground((const rg_plane_record_t*)rec));
    ASSERT(rg_record_iter_next(&it, &rec, &rec_len) == 1);
    ASSERT(memcmp(rec, &p1, sizeof(p1)) == 0);
    ASSERT(((const rg_plane_record_t*)rec)->plane_type == RG_PLANE_TYPE_GROUND);
    ASSERT(rg_plane_is_ground((const rg_plane_record_t*)rec));
    ASSERT(!rg_plane_is_roof_candidate((const rg_plane_record_t*)rec));  // ground != roof
    ASSERT(rg_record_iter_next(&it, &rec, &rec_len) == 0);

    ASSERT(rg_reader_section(&r, RG_SECT_CORNERS, &sect, &sl) == 0);
    ASSERT(rg_section_header(sect, sl, &n, &stride) == 0);
    ASSERT(n == 2);
    ASSERT(stride == 0);
    ASSERT(rg_record_iter_init(&it, sect, sl) == 0);
    ASSERT(rg_record_iter_next(&it, &rec, &rec_len) == 1);
    ASSERT(rec_len == strlen(rec0));
    ASSERT(memcmp(rec, rec0, rec_len) == 0);
    ASSERT(rg_record_iter_next(&it, &rec, &rec_len) == 1);
    ASSERT(rec_len == strlen(rec1));
    ASSERT(memcmp(rec, rec1, rec_len) == 0);
    ASSERT(rg_record_iter_next(&it, &rec, &rec_len) == 0);

    // Unknown section query → 1 (not present).
    ASSERT(rg_reader_section(&r, RG_SECT_DOF_TAGS, &sect, &sl) == 1);

    free(bytes);
    rg_writer_free(&w);
    fprintf(stderr, "  test_roundtrip_basic: OK\n");
}

// ────────────────────────────────────────────────────────────────────
// Test 2: pass-through — producer-2 forwards producer-1's section
// byte-identical (so far as section bytes go; offsets get rebiased).
// ────────────────────────────────────────────────────────────────────
static void test_passthrough(void) {
    rg_writer_t w1; rg_writer_init(&w1);
    ASSERT(rg_writer_section_begin(&w1, RG_SECT_PLANES, sizeof(rg_plane_record_t)) == 0);
    rg_plane_record_t p = {0};
    p.id = 1; p.tilt_deg = 45; p.normal[2] = 1.0;
    ASSERT(rg_writer_record(&w1, &p, sizeof(p)) == 0);
    ASSERT(rg_writer_section_end(&w1) == 0);

    ASSERT(rg_writer_section_begin(&w1, RG_SECT_RIDGES, sizeof(rg_ridge_record_t)) == 0);
    rg_ridge_record_t r = {0};
    r.id = 0; r.plane_a = 1; r.plane_b = 2; r.type = 1;
    r.from[0] = 0; r.to[0] = 10; r.length = 10.0;
    ASSERT(rg_writer_record(&w1, &r, sizeof(r)) == 0);
    ASSERT(rg_writer_section_end(&w1) == 0);

    uint8_t* up_bytes; size_t up_len;
    ASSERT(rg_writer_finalize(&w1, &up_bytes, &up_len) == 0);
    rg_writer_free(&w1);

    rg_reader_t up;
    ASSERT(rg_reader_open(&up, up_bytes, up_len) == 0);

    rg_writer_t w2; rg_writer_init(&w2);
    ASSERT(rg_writer_passthrough(&w2, &up, RG_SECT_NONE) == 0);
    ASSERT(rg_writer_section_begin(&w2, RG_SECT_DOF_TAGS, sizeof(rg_dof_record_t)) == 0);
    rg_dof_record_t d = {0};
    d.corner_id = 42; d.dof = 1; d.flags = RG_DOF_HAS_AXIS_A;
    d.axis_a[0] = 1.0;
    ASSERT(rg_writer_record(&w2, &d, sizeof(d)) == 0);
    ASSERT(rg_writer_section_end(&w2) == 0);

    uint8_t* out_bytes; size_t out_len;
    ASSERT(rg_writer_finalize(&w2, &out_bytes, &out_len) == 0);
    rg_writer_free(&w2);

    rg_reader_t r2;
    ASSERT(rg_reader_open(&r2, out_bytes, out_len) == 0);
    ASSERT(r2.n_sections == 3);

    const uint8_t* up_sect; size_t up_slen;
    const uint8_t* out_sect; size_t out_slen;
    ASSERT(rg_reader_section(&up, RG_SECT_PLANES, &up_sect, &up_slen) == 0);
    ASSERT(rg_reader_section(&r2, RG_SECT_PLANES, &out_sect, &out_slen) == 0);
    ASSERT(up_slen == out_slen);
    ASSERT(memcmp(up_sect, out_sect, up_slen) == 0);

    ASSERT(rg_reader_section(&up, RG_SECT_RIDGES, &up_sect, &up_slen) == 0);
    ASSERT(rg_reader_section(&r2, RG_SECT_RIDGES, &out_sect, &out_slen) == 0);
    ASSERT(up_slen == out_slen);
    ASSERT(memcmp(up_sect, out_sect, up_slen) == 0);

    ASSERT(rg_reader_section(&r2, RG_SECT_DOF_TAGS, &out_sect, &out_slen) == 0);
    rg_record_iter_t it;
    ASSERT(rg_record_iter_init(&it, out_sect, out_slen) == 0);
    const uint8_t* rec; uint32_t rl;
    ASSERT(rg_record_iter_next(&it, &rec, &rl) == 1);
    ASSERT(memcmp(rec, &d, sizeof(d)) == 0);

    free(up_bytes);
    free(out_bytes);
    fprintf(stderr, "  test_passthrough: OK\n");
}

// ────────────────────────────────────────────────────────────────────
// Test 3: unknown-section skip — pass through a producer-private type
// the current code doesn't recognize (≥0x80 reserved range).
// ────────────────────────────────────────────────────────────────────
static void test_unknown_section_skip(void) {
    rg_writer_t w1; rg_writer_init(&w1);
    const uint32_t PRIVATE_TYPE = 0xF7;
    ASSERT(rg_writer_section_begin(&w1, PRIVATE_TYPE, 16) == 0);
    uint8_t rec[16];
    for (int i = 0; i < 16; i++) rec[i] = (uint8_t)(i * 13);
    ASSERT(rg_writer_record(&w1, rec, sizeof(rec)) == 0);
    ASSERT(rg_writer_section_end(&w1) == 0);

    ASSERT(rg_writer_section_begin(&w1, RG_SECT_PLANES, sizeof(rg_plane_record_t)) == 0);
    rg_plane_record_t p = {0};
    p.id = 99; p.normal[0] = 1.0;
    ASSERT(rg_writer_record(&w1, &p, sizeof(p)) == 0);
    ASSERT(rg_writer_section_end(&w1) == 0);

    uint8_t* up_bytes; size_t up_len;
    ASSERT(rg_writer_finalize(&w1, &up_bytes, &up_len) == 0);
    rg_writer_free(&w1);

    rg_reader_t up;
    ASSERT(rg_reader_open(&up, up_bytes, up_len) == 0);
    const uint8_t* sect; size_t slen;
    ASSERT(rg_reader_section(&up, RG_SECT_PLANES, &sect, &slen) == 0);
    ASSERT(rg_reader_section(&up, PRIVATE_TYPE, &sect, &slen) == 0);

    rg_writer_t w2; rg_writer_init(&w2);
    ASSERT(rg_writer_passthrough(&w2, &up, RG_SECT_NONE) == 0);
    uint8_t* out_bytes; size_t out_len;
    ASSERT(rg_writer_finalize(&w2, &out_bytes, &out_len) == 0);
    rg_writer_free(&w2);

    rg_reader_t r2;
    ASSERT(rg_reader_open(&r2, out_bytes, out_len) == 0);
    ASSERT(r2.n_sections == 2);
    const uint8_t* up_sect; size_t up_slen;
    ASSERT(rg_reader_section(&up, PRIVATE_TYPE, &up_sect, &up_slen) == 0);
    const uint8_t* out_sect; size_t out_slen;
    ASSERT(rg_reader_section(&r2, PRIVATE_TYPE, &out_sect, &out_slen) == 0);
    ASSERT(up_slen == out_slen);
    ASSERT(memcmp(up_sect, out_sect, up_slen) == 0);

    free(up_bytes);
    free(out_bytes);
    fprintf(stderr, "  test_unknown_section_skip: OK\n");
}

// ────────────────────────────────────────────────────────────────────
// Test 4: empty / zero-record sections + zero-section file.
// ────────────────────────────────────────────────────────────────────
static void test_empty_sections(void) {
    rg_writer_t w; rg_writer_init(&w);
    ASSERT(rg_writer_section_begin(&w, RG_SECT_PLANES, sizeof(rg_plane_record_t)) == 0);
    ASSERT(rg_writer_section_end(&w) == 0);
    ASSERT(rg_writer_section_begin(&w, RG_SECT_CORNERS, 0) == 0);
    ASSERT(rg_writer_section_end(&w) == 0);

    rg_writer_t empty; rg_writer_init(&empty);
    uint8_t* eb; size_t el;
    ASSERT(rg_writer_finalize(&empty, &eb, &el) == 0);
    rg_writer_free(&empty);
    rg_reader_t er;
    ASSERT(rg_reader_open(&er, eb, el) == 0);
    ASSERT(er.n_sections == 0);
    free(eb);

    uint8_t* bytes; size_t len;
    ASSERT(rg_writer_finalize(&w, &bytes, &len) == 0);
    rg_writer_free(&w);

    rg_reader_t r;
    ASSERT(rg_reader_open(&r, bytes, len) == 0);
    ASSERT(r.n_sections == 2);

    const uint8_t* sect; size_t sl;
    ASSERT(rg_reader_section(&r, RG_SECT_PLANES, &sect, &sl) == 0);
    uint32_t n, stride;
    ASSERT(rg_section_header(sect, sl, &n, &stride) == 0);
    ASSERT(n == 0);
    ASSERT(stride == sizeof(rg_plane_record_t));
    rg_record_iter_t it;
    ASSERT(rg_record_iter_init(&it, sect, sl) == 0);
    ASSERT(rg_record_iter_next(&it, NULL, NULL) == 0);

    ASSERT(rg_reader_section(&r, RG_SECT_CORNERS, &sect, &sl) == 0);
    ASSERT(rg_section_header(sect, sl, &n, &stride) == 0);
    ASSERT(n == 0);
    ASSERT(stride == 0);

    free(bytes);
    fprintf(stderr, "  test_empty_sections: OK\n");
}

// ────────────────────────────────────────────────────────────────────
// Test 5: duplicate-type rejection (writer + reader).
// ────────────────────────────────────────────────────────────────────
static void test_duplicate_type_rejected(void) {
    // Writer side.
    rg_writer_t w; rg_writer_init(&w);
    ASSERT(rg_writer_section_begin(&w, RG_SECT_PLANES, sizeof(rg_plane_record_t)) == 0);
    ASSERT(rg_writer_section_end(&w) == 0);
    ASSERT(rg_writer_section_begin(&w, RG_SECT_PLANES, sizeof(rg_plane_record_t)) == -2);
    rg_writer_free(&w);

    // Reader side: hand-craft a buffer with two identical-type dir entries.
    // File layout: 32 B header + 2 × 24 B dir + 8 B fake section payload
    //              (we never read the payload — reader rejects on duplicate
    //              dir type before that).
    const size_t total = RG_FILE_HEADER_BYTES + 2 * RG_DIR_ENTRY_BYTES + 8;
    uint8_t buf[RG_FILE_HEADER_BYTES + 2 * RG_DIR_ENTRY_BYTES + 8];
    memset(buf, 0, total);
    memcpy(buf, RG_MAGIC, 8);
    le_put_u32(buf + 8, 0);
    le_put_u32(buf + 12, 2);
    // Both dir entries point to the same 8-byte payload after the dir.
    uint64_t pl_off = (uint64_t)(RG_FILE_HEADER_BYTES + 2 * RG_DIR_ENTRY_BYTES);
    uint8_t* e0 = buf + RG_FILE_HEADER_BYTES;
    le_put_u32(e0,     RG_SECT_PLANES);
    le_put_u32(e0 + 4, 0);
    le_put_u64(e0 + 8, pl_off);
    le_put_u64(e0 + 16, 8);
    uint8_t* e1 = e0 + RG_DIR_ENTRY_BYTES;
    le_put_u32(e1,     RG_SECT_PLANES);   // duplicate
    le_put_u32(e1 + 4, 0);
    le_put_u64(e1 + 8, pl_off);
    le_put_u64(e1 + 16, 8);
    le_put_u32(buf + pl_off,     0);  // n_records=0
    le_put_u32(buf + pl_off + 4, 0);  // stride=0

    rg_reader_t r;
    ASSERT(rg_reader_open(&r, buf, total) == -2);

    fprintf(stderr, "  test_duplicate_type_rejected: OK\n");
}

// ────────────────────────────────────────────────────────────────────
// Test 6: garbage / truncated input.
// ────────────────────────────────────────────────────────────────────
static void test_bad_input_rejected(void) {
    rg_reader_t r;
    ASSERT(rg_reader_open(&r, (const uint8_t*)"", 0) == -2);

    uint8_t buf[64] = {0};
    memcpy(buf, "RGRAPHXX", 8);  // wrong magic
    ASSERT(rg_reader_open(&r, buf, sizeof(buf)) == -1);

    memcpy(buf, RG_MAGIC, 8);
    le_put_u32(buf + 12, 5);  // n_sections=5 but no entries
    ASSERT(rg_reader_open(&r, buf, RG_FILE_HEADER_BYTES) == -2);

    fprintf(stderr, "  test_bad_input_rejected: OK\n");
}

// The canonical plane-type classifier (rg_classify_plane_type) — the writer's
// logic, one definition, unit-tested here.
static void test_plane_type_classifier(void) {
    // WALL: tilt at/above the wall threshold (regardless of class stats).
    ASSERT(rg_classify_plane_type(RG_WALL_TILT_MIN_DEG_DEFAULT, false, 0, 0) == RG_PLANE_TYPE_WALL);
    ASSERT(rg_classify_plane_type(90, true, 100, 100) == RG_PLANE_TYPE_WALL);  // wall wins over class
    // UNCLASSIFIED: horizontal but no class data to tell roof from ground.
    ASSERT(rg_classify_plane_type(0, false, 0, 0)   == RG_PLANE_TYPE_UNCLASSIFIED);
    ASSERT(rg_classify_plane_type(20, true, 0, 0)   == RG_PLANE_TYPE_UNCLASSIFIED);  // cs_total==0
    // GROUND: horizontal, class-2 majority.
    ASSERT(rg_classify_plane_type(0, true, 90, 100)  == RG_PLANE_TYPE_GROUND);
    ASSERT(rg_classify_plane_type(0, true, 51, 100)  == RG_PLANE_TYPE_GROUND);
    // ROOF: horizontal, NOT class-2 majority (e.g. building/class-6 dominant).
    ASSERT(rg_classify_plane_type(0, true, 50, 100)  == RG_PLANE_TYPE_ROOF);   // exactly 0.5 → not majority
    ASSERT(rg_classify_plane_type(30, true, 5, 100)  == RG_PLANE_TYPE_ROOF);
    fprintf(stderr, "  test_plane_type_classifier: OK\n");
}

int main(void) {
    fprintf(stderr, "v7-roof-graph-format unit tests:\n");
    test_roundtrip_basic();
    test_passthrough();
    test_unknown_section_skip();
    test_empty_sections();
    test_duplicate_type_rejected();
    test_bad_input_rejected();
    test_plane_type_classifier();
    fprintf(stderr, "ALL TESTS PASSED.\n");
    return 0;
}
