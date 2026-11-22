// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#define _POSIX_C_SOURCE 200809L

#include "v3/pipeline.h"

#include <jansson.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v3/artifact.h"

typedef struct pipeline_step_entry {
    const step_def_t* def;
    char* params_json; // owned; NULL if none
} pipeline_step_entry_t;

typedef struct pipeline_attr_entry {
    char* label; // owned; non-NULL
    char* url;   // owned; NULL if absent
    pipeline_attribution_t view; // points into label/url; rebuilt on realloc
} pipeline_attr_entry_t;

struct pipeline {
    char* name;
    pipeline_step_entry_t* steps;
    size_t n_steps;
    size_t cap_steps;
    pipeline_attr_entry_t* attrs;
    size_t n_attrs;
    size_t cap_attrs;
};

pipeline_t* pipeline_new(const char* name) {
    if (!name) {
        return NULL;
    }
    pipeline_t* p = calloc(1, sizeof(*p));
    if (!p) {
        return NULL;
    }
    p->name = strdup(name);
    if (!p->name) {
        free(p);
        return NULL;
    }
    return p;
}

void pipeline_free(pipeline_t* p) {
    if (!p) {
        return;
    }
    for (size_t i = 0; i < p->n_steps; i++) {
        free(p->steps[i].params_json);
    }
    free(p->steps);
    for (size_t i = 0; i < p->n_attrs; i++) {
        free(p->attrs[i].label);
        free(p->attrs[i].url);
    }
    free(p->attrs);
    free(p->name);
    free(p);
}

int pipeline_add_step(pipeline_t* p, const char* step_name, const char* params_json) {
    if (!p || !step_name) {
        return -1;
    }
    const step_def_t* def = step_lookup(step_name);
    if (!def) {
        return -1;
    }
    // No duplicate step insertions.
    for (size_t i = 0; i < p->n_steps; i++) {
        if (p->steps[i].def == def) {
            return -1;
        }
    }
    if (p->n_steps == p->cap_steps) {
        size_t new_cap = p->cap_steps ? p->cap_steps * 2 : 4;
        pipeline_step_entry_t* arr = realloc(p->steps, new_cap * sizeof(*arr));
        if (!arr) {
            return -1;
        }
        p->steps = arr;
        p->cap_steps = new_cap;
    }
    p->steps[p->n_steps].def = def;
    p->steps[p->n_steps].params_json = NULL;
    if (params_json) {
        p->steps[p->n_steps].params_json = strdup(params_json);
        if (!p->steps[p->n_steps].params_json) {
            return -1;
        }
    }
    p->n_steps++;
    return 0;
}

int pipeline_validate(const pipeline_t* p) {
    if (!p) {
        return -1;
    }
    // Defensive: pipeline_add_step rejects unknown step names (def==NULL
    // from step_lookup), so every entry of p->steps[].def is non-NULL by
    // construction. Eva can't carry that invariant across loop bodies
    // (a front-loaded scan-and-check loop doesn't propagate — observed
    // sure alarm at the di->n_writes deref in the SECOND loop even after
    // pre-validating all entries). So we re-check inside each loop where
    // it's used — also doubles as runtime defense against corruption.
    // No step writes the reserved name.
    for (size_t i = 0; i < p->n_steps; i++) {
        const step_def_t* di = p->steps[i].def;
        if (!di) return -1;
        for (size_t w = 0; w < di->n_writes; w++) {
            if (strcmp(di->writes[w].name, PIPELINE_INPUT_ARTIFACT) == 0) {
                return -1;
            }
        }
    }
    // No duplicate writes across steps (O(n²) — pipelines are small).
    for (size_t i = 0; i < p->n_steps; i++) {
        const step_def_t* di = p->steps[i].def;
        if (!di) return -1;
        for (size_t wi = 0; wi < di->n_writes; wi++) {
            for (size_t j = i + 1; j < p->n_steps; j++) {
                const step_def_t* dj = p->steps[j].def;
                if (!dj) return -1;
                for (size_t wj = 0; wj < dj->n_writes; wj++) {
                    if (strcmp(di->writes[wi].name, dj->writes[wj].name) == 0) {
                        return -1;
                    }
                }
            }
        }
    }
    // Every read (other than pipeline-input) has a producer in the pipeline,
    // and if the reader declared `array: true`, the producer's write must be
    // an ARRAY_INLINE or ARRAY_BY_HASH write with `array: true` too.
    for (size_t i = 0; i < p->n_steps; i++) {
        const step_def_t* di = p->steps[i].def;
        if (!di) return -1;
        for (size_t r = 0; r < di->n_reads; r++) {
            if (strcmp(di->reads[r].name, PIPELINE_INPUT_ARTIFACT) == 0) {
                continue;
            }
            bool found = false;
            for (size_t j = 0; j < p->n_steps && !found; j++) {
                if (i == j) {
                    continue; // a step cannot read its own writes
                }
                const step_def_t* dj = p->steps[j].def;
                if (!dj) return -1;
                for (size_t w = 0; w < dj->n_writes && !found; w++) {
                    if (strcmp(di->reads[r].name, dj->writes[w].name) != 0) {
                        continue;
                    }
                    found = true;
                    // Array-ness must match on both sides.
                    bool writer_is_array = dj->writes[w].array;
                    bool reader_is_array = di->reads[r].array;
                    if (writer_is_array != reader_is_array) {
                        return -1;
                    }
                    // If writer declares array, kind must be ARRAY_*.
                    if (writer_is_array) {
                        if (dj->writes[w].kind != AK_ARRAY_INLINE &&
                            dj->writes[w].kind != AK_ARRAY_BY_HASH) {
                            return -1;
                        }
                    }
                }
            }
            if (!found) {
                return -1;
            }
        }
    }
    // Any write declared `array: true` must have a matching ARRAY_* kind.
    for (size_t i = 0; i < p->n_steps; i++) {
        const step_def_t* di = p->steps[i].def;
        for (size_t w = 0; w < di->n_writes; w++) {
            if (di->writes[w].array &&
                di->writes[w].kind != AK_ARRAY_INLINE &&
                di->writes[w].kind != AK_ARRAY_BY_HASH) {
                return -1;
            }
            if ((di->writes[w].kind == AK_ARRAY_INLINE ||
                 di->writes[w].kind == AK_ARRAY_BY_HASH) &&
                !di->writes[w].array) {
                return -1;
            }
        }
    }
    return 0;
}

