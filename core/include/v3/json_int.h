// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Fail-loud scalar-from-JSON helpers used by every step's parse_params():
// json_int_or_default / json_int32_or_default (integers) and
// json_number_or_default (doubles — see the bottom of this header).
// Introduced as part of the Frama-C audit pass 3 hardening — see
// docs/quality/README.md "(int)json_integer_value" pattern note.
//
// Pre-audit, step params parsers used the idiom:
//
//   if ((j = json_object_get(obj, "k")) && json_is_integer(j))
//       p->k = (int)json_integer_value(j);
//
// which has two defects:
//   1. (int)json_integer_value silently truncates values outside int range
//      (json_int_t is long long). Attacker-supplied {"k": 9999999999}
//      triggered UB.
//   2. Wrong type ({"k": "five"}) was silently ignored; the user got
//      the default value with no log telling them their config was bad.
//
// Migration replaces every such site with:
//
//   j = json_object_get(obj, "k");
//   if (json_int_or_default(j, K_MIN, K_MAX, "k", &p->k) != 0) return -1;
//
// Semantics:
//   - j == NULL (field absent in JSON):    success, *out untouched (default).
//   - j non-integer:                       failure, stderr message, return -1.
//   - j integer but out of [min, max]:     failure, stderr message, return -1.
//   - j integer in range:                  success, *out = (int)value.
//
// Failure mode propagates: parse_params returns -1, step exec returns -1,
// runner reports STEP_FAILED to the operator with the offending param visible
// in the stderr stream and structured log.
#ifndef V3_JSON_INT_H
#define V3_JSON_INT_H

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <jansson.h>

#ifdef __cplusplus
extern "C" {
#endif

// Strict, range-checked int extractor.
// - j == NULL: success, *out untouched (use default).
// - j is integer in [min, max]: success, *out = (int)value.
// - j non-integer OR out of range: failure, log to stderr, return -1.
//
// `field_name` is the JSON object key for diagnostics; it must be a
// stable string (a string literal in practice).
static inline int json_int_or_default(const json_t* j, int min, int max,
                                       const char* field_name, int* out) {
    if (!j) return 0;
    if (!json_is_integer(j)) {
        fprintf(stderr, "step param '%s': not an integer\n",
                field_name ? field_name : "?");
        return -1;
    }
    json_int_t v = json_integer_value(j);
    if (v < (json_int_t)min || v > (json_int_t)max) {
        fprintf(stderr, "step param '%s': value %lld out of range [%d, %d]\n",
                field_name ? field_name : "?", (long long)v, min, max);
        return -1;
    }
    *out = (int)v;
    return 0;
}

// Same shape for fields stored as int32_t. C says int and int32_t are
// the same type on every platform the audit covers (x86_64 / arm64),
// but the explicit-width form avoids any -Wsign-conversion noise in
// strict-warning callers.
static inline int json_int32_or_default(const json_t* j, int32_t min, int32_t max,
                                         const char* field_name, int32_t* out) {
    int tmp;
    if (!j) return 0;
    if (json_int_or_default(j, (int)min, (int)max, field_name, &tmp) != 0)
        return -1;
    *out = (int32_t)tmp;
    return 0;
}

// Strict, range-checked floating-point extractor — the json_int_or_default
// twin for fields stored as double (tilt_deg, snap_radius,
// coplanar_balance_min, …). Accepts BOTH JSON integers and reals: jansson
// parses `90` as an integer and `90.0` as a real, and json_is_real /
// json_real_value return false / 0.0 on the integer form — the exact
// silent-zero defect the equivalence sweep hit on {"tilt": 90}. Using
// json_is_number / json_number_value bridges both encodings.
//
//   - j == NULL: success, *out untouched (use default).
//   - j is a finite number in [min, max]: success, *out = value.
//   - j non-number, non-finite (NaN/Inf), OR out of range: log, return -1.
static inline int json_number_or_default(const json_t* j, double min, double max,
                                         const char* field_name, double* out) {
    if (!j) return 0;
    if (!json_is_number(j)) {
        fprintf(stderr, "step param '%s': not a number\n",
                field_name ? field_name : "?");
        return -1;
    }
    double v = json_number_value(j);
    if (!isfinite(v)) {
        fprintf(stderr, "step param '%s': value is not finite\n",
                field_name ? field_name : "?");
        return -1;
    }
    if (v < min || v > max) {
        fprintf(stderr, "step param '%s': value %g out of range [%g, %g]\n",
                field_name ? field_name : "?", v, min, max);
        return -1;
    }
    *out = v;
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif // V3_JSON_INT_H
