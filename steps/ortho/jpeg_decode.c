// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#include "jpeg_decode.h"

#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>

// ----------------------------------------------------------------------------
// libjpeg error handling: the default behaviour is to call exit() on any
// parse error, which we cannot tolerate inside a step. Replace `error_exit`
// with a longjmp that the caller catches.
// ----------------------------------------------------------------------------

typedef struct jpeg_err_state {
    struct jpeg_error_mgr pub; // must be first
    jmp_buf jmp_buf;
} jpeg_err_state_t;

static void on_error_exit(j_common_ptr cinfo) {
    jpeg_err_state_t* es = (jpeg_err_state_t*)cinfo->err;
    longjmp(es->jmp_buf, 1);
}

int jpeg_decode_rgba(const uint8_t* bytes, size_t len, decoded_image_t* out) {
    if (!bytes || len == 0 || !out) {
        return -1;
    }
    out->rgba = NULL;
    out->w = 0;
    out->h = 0;

    struct jpeg_decompress_struct cinfo;
    jpeg_err_state_t es;
    cinfo.err = jpeg_std_error(&es.pub);
    es.pub.error_exit = on_error_exit;

    uint8_t* rgba = NULL;
    JSAMPLE* row_buf = NULL;
    if (setjmp(es.jmp_buf)) {
        // Came here via on_error_exit longjmp.
        jpeg_destroy_decompress(&cinfo);
        free(rgba);
        free(row_buf);
        return -1;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, bytes, (unsigned long)len);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        longjmp(es.jmp_buf, 1);
    }
    cinfo.out_color_space = JCS_RGB;
    if (!jpeg_start_decompress(&cinfo)) {
        longjmp(es.jmp_buf, 1);
    }

    int w = (int)cinfo.output_width;
    int h = (int)cinfo.output_height;
    int comps = (int)cinfo.output_components;
    if (w <= 0 || h <= 0 || comps != 3) {
        longjmp(es.jmp_buf, 1);
    }
    rgba = malloc((size_t)w * h * 4);
    if (!rgba) {
        longjmp(es.jmp_buf, 1);
    }
    row_buf = malloc((size_t)w * comps);
    if (!row_buf) {
        longjmp(es.jmp_buf, 1);
    }

    while (cinfo.output_scanline < (JDIMENSION)h) {
        JSAMPROW row_ptr[1] = {row_buf};
        if (jpeg_read_scanlines(&cinfo, row_ptr, 1) != 1) {
            longjmp(es.jmp_buf, 1);
        }
        int y = (int)cinfo.output_scanline - 1;
        uint8_t* dst = rgba + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            dst[x * 4 + 0] = row_buf[x * 3 + 0];
            dst[x * 4 + 1] = row_buf[x * 3 + 1];
            dst[x * 4 + 2] = row_buf[x * 3 + 2];
            dst[x * 4 + 3] = 255;
        }
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    free(row_buf);

    out->rgba = rgba;
    out->w = w;
    out->h = h;
    return 0;
}

void decoded_image_free(decoded_image_t* img) {
    if (!img) {
        return;
    }
    free(img->rgba);
    img->rgba = NULL;
    img->w = 0;
    img->h = 0;
}

// ----------------------------------------------------------------------------
// Bilinear resample with optional Y-flip.
// ----------------------------------------------------------------------------

uint8_t* jpeg_resample_rgba(const uint8_t* src, int src_w, int src_h, int dst_w, int dst_h,
                            bool flip_y) {
    if (!src || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        return NULL;
    }
    uint8_t* dst = malloc((size_t)dst_w * dst_h * 4);
    if (!dst) {
        return NULL;
    }
    // Map destination cell centres to source pixel centres. dx = (i+0.5) /
    // dst_w * src_w − 0.5 — the standard bilinear convention.
    double sx_step = (double)src_w / (double)dst_w;
    double sy_step = (double)src_h / (double)dst_h;
    for (int dy = 0; dy < dst_h; dy++) {
        // Optional Y-flip: dy=0 in the destination corresponds to dst_h-1 in
        // source-coords (so the bottom of the source becomes the top of the
        // destination). The LiDAR raster convention is row 0 = L93 ymin.
        int read_dy = flip_y ? (dst_h - 1 - dy) : dy;
        double sy = (read_dy + 0.5) * sy_step - 0.5;
        int sy0 = (int)floor(sy);
        int sy1 = sy0 + 1;
        double fy = sy - sy0;
        if (sy0 < 0) {
            sy0 = 0;
            fy = 0;
        }
        if (sy1 > src_h - 1) {
            sy1 = src_h - 1;
        }
        if (sy0 > src_h - 1) {
            sy0 = src_h - 1;
        }
        for (int dx = 0; dx < dst_w; dx++) {
            double sx = (dx + 0.5) * sx_step - 0.5;
            int sx0 = (int)floor(sx);
            int sx1 = sx0 + 1;
            double fx = sx - sx0;
            if (sx0 < 0) {
                sx0 = 0;
                fx = 0;
            }
            if (sx1 > src_w - 1) {
                sx1 = src_w - 1;
            }
            if (sx0 > src_w - 1) {
                sx0 = src_w - 1;
            }
            const uint8_t* p00 = src + (sy0 * src_w + sx0) * 4;
            const uint8_t* p01 = src + (sy0 * src_w + sx1) * 4;
            const uint8_t* p10 = src + (sy1 * src_w + sx0) * 4;
            const uint8_t* p11 = src + (sy1 * src_w + sx1) * 4;
            for (int c = 0; c < 4; c++) {
                double a = (1.0 - fx) * p00[c] + fx * p01[c];
                double b = (1.0 - fx) * p10[c] + fx * p11[c];
                double v = (1.0 - fy) * a + fy * b;
                if (v < 0) v = 0;
                if (v > 255) v = 255;
                dst[(dy * dst_w + dx) * 4 + c] = (uint8_t)(v + 0.5);
            }
        }
    }
    return dst;
}
