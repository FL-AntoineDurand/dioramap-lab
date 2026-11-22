// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// C-visible wrapper for extracting a disc-filtered point subset from a
// COPC-LAZ tile held entirely in memory.

#include "v3/lazperf.h"

#include <cstring>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <copc-lib/geometry/box.hpp>
#include <copc-lib/hierarchy/node.hpp>
#include <copc-lib/io/copc_reader.hpp>
#include <copc-lib/las/point.hpp>

// ---------------------------------------------------------------------------
// Helper: view over a contiguous byte buffer as a std::istream. copc-lib's
// Reader takes std::istream*; we need a zero-copy adapter so we don't
// duplicate 100-500 MB per tile.
// ---------------------------------------------------------------------------

namespace {

class MembufStreambuf : public std::streambuf {
public:
    MembufStreambuf(const char* data, size_t len) {
        setg(const_cast<char*>(data), const_cast<char*>(data), const_cast<char*>(data) + len);
    }

protected:
    // Needed so Reader can seek via seekg/tellg.
    pos_type seekoff(off_type off, std::ios_base::seekdir dir,
                     std::ios_base::openmode which = std::ios_base::in) override {
        (void)which;
        const char* base = eback();
        const char* end = egptr();
        const char* cur = gptr();
        off_type newpos = 0;
        if (dir == std::ios_base::beg)
            newpos = off;
        else if (dir == std::ios_base::cur)
            newpos = (cur - base) + off;
        else /* end */
            newpos = (end - base) + off;
        if (newpos < 0 || newpos > (end - base))
            return pos_type(off_type(-1));
        setg(const_cast<char*>(base), const_cast<char*>(base) + newpos, const_cast<char*>(end));
        return pos_type(newpos);
    }
    pos_type seekpos(pos_type pos, std::ios_base::openmode which = std::ios_base::in) override {
        return seekoff(off_type(pos), std::ios_base::beg, which);
    }
};

class MemIstream : public std::istream {
public:
    MemIstream(const char* data, size_t len) : std::istream(&buf_), buf_(data, len) {
        rdbuf(&buf_);
    }

private:
    MembufStreambuf buf_;
};

} // namespace

// ---------------------------------------------------------------------------
// Output buffer helpers (C-visible struct).
// ---------------------------------------------------------------------------

extern "C" void laz_pointbuf_init(laz_pointbuf_t* pb) {
    if (!pb)
        return;
    pb->xyz = nullptr;
    pb->n_points = 0;
    pb->cap_points = 0;
    pb->classifications = nullptr;
    pb->intensities = nullptr;
    pb->num_returns = nullptr;
}

extern "C" void laz_pointbuf_free(laz_pointbuf_t* pb) {
    if (!pb)
        return;
    std::free(pb->xyz);
    std::free(pb->classifications);
    std::free(pb->intensities);
    std::free(pb->num_returns);
    laz_pointbuf_init(pb);
}

static int ensure_capacity(laz_pointbuf_t* pb, size_t needed) {
    if (needed <= pb->cap_points)
        return 0;
    size_t nc = pb->cap_points ? pb->cap_points * 2 : 8192;
    while (nc < needed)
        nc *= 2;
    double* xyz = (double*)std::realloc(pb->xyz, nc * 3 * sizeof(double));
    uint8_t* cls = (uint8_t*)std::realloc(pb->classifications, nc * sizeof(uint8_t));
    uint16_t* in = (uint16_t*)std::realloc(pb->intensities, nc * sizeof(uint16_t));
    uint8_t* nr = (uint8_t*)std::realloc(pb->num_returns, nc * sizeof(uint8_t));
    if (!xyz || !cls || !in || !nr)
        return -1;
    pb->xyz = xyz;
    pb->classifications = cls;
    pb->intensities = in;
    pb->num_returns = nr;
    pb->cap_points = nc;
    return 0;
}

// ---------------------------------------------------------------------------
// Main entry.
// ---------------------------------------------------------------------------

extern "C" int laz_extract_disc_from_tile(const uint8_t* bytes, size_t len,
                                          const double bbox_l93[4], double disc_cx,
                                          double disc_cy, double disc_r, int max_depth,
                                          laz_pointbuf_t* out) {
    if (!bytes || !bbox_l93 || !out)
        return -1;
    try {
        MemIstream stream(reinterpret_cast<const char*>(bytes), len);
        copc::Reader reader(&stream);

        copc::Box query(bbox_l93[0], bbox_l93[1], bbox_l93[2], bbox_l93[3]);
        auto points = reader.GetPointsWithinBox(query);

        const double r2 = disc_r * disc_r;
        for (const auto& pt : points.Get()) {
            double x = pt->X();
            double y = pt->Y();
            double dx = x - disc_cx;
            double dy = y - disc_cy;
            if (dx * dx + dy * dy > r2)
                continue;

            size_t i = out->n_points;
            if (ensure_capacity(out, i + 1) != 0)
                return -1;
            out->xyz[i * 3 + 0] = x;
            out->xyz[i * 3 + 1] = y;
            out->xyz[i * 3 + 2] = pt->Z();
            out->classifications[i] = pt->Classification();
            out->intensities[i] = pt->Intensity();
            out->num_returns[i] = pt->NumberOfReturns();
            out->n_points = i + 1;

            (void)max_depth; // copc-lib's GetPointsWithinBox walks the octree
                             // at full depth by default; we can add a
                             // resolution cutoff later if needed.
        }
        return 0;
    } catch (const std::exception& e) {
        (void)e;
        return -1;
    }
}
