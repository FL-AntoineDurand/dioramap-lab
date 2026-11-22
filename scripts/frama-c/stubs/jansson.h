// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Frama-C audit stub for <jansson.h>.
//
// The real jansson header (libjansson-dev) forward-declares `json_t` as
// `struct json_t;` and exposes API via opaque pointers. Frama-C 25's
// Eva can't reason about the typed accessors (json_object_get,
// json_is_string, json_string_value, …) because the discriminator field
// is invisible. Result across the codebase: every `parse_params` /
// `clone_pipeline_with_overrides` / pipeline.c emits Unknown alarms on
// `\valid_read(&j->type)` chains.
//
// This stub declares `json_t` with the discriminator field at its real
// layout offset (matching jansson 2.x's `struct json_t { json_type type;
// size_t refcount; }`), then attaches ACSL contracts to the public
// accessors so Eva propagates the typed-discriminator through
// `if (json_is_string(j)) { ... json_string_value(j) ... }` chains.
//
// The stub is NOT a replacement for jansson at link time — production
// builds use the real header from libjansson-dev. The stub is only
// activated when DIORAMAP_FRAMA_C_AUDIT is set AND
// scripts/frama-c/stubs/ comes first in the include path (which the
// audit runner ensures via `-Iscripts/frama-c/stubs` before other -I
// directories).
#ifndef DIORAMAP_AUDIT_JANSSON_H
#define DIORAMAP_AUDIT_JANSSON_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>   // ACSL logic for valid_read_string

typedef enum {
    JSON_OBJECT = 0,
    JSON_ARRAY,
    JSON_STRING,
    JSON_INTEGER,
    JSON_REAL,
    JSON_TRUE,
    JSON_FALSE,
    JSON_NULL
} json_type;

typedef struct json_t {
    json_type type;
    size_t    refcount;
} json_t;

typedef long long json_int_t;

typedef struct {
    int   line;
    int   column;
    int   position;
    char  source[80];
    char  text[160];
} json_error_t;

// Load flags (subset that v3 uses).
#define JSON_COMPACT     (1 << 5)
#define JSON_SORT_KEYS   (1 << 6)
#define JSON_ENSURE_ASCII (1 << 7)
#define JSON_PRESERVE_ORDER (1 << 8)
#define JSON_REJECT_DUPLICATES (1 << 1)
#define JSON_DECODE_ANY  (1 << 3)
#define JSON_DISABLE_EOF_CHECK (1 << 2)
#define JSON_INDENT(n)   ((n) & 0x1F)
#define JSON_REAL_PRECISION(n) ((((n) & 0x1F) << 11))
#define JSON_ENCODE_ANY  (1 << 4)
#define JSON_ESCAPE_SLASH (1 << 10)
#define JSON_EMBED       (1 << 9)
#define JSON_ALLOW_NUL   (1 << 11)
#define JSON_DECODE_INT_AS_REAL (1 << 12)

// ── Type predicates (return 0 or 1; on success carry the discriminator) ──

/*@ requires j == \null || \valid_read(j);
    assigns \nothing;
    ensures \result == 0 || \result == 1;
    ensures \result == 1 ==> (j != \null && j->type == JSON_OBJECT);
*/
int json_is_object(const json_t* j);

/*@ requires j == \null || \valid_read(j);
    assigns \nothing;
    ensures \result == 0 || \result == 1;
    ensures \result == 1 ==> (j != \null && j->type == JSON_ARRAY);
*/
int json_is_array(const json_t* j);

/*@ requires j == \null || \valid_read(j);
    assigns \nothing;
    ensures \result == 0 || \result == 1;
    ensures \result == 1 ==> (j != \null && j->type == JSON_STRING);
*/
int json_is_string(const json_t* j);

/*@ requires j == \null || \valid_read(j);
    assigns \nothing;
    ensures \result == 0 || \result == 1;
    ensures \result == 1 ==> (j != \null && j->type == JSON_INTEGER);
*/
int json_is_integer(const json_t* j);

/*@ requires j == \null || \valid_read(j);
    assigns \nothing;
    ensures \result == 0 || \result == 1;
    ensures \result == 1 ==> (j != \null && j->type == JSON_REAL);
*/
int json_is_real(const json_t* j);

/*@ requires j == \null || \valid_read(j);
    assigns \nothing;
    ensures \result == 0 || \result == 1;
    ensures \result == 1 ==> (j != \null && (j->type == JSON_INTEGER || j->type == JSON_REAL));
*/
int json_is_number(const json_t* j);

/*@ requires j == \null || \valid_read(j);
    assigns \nothing;
    ensures \result == 0 || \result == 1;
    ensures \result == 1 ==> (j != \null && j->type == JSON_TRUE);
*/
int json_is_true(const json_t* j);

/*@ requires j == \null || \valid_read(j);
    assigns \nothing;
    ensures \result == 0 || \result == 1;
    ensures \result == 1 ==> (j != \null && j->type == JSON_FALSE);
*/
int json_is_false(const json_t* j);

/*@ requires j == \null || \valid_read(j);
    assigns \nothing;
    ensures \result == 0 || \result == 1;
    ensures \result == 1 ==> (j != \null &&
                              (j->type == JSON_TRUE || j->type == JSON_FALSE));
*/
int json_is_boolean(const json_t* j);

/*@ requires j == \null || \valid_read(j);
    assigns \nothing;
    ensures \result == 0 || \result == 1;
    ensures \result == 1 ==> (j != \null && j->type == JSON_NULL);
*/
int json_is_null(const json_t* j);

