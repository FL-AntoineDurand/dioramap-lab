// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-trace.h — compile-time-gated stderr tracing for v7 steps.
//
// Trace calls stay in the source permanently but cost NOTHING in a normal
// build: when V7_TRACE is undefined every macro expands to `((void)0)` —
// no branch, no fprintf, no format string in the binary.
//
// Enable for a debug build:
//     cmake -DDIORAMAP_V7_TRACE=ON -B build-trace …
//   (CMake adds `target_compile_definitions(dioramap_steps_v7 PRIVATE
//    V7_TRACE=1)` — see core/CMakeLists.txt.)
//
// Usage:
//     #include "v7-trace.h"
//     V7_TRACEF("SEED", "seg=%u n_pts=%u", id, n);          // unconditional
//     V7_TRACE_IF(seg_id == target, "SEED", "killed: %s", why);  // filtered
//
// The runtime `if` in V7_TRACE_IF exists ONLY in trace builds — release
// binaries never see it. For per-feature filtering, read the target from
// the environment INSIDE a `#ifdef V7_TRACE` guard so the lookup is also
// compiled out by default (see v7-normal-cascade-seed-impl.c for the
// pattern).
//
// Not BR_PROFILE: this header is a COMPILE-time gate (V7_TRACE on/off
// at build). BR_PROFILE in v7-boundary-regularize-impl.c is a separate
// RUNTIME env-var gate around per-phase wall-time counters that always
// compile in. Different mechanisms, different scopes — don't conflate
// them. The clock_gettime libc-stub FP that BR_PROFILE tripped is
// documented as entry #12 in docs/quality/FRAMA-C-FALSE-POSITIVES.md.
//
// Frama-C audit interaction: V7_TRACE is undefined in audit builds (the
// audit doesn't pass -DV7_TRACE) so every V7_TRACEF expands to the
// unevaluated-sizeof no-op below — Eva sees nothing. No explicit
// `#ifndef DIORAMAP_FRAMA_C_AUDIT` gate is needed.
#pragma once

#include <stdio.h>

#ifdef V7_TRACE
  #define V7_TRACE_ENABLED 1
  #define V7_TRACEF(tag, fmt, ...) \
      fprintf(stderr, "[" tag "] " fmt "\n", ##__VA_ARGS__)
  #define V7_TRACE_IF(cond, tag, fmt, ...) \
      do { if (cond) V7_TRACEF(tag, fmt, ##__VA_ARGS__); } while (0)
#else
  #define V7_TRACE_ENABLED 0
  // No-op, but the args are still NAMED inside an unevaluated sizeof: the
  // operand is never computed (zero runtime cost, no call emitted, no `if`),
  // yet the compiler (a) counts the referenced variables as "used" — so
  // trace-only locals don't trip -Wunused — and (b) still type-checks the
  // printf format in normal builds. `cond` is likewise referenced but
  // unevaluated.
  #define V7_TRACEF(tag, fmt, ...) \
      ((void)sizeof(printf(fmt, ##__VA_ARGS__)))
  #define V7_TRACE_IF(cond, tag, fmt, ...) \
      ((void)sizeof((cond) ? printf(fmt, ##__VA_ARGS__) : 0))
#endif