const char* pipeline_name(const pipeline_t* p) {
    return p ? p->name : NULL;
}

size_t pipeline_step_count(const pipeline_t* p) {
    return p ? p->n_steps : 0;
}

const step_def_t* pipeline_step_def_at(const pipeline_t* p, size_t i) {
    if (!p || i >= p->n_steps) {
        return NULL;
    }
    return p->steps[i].def;
}

const char* pipeline_step_params_at(const pipeline_t* p, size_t i) {
    if (!p || i >= p->n_steps) {
        return NULL;
    }
    return p->steps[i].params_json;
}

int pipeline_add_attribution(pipeline_t* p, const char* label, const char* url) {
    if (!p || !label || !*label) {
        return -1;
    }
    if (p->n_attrs == p->cap_attrs) {
        size_t new_cap = p->cap_attrs ? p->cap_attrs * 2 : 4;
        pipeline_attr_entry_t* arr = realloc(p->attrs, new_cap * sizeof(*arr));
        if (!arr) {
            return -1;
        }
        p->attrs = arr;
        p->cap_attrs = new_cap;
        // realloc may have moved the storage; rebuild the view pointers so
        // earlier callers' pipeline_attribution_at() results stay valid for
        // the next call. We don't promise they stay valid across mutations.
        for (size_t i = 0; i < p->n_attrs; i++) {
            p->attrs[i].view.label = p->attrs[i].label;
            p->attrs[i].view.url = p->attrs[i].url;
        }
    }
    pipeline_attr_entry_t* slot = &p->attrs[p->n_attrs];
    slot->label = strdup(label);
    if (!slot->label) {
        return -1;
    }
    if (url) {
        slot->url = strdup(url);
        if (!slot->url) {
            free(slot->label);
            slot->label = NULL;
            return -1;
        }
    } else {
        slot->url = NULL;
    }
    slot->view.label = slot->label;
    slot->view.url = slot->url;
    p->n_attrs++;
    return 0;
}

size_t pipeline_attribution_count(const pipeline_t* p) {
    return p ? p->n_attrs : 0;
}

const pipeline_attribution_t* pipeline_attribution_at(const pipeline_t* p, size_t i) {
    if (!p || i >= p->n_attrs) {
        return NULL;
    }
    return &p->attrs[i].view;
}

// ----------------------------------------------------------------------------
// JSON loader
// ----------------------------------------------------------------------------

__attribute__((format(printf, 3, 4))) static void set_err(char* err_buf, size_t err_buf_sz,
                                                          const char* fmt, ...) {
    if (!err_buf || err_buf_sz == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err_buf, err_buf_sz, fmt, ap);
    va_end(ap);
}