// ── Typed accessors (preconditioned on the matching type) ──

/*@ requires \valid_read(j);
    requires j->type == JSON_STRING;
    assigns \nothing;
    ensures \result == \null || valid_read_string(\result);
*/
const char* json_string_value(const json_t* j);

/*@ requires \valid_read(j);
    requires j->type == JSON_STRING;
    assigns \nothing;
*/
size_t json_string_length(const json_t* j);

/*@ requires \valid_read(j);
    requires j->type == JSON_INTEGER;
    assigns \nothing;
*/
json_int_t json_integer_value(const json_t* j);

/*@ requires \valid_read(j);
    requires j->type == JSON_REAL;
    assigns \nothing;
*/
double json_real_value(const json_t* j);

/*@ requires \valid_read(j);
    requires j->type == JSON_INTEGER || j->type == JSON_REAL;
    assigns \nothing;
*/
double json_number_value(const json_t* j);

/*@ requires \valid_read(j);
    requires j->type == JSON_TRUE || j->type == JSON_FALSE;
    assigns \nothing;
    ensures \result == 0 || \result == 1;
*/
int json_boolean_value(const json_t* j);

// ── Object accessors ──

/*@ requires obj == \null || \valid_read(obj);
    requires valid_read_string(key);
    assigns \nothing;
    ensures \result == \null || \valid_read(\result);
*/
json_t* json_object_get(const json_t* obj, const char* key);

/*@ requires \valid_read(obj);
    requires obj->type == JSON_OBJECT;
    assigns \nothing;
*/
size_t json_object_size(const json_t* obj);

// ── Array accessors ──

/*@ requires array == \null || \valid_read(array);
    assigns \nothing;
*/
size_t json_array_size(const json_t* array);

/*@ requires \valid_read(array);
    requires array->type == JSON_ARRAY;
    assigns \nothing;
    ensures \result == \null || \valid_read(\result);
*/
json_t* json_array_get(const json_t* array, size_t index);

// ── Constructors / refcount mgmt ──

json_t* json_object(void);
json_t* json_array(void);
json_t* json_string(const char* value);
json_t* json_stringn(const char* value, size_t len);
json_t* json_integer(json_int_t value);
json_t* json_real(double value);
json_t* json_true(void);
json_t* json_false(void);
json_t* json_null(void);
json_t* json_boolean(int v);

int json_object_set(json_t* obj, const char* key, json_t* value);
int json_object_set_new(json_t* obj, const char* key, json_t* value);
int json_object_del(json_t* obj, const char* key);
int json_object_clear(json_t* obj);

int json_array_append(json_t* array, json_t* value);
int json_array_append_new(json_t* array, json_t* value);
int json_array_insert(json_t* array, size_t index, json_t* value);
int json_array_insert_new(json_t* array, size_t index, json_t* value);
int json_array_set(json_t* array, size_t index, json_t* value);
int json_array_set_new(json_t* array, size_t index, json_t* value);
int json_array_remove(json_t* array, size_t index);
int json_array_clear(json_t* array);

/*@ requires j == \null || \valid(j);
    assigns *j;
*/
void json_decref(json_t* j);

/*@ requires j == \null || \valid(j);
    assigns *j;
*/
json_t* json_incref(json_t* j);

// ── Load / dump ──

/*@ requires valid_read_string(input);
    requires error == \null || \valid(error);
    assigns *error \from indirect:input;
    ensures \result == \null || \valid(\result);
*/
json_t* json_loads(const char* input, size_t flags, json_error_t* error);

/*@ requires buffer == \null || \valid_read(((char*)buffer) + (0 .. buflen - 1));
    requires error == \null || \valid(error);
    assigns *error \from indirect:buffer, indirect:buflen;
    ensures \result == \null || \valid(\result);
*/
json_t* json_loadb(const char* buffer, size_t buflen, size_t flags, json_error_t* error);

json_t* json_loadf(FILE* input, size_t flags, json_error_t* error);
json_t* json_load_file(const char* path, size_t flags, json_error_t* error);

/*@ requires j == \null || \valid_read(j);
    assigns \nothing;
    ensures \result == \null || valid_read_string(\result);
*/
char* json_dumps(const json_t* j, size_t flags);

int json_dumpf(const json_t* j, FILE* output, size_t flags);
int json_dump_file(const json_t* j, const char* path, size_t flags);

// ── Object iteration (used by v7 / pipeline.c) ──

void* json_object_iter(json_t* object);
void* json_object_iter_at(json_t* object, const char* key);
void* json_object_iter_next(json_t* object, void* iter);

// ACSL can't take \valid on a void* opaque type, so these iter accessors
// stay unconstrained. Callers always null-check the iter first.
const char* json_object_iter_key(void* iter);
json_t* json_object_iter_value(void* iter);

#define json_object_foreach(object, key, value) \
    for (void* _iter = json_object_iter(object); \
         (key = (const char*)(_iter ? json_object_iter_key(_iter) : NULL)) \
         && (value = (_iter ? json_object_iter_value(_iter) : NULL)); \
         _iter = json_object_iter_next(object, _iter))

#define json_array_foreach(array, index, value) \
    for (index = 0; index < json_array_size(array) \
         && (value = json_array_get(array, index)); \
         index++)

#endif // DIORAMAP_AUDIT_JANSSON_H
