// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#ifndef V3_PIPELINE_H
#define V3_PIPELINE_H

#include <stddef.h>
#include <string.h>  // ACSL valid_read_string

#include "v3/step.h"

#ifdef __cplusplus
extern "C" {
#endif

// A pipeline is a *set* of steps (order-agnostic). The runner computes the
// execution order from the steps' declared reads/writes.
typedef struct pipeline pipeline_t;

// Pipeline-declared data-source credit, surfaced via CATALOG_RESPONSE so the
// SDK can render an attribution overlay that updates with the pipeline.
// `label` is required (short, user-facing token e.g. "IGN"); `url` is
// optional (NULL when the source has no public homepage).
typedef struct pipeline_attribution {
    const char* label;
    const char* url; // may be NULL
} pipeline_attribution_t;

// ----------------------------------------------------------------------------
// Primary API: load from a JSON document.
//
// Expected schema (minimum):
//   {
//     "name": "my-pipeline",
//     "steps": [
//       { "step": "step-name" },
//       { "step": "other-step", "params": { ... } }
//     ]
//   }
//
// `params` (optional, any JSON value) is re-serialized to a JSON string and
// passed to the step at runtime via step_get_params().
//
// On failure returns NULL; if err_buf is provided, a diagnostic message is
// written into it (NUL-terminated, truncated to err_buf_sz).
// ----------------------------------------------------------------------------

/*@ requires json == \null || \valid_read(json + (0 .. len - 1));
    requires err_buf == \null || \valid(err_buf + (0 .. err_buf_sz - 1));
    assigns *(err_buf + (0 .. err_buf_sz - 1));
    ensures \result == \null || \valid(\result);
*/
pipeline_t* pipeline_load_json_string(const char* json, size_t len, char* err_buf,
                                      size_t err_buf_sz);

/*@ requires path == \null || valid_read_string(path);
    requires err_buf == \null || \valid(err_buf + (0 .. err_buf_sz - 1));
    assigns *(err_buf + (0 .. err_buf_sz - 1));
    ensures \result == \null || \valid(\result);
*/
pipeline_t* pipeline_load_json_file(const char* path, char* err_buf, size_t err_buf_sz);

// ----------------------------------------------------------------------------
// Programmatic builder API (used by the JSON loader internally; also handy
// for tests and hardcoded pipelines).
// ----------------------------------------------------------------------------

/*@ requires name == \null || valid_read_string(name);
    assigns \nothing;
    ensures \result == \null || \valid(\result);
*/
pipeline_t* pipeline_new(const char* name);

/*@ requires p == \null || \valid(p);
    assigns *p;
*/
void pipeline_free(pipeline_t* p);

// Add a step. step_name must be registered. params_json may be NULL.
// params_json is copied; caller retains ownership of its pointer.
/*@ requires p == \null || \valid(p);
    requires step_name == \null || valid_read_string(step_name);
    requires params_json == \null || valid_read_string(params_json);
    assigns *p;
    ensures \result == 0 || \result == -1;
*/
int pipeline_add_step(pipeline_t* p, const char* step_name, const char* params_json);

/*@ requires p == \null || \valid_read(p);
    assigns \nothing;
    ensures \result == 0 || \result == -1;
*/
int pipeline_validate(const pipeline_t* p);

/*@ requires p == \null || \valid_read(p);
    assigns \nothing;
    ensures \result == \null || valid_read_string(\result);
*/
const char* pipeline_name(const pipeline_t* p);

/*@ requires p == \null || \valid_read(p);
    assigns \nothing;
*/
size_t pipeline_step_count(const pipeline_t* p);

/*@ requires p == \null || \valid_read(p);
    assigns \nothing;
    ensures \result == \null || \valid_read(\result);
*/
const step_def_t* pipeline_step_def_at(const pipeline_t* p, size_t i);

/*@ requires p == \null || \valid_read(p);
    assigns \nothing;
    ensures \result == \null || valid_read_string(\result);
*/
const char* pipeline_step_params_at(const pipeline_t* p, size_t i);

/*@ requires p == \null || \valid_read(p);
    assigns \nothing;
*/
size_t                        pipeline_attribution_count(const pipeline_t* p);

/*@ requires p == \null || \valid_read(p);
    assigns \nothing;
    ensures \result == \null || \valid_read(\result);
*/
const pipeline_attribution_t* pipeline_attribution_at(const pipeline_t* p, size_t i);

/*@ requires p == \null || \valid(p);
    requires label == \null || valid_read_string(label);
    requires url == \null || valid_read_string(url);
    assigns *p;
    ensures \result == 0 || \result == -1;
*/
int pipeline_add_attribution(pipeline_t* p, const char* label, const char* url);

#ifdef __cplusplus
}
#endif
#endif
