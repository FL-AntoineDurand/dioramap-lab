// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-mulberry32 — Mulberry32 PRNG, byte-identical to bench-v2's
// lib/geometry/primitives.js:createRng (Tommy Ettinger's 2017 design,
// 32-bit state, period 2^32, no setup).
//
// V2 RANSAC paths use it for repeatable random-triple sampling. To
// byte-validate the v3 port we replicate the exact bit semantics:
//   - 32-bit signed ALU (`| 0` in JS)
//   - `Math.imul(a, b)` = (int32_t)((int64_t)a * b)
//   - `>>> n` = (uint32_t) right shift
//   - final divide by 2^32 to land in [0, 1)
//
// Self-check: `seed=42` produces 0x99e1ef7c, 0x72c32b8a, 0xda3b32c0,
// 0xab73b0ad, 0x2cc09a8a, 0x86cec4d3, 0x45f24514, 0x9fef4401,
// 0xdd8fbf1e, 0x78e9c541, … — verified against V8 in bench-v2.

#ifndef V3_STEPS_V7_MULBERRY32_H
#define V3_STEPS_V7_MULBERRY32_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct v7_rng {
    int32_t state;       // matches JS `let s = seed | 0` (signed 32-bit)
} v7_rng_t;

static inline void v7_rng_init(v7_rng_t* rng, int32_t seed) {
    rng->state = seed;
}

// One step → returns the raw 32-bit u32 (uint cast). Used internally
// + by tests that want bit-exact comparison against V8.
static inline uint32_t v7_rng_next_u32(v7_rng_t* rng) {
    // s = (s + 0x6d2b79f5) | 0  —  signed-32 wrap on add
    int32_t s = (int32_t)((uint32_t)rng->state + 0x6d2b79f5u);
    rng->state = s;

    // t = Math.imul(s ^ (s >>> 15), 1 | s)
    //   `>>> 15` is JS unsigned shift; cast s to u32 first.
    //   Math.imul is 32-bit signed multiply; (uint32_t * int32_t) cast
    //   to int32_t replicates the truncation.
    const uint32_t s_u = (uint32_t)s;
    const int32_t a = (int32_t)(s_u ^ (s_u >> 15));
    const int32_t b = (int32_t)(1u | s_u);
    int32_t t = (int32_t)((uint32_t)a * (uint32_t)b);

    // t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t
    const uint32_t t_u = (uint32_t)t;
    const int32_t c = (int32_t)(t_u ^ (t_u >> 7));
    const int32_t d = (int32_t)(61u | t_u);
    const int32_t mul = (int32_t)((uint32_t)c * (uint32_t)d);
    t = (int32_t)(((uint32_t)t + (uint32_t)mul)) ^ t;

    // return ((t ^ (t >>> 14)) >>> 0) / 2^32
    const uint32_t out = ((uint32_t)t) ^ (((uint32_t)t) >> 14);
    return out;
}

// Float in [0, 1). Matches v2's `((t ^ ...) >>> 0) / 4294967296`.
static inline double v7_rng_next(v7_rng_t* rng) {
    return (double)v7_rng_next_u32(rng) / 4294967296.0;
}

#ifdef __cplusplus
}
#endif
#endif
