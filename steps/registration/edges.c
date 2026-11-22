// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#include "edges.h"
#include "v3/safe_alloc.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

float* edges_fill_nodata(const float* src, size_t cells, float fill_val) {
    if (!src) {
        return NULL;
    }
    float* out = safe_malloc_array(cells, sizeof(float));
    if (!out) {
        return NULL;
    }
    float fill = fill_val;
    // NaN input means "use global min of valid cells".
    if (isnan(fill_val)) {
        float m = INFINITY;
        for (size_t i = 0; i < cells; i++) {
            float v = src[i];
            if (!isnan(v) && v < m) {
                m = v;
            }
        }
        fill = isfinite(m) ? m : 0.0f;
    }
    for (size_t i = 0; i < cells; i++) {
        float v = src[i];
        out[i] = isnan(v) ? fill : v;
    }
    return out;
}

void edges_sobel_magnitude(const float* img, uint32_t cols, uint32_t rows, float* out) {
    if (cols < 3 || rows < 3) {
        memset(out, 0, (size_t)cols * rows * sizeof(float));
        return;
    }
    // Zero the border ring first so untouched cells stay 0.
    memset(out, 0, (size_t)cols * rows * sizeof(float));
    for (uint32_t r = 1; r < rows - 1; r++) {
        for (uint32_t c = 1; c < cols - 1; c++) {
            float a = img[(size_t)(r - 1) * cols + (c - 1)];
            float b = img[(size_t)(r - 1) * cols + c];
            float cc = img[(size_t)(r - 1) * cols + (c + 1)];
            float d = img[(size_t)r * cols + (c - 1)];
            float f = img[(size_t)r * cols + (c + 1)];
            float g = img[(size_t)(r + 1) * cols + (c - 1)];
            float h2 = img[(size_t)(r + 1) * cols + c];
            float i = img[(size_t)(r + 1) * cols + (c + 1)];
            float gx = -a + cc - 2.0f * d + 2.0f * f - g + i;
            float gy = -a - 2.0f * b - cc + g + 2.0f * h2 + i;
            out[(size_t)r * cols + c] = sqrtf(gx * gx + gy * gy);
        }
    }
}

void edges_adaptive_normalize(float* arr, size_t n) {
    if (n == 0) {
        return;
    }
    double s = 0;
    for (size_t i = 0; i < n; i++) {
        s += arr[i];
    }
    double mean = s / n;
    double s2 = 0;
    for (size_t i = 0; i < n; i++) {
        double d = arr[i] - mean;
        s2 += d * d;
    }
    double std = sqrt(s2 / n);
    if (std == 0.0) {
        std = 1.0;
    }
    double ceil_v = mean + 4.0 * std;
    double denom = ceil_v - mean;
    if (denom < 1e-9) {
        denom = 1e-9;
    }
    double inv = 1.0 / denom;
    for (size_t i = 0; i < n; i++) {
        double v = (arr[i] - mean) * inv;
        if (v < 0) {
            v = 0;
        } else if (v > 1) {
            v = 1;
        }
        arr[i] = (float)v;
    }
}

void edges_mask_border_only(float* edges, uint32_t cols, uint32_t rows, int margin) {
    if (margin <= 0) {
        return;
    }
    int m = margin;
    for (uint32_t r = 0; r < rows; r++) {
        int rr = (int)r;
        if (rr < m || rr >= (int)rows - m) {
            for (uint32_t c = 0; c < cols; c++) {
                edges[(size_t)r * cols + c] = 0;
            }
        } else {
            for (int c = 0; c < m; c++) {
                edges[(size_t)r * cols + c] = 0;
            }
            for (int c = (int)cols - m; c < (int)cols; c++) {
                if (c < 0) continue;
                edges[(size_t)r * cols + c] = 0;
            }
        }
    }
}

void edges_mask_boundaries(float* edges, const float* source, uint32_t cols, uint32_t rows,
                           int margin) {
    // First, border-zero. Same as edges_mask_border_only.
    edges_mask_border_only(edges, cols, rows, margin);
    // Then NaN-adjacency zero: any cell whose 3×3 neighbourhood in source
    // contains a NaN gets zeroed.
    for (uint32_t r = 1; r < rows - 1; r++) {
        for (uint32_t c = 1; c < cols - 1; c++) {
            size_t k = (size_t)r * cols + c;
            if (edges[k] == 0) {
                continue;
            }
            int touches_nan = 0;
            for (int dr = -1; dr <= 1 && !touches_nan; dr++) {
                for (int dc = -1; dc <= 1; dc++) {
                    size_t kk = (size_t)(r + dr) * cols + (c + dc);
                    if (isnan(source[kk])) {
                        touches_nan = 1;
                        break;
                    }
                }
            }
            if (touches_nan) {
                edges[k] = 0;
            }
        }
    }
}