pipeline_t* pipeline_load_json_string(const char* json, size_t len, char* err_buf,
                                      size_t err_buf_sz) {
    if (!json) {
        set_err(err_buf, err_buf_sz, "null json input");
        return NULL;
    }
    json_error_t je;
    json_t* root = json_loadb(json, len, 0, &je);
    if (!root) {
        set_err(err_buf, err_buf_sz, "json parse error at line %d col %d: %s", je.line, je.column,
                je.text);
        return NULL;
    }

    pipeline_t* p = NULL;
    if (!json_is_object(root)) {
        set_err(err_buf, err_buf_sz, "top-level value must be an object");
        goto done;
    }

    json_t* j_name = json_object_get(root, "name");
    if (!j_name || !json_is_string(j_name)) {
        set_err(err_buf, err_buf_sz, "'name' must be a string");
        goto done;
    }
    json_t* j_steps = json_object_get(root, "steps");
    if (!j_steps || !json_is_array(j_steps)) {
        set_err(err_buf, err_buf_sz, "'steps' must be an array");
        goto done;
    }

    p = pipeline_new(json_string_value(j_name));
    if (!p) {
        set_err(err_buf, err_buf_sz, "failed to allocate pipeline");
        goto done;
    }

    size_t i;
    json_t* j_step;
    json_array_foreach(j_steps, i, j_step) {
        if (!json_is_object(j_step)) {
            set_err(err_buf, err_buf_sz, "steps[%zu] must be an object", i);
            pipeline_free(p);
            p = NULL;
            goto done;
        }
        json_t* j_step_name = json_object_get(j_step, "step");
        if (!j_step_name || !json_is_string(j_step_name)) {
            set_err(err_buf, err_buf_sz, "steps[%zu].step must be a string", i);
            pipeline_free(p);
            p = NULL;
            goto done;
        }
        const char* step_name = json_string_value(j_step_name);

        // Optional params: re-serialize to JSON string.
        char* params_str = NULL;
        json_t* j_params = json_object_get(j_step, "params");
        if (j_params) {
            params_str = json_dumps(j_params, JSON_COMPACT | JSON_SORT_KEYS);
            if (!params_str) {
                set_err(err_buf, err_buf_sz, "steps[%zu]: failed to serialize params", i);
                pipeline_free(p);
                p = NULL;
                goto done;
            }
        }

        int rc = pipeline_add_step(p, step_name, params_str);
        free(params_str);
        if (rc != 0) {
            set_err(err_buf, err_buf_sz, "steps[%zu].step '%s' not registered or duplicate", i,
                    step_name);
            pipeline_free(p);
            p = NULL;
            goto done;
        }
    }

    // Optional `attributions` array. Each element is {label: string, url: string|null}.
    // Absent / empty array → no credit; SDK falls back to "Powered by dioramap".
    json_t* j_attrs = json_object_get(root, "attributions");
    if (j_attrs) {
        if (!json_is_array(j_attrs)) {
            set_err(err_buf, err_buf_sz, "'attributions' must be an array");
            pipeline_free(p);
            p = NULL;
            goto done;
        }
        size_t k;
        json_t* j_attr;
        json_array_foreach(j_attrs, k, j_attr) {
            if (!json_is_object(j_attr)) {
                set_err(err_buf, err_buf_sz, "attributions[%zu] must be an object", k);
                pipeline_free(p);
                p = NULL;
                goto done;
            }
            json_t* j_label = json_object_get(j_attr, "label");
            if (!j_label || !json_is_string(j_label) || json_string_length(j_label) == 0) {
                set_err(err_buf, err_buf_sz,
                        "attributions[%zu].label must be a non-empty string", k);
                pipeline_free(p);
                p = NULL;
                goto done;
            }
            json_t* j_url = json_object_get(j_attr, "url");
            const char* url = NULL;
            if (j_url && !json_is_null(j_url)) {
                if (!json_is_string(j_url)) {
                    set_err(err_buf, err_buf_sz,
                            "attributions[%zu].url must be a string or null", k);
                    pipeline_free(p);
                    p = NULL;
                    goto done;
                }
                url = json_string_value(j_url);
            }
            if (pipeline_add_attribution(p, json_string_value(j_label), url) != 0) {
                set_err(err_buf, err_buf_sz, "attributions[%zu]: failed to add", k);
                pipeline_free(p);
                p = NULL;
                goto done;
            }
        }
    }

done:
    json_decref(root);
    return p;
}

pipeline_t* pipeline_load_json_file(const char* path, char* err_buf, size_t err_buf_sz) {
    if (!path) {
        set_err(err_buf, err_buf_sz, "null path");
        return NULL;
    }
    FILE* f = fopen(path, "rb");
    if (!f) {
        set_err(err_buf, err_buf_sz, "cannot open '%s'", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) {
        fclose(f);
        set_err(err_buf, err_buf_sz, "ftell failed on '%s'", path);
        return NULL;
    }
    char* buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        set_err(err_buf, err_buf_sz, "oom");
        return NULL;
    }
    size_t nr = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (nr != (size_t)sz) {
        free(buf);
        set_err(err_buf, err_buf_sz, "short read on '%s'", path);
        return NULL;
    }
    buf[sz] = '\0';
    pipeline_t* p = pipeline_load_json_string(buf, (size_t)sz, err_buf, err_buf_sz);
    free(buf);
    return p;
}
