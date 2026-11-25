// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Dioramap processor entry point.
//
// Phase A8: --version / --help stay quiet (no logs; clean stdout). The
// `batch` subcommand loads a pipeline from JSON, runs it against a store,
// and writes subscribed artifacts + a manifest.json into an output directory.

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>

#include <dirent.h>
#include <signal.h>

#include "v3/batch_ctx.h"
#include "v3/extension.h"
#include "v3/hash.h"
#include <jansson.h>

#include "v3/log.h"
#include "v3/pipeline.h"
#include "v3/pipeline_embedded.h"
#include "v3/run_journal.h"
#include "v3/runner.h"
#include "v3/server.h"
#include "v3/store.h"

// ASan defaults when the build is ASan-instrumented:
//   verify_asan_link_order=0 — lets the binary run when invoked from a
//     non-ASan shell that hasn't preloaded libasan (the LD_PRELOAD trick
//     isn't needed). Important for ctest, which doesn't preload ASan but
//     forks our binary.
//   detect_leaks=0 — leak detection runs at process exit; for short-lived
//     tool invocations the false-positive cost outweighs the value. Tests
//     and the bench harnesses verify leak-freedom under explicit ASan
//     control where it matters.
// Weak/optional: defined as a __weak symbol so non-ASan builds compile
// (the symbol is just unused). When ASan IS linked in, it resolves and
// is consulted at startup.
const char* __asan_default_options(void);
const char* __asan_default_options(void) {
    return "verify_asan_link_order=0:detect_leaks=0";
}

#include "buildings-steps.h"
#include "coord-steps.h"
#include "copc-steps.h"
#if DIORAMAP_V7
#include "v7-steps.h"
#endif
#include "demo.h"
#include "fetchers.h"
#include "gpu-smoke.h"
#include "classification-steps.h"
#include "ground-steps.h"
#include "normals-steps.h"
#include "ortho-steps.h"
#include "registration-steps.h"
#include "shadow-steps.h"

#ifndef DIORAMAP_VERSION
#define DIORAMAP_VERSION "0.0.0-dev"
#endif

// ----------------------------------------------------------------------------
// Utility: mkdir -p
// ----------------------------------------------------------------------------

// Phase B integrity audit context — populated by audit_cb walking the
// intent map. See audit_intent_vs_store call site for usage.
typedef struct integrity_audit {
    store_t* store;
    uint64_t total;
    uint64_t missing_content;
} integrity_audit_t;

static void audit_intent_cb(const hash_t* identity, const hash_t* content_hash, void* vctx) {
    (void)identity;
    integrity_audit_t* a = vctx;
    a->total++;
    if (!store_contains(a->store, content_hash)) {
        a->missing_content++;
    }
}

static int mkdir_p(const char* path) {
    char buf[4096];
    size_t n = strlen(path);
    if (n >= sizeof(buf)) {
        return -1;
    }
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i <= n; i++) {
        if (buf[i] == '/' || buf[i] == '\0') {
            char c = buf[i];
            buf[i] = '\0';
            if (mkdir(buf, 0700) != 0 && errno != EEXIST) {
                return -1;
            }
            buf[i] = c;
        }
    }
    return 0;
}

// ----------------------------------------------------------------------------
// Batch-mode "--produce" list parsing + subscribe tracking
// ----------------------------------------------------------------------------

typedef struct produce_list {
    char** names;
    size_t count;
    char* scratch;
} produce_list_t;

static int produce_list_parse(produce_list_t* pl, const char* csv) {
    pl->scratch = strdup(csv);
    if (!pl->scratch)
        return -1;
    size_t count = 1;
    for (char* p = pl->scratch; *p; p++) {
        if (*p == ',')
            count++;
    }
    pl->names = calloc(count, sizeof(char*));
    if (!pl->names) {
        free(pl->scratch);
        return -1;
    }
    pl->count = 0;
    char* tok = strtok(pl->scratch, ",");
    while (tok) {
        while (*tok == ' ')
            tok++;
        pl->names[pl->count++] = tok;
        tok = strtok(NULL, ",");
    }
    return 0;
}

static void produce_list_free(produce_list_t* pl) {
    free(pl->names);
    free(pl->scratch);
}

// ----------------------------------------------------------------------------
// Subscribe-set predicate for batch_ctx — bridges produce_list_t →
// batch_subscribe_fn signature.
// ----------------------------------------------------------------------------

static bool produce_list_contains(void* vctx, const char* name) {
    const produce_list_t* pl = vctx;
    for (size_t i = 0; i < pl->count; i++) {
        if (strcmp(pl->names[i], name) == 0) return true;
    }
    return false;
}

// ----------------------------------------------------------------------------
// Manifest writer — fed by batch_ctx_for_each_produced.
// ----------------------------------------------------------------------------

typedef struct {
    FILE*  mf;
    size_t i;
    size_t total;
} manifest_writer_ctx_t;

static void manifest_visit(void* vctx, const batch_produced_t* e) {
    manifest_writer_ctx_t* mw = vctx;
    char hex[65];
    hash_format_hex(&e->hash, hex);
    fprintf(mw->mf, "    {\"name\":\"%s\",\"hash\":\"%s\",\"size\":%zu,\"path\":\"%s\"}%s\n",
            e->name, hex, e->size, e->path,
            mw->i + 1 == mw->total ? "" : ",");
    mw->i++;
}

// ----------------------------------------------------------------------------
// Batch mode main entry
// ----------------------------------------------------------------------------

static int cmd_batch(int argc, char** argv) {
    const char* pipeline_path = NULL;
    const char* input_json = NULL;
    const char* produce_csv = NULL;
    const char* output_dir = NULL;
    const char* cache_dir = getenv("DIORAMAP_CACHE_DIR");
    if (!cache_dir || !*cache_dir) {
        cache_dir = "/tmp/dioramap-cache";
    }

    for (int i = 0; i < argc; i++) {
        const char* a = argv[i];
        if (strcmp(a, "--pipeline") == 0 && i + 1 < argc) {
            pipeline_path = argv[++i];
        } else if (strcmp(a, "--input-json") == 0 && i + 1 < argc) {
            input_json = argv[++i];
        } else if (strcmp(a, "--produce") == 0 && i + 1 < argc) {
            produce_csv = argv[++i];
        } else if (strcmp(a, "--output-dir") == 0 && i + 1 < argc) {
            output_dir = argv[++i];
        } else if (strcmp(a, "--cache-dir") == 0 && i + 1 < argc) {
            cache_dir = argv[++i];
        } else {
            fprintf(stderr, "batch: unknown arg '%s'\n", a);
            return 2;
        }
    }

    if (!pipeline_path || !input_json || !produce_csv || !output_dir) {
        fprintf(stderr,
                "usage: dioramap-processor batch \\\n"
                "  --pipeline <name-or-path>   (embedded pipeline name, or path to .json)\n"
                "  --input-json <JSON-string>\n"
                "  --produce <artifact1,artifact2,...>\n"
                "  --output-dir <dir>\n"
                "  [--cache-dir <dir>]    (default: $DIORAMAP_CACHE_DIR or /tmp/dioramap-cache)\n");
        return 2;
    }

    if (mkdir_p(output_dir) != 0) {
        fprintf(stderr, "batch: cannot create output dir '%s'\n", output_dir);
        return 1;
    }

    log_init();

    // Register step packages. Real deployments register whatever step packages
    // they ship; the v3 tree currently includes demo, gpu-smoke, and fetchers.
    if (demo_register_steps() != 0) {
        fprintf(stderr, "batch: demo step registration failed\n");
        log_shutdown();
        return 1;
    }
    if (gpu_smoke_register() != 0) {
        fprintf(stderr, "batch: gpu-smoke step registration failed\n");
        log_shutdown();
        return 1;
    }
    if (fetchers_register_steps() != 0) {
        fprintf(stderr, "batch: fetcher step registration failed\n");
        log_shutdown();
        return 1;
    }
    if (coord_steps_register() != 0) {
        fprintf(stderr, "batch: coord step registration failed\n");
        log_shutdown();
        return 1;
    }
    if (copc_steps_register() != 0) {
        fprintf(stderr, "batch: copc step registration failed\n");
        log_shutdown();
        return 1;
    }
    if (normals_register_steps() != 0) {
        fprintf(stderr, "batch: normals step registration failed\n");
        log_shutdown();
        return 1;
    }
    if (registration_register_steps() != 0) {
        fprintf(stderr, "batch: registration step registration failed\n");
        log_shutdown();
        return 1;
    }
    if (ortho_register_steps() != 0) {
        fprintf(stderr, "batch: ortho step registration failed\n");
        log_shutdown();
        return 1;
    }
    if (classification_register_steps() != 0) {
        fprintf(stderr, "batch: classification step registration failed\n");
        log_shutdown();
        return 1;
    }
    if (ground_register_steps() != 0) {
        fprintf(stderr, "batch: ground step registration failed\n");
        log_shutdown();
        return 1;
    }
    if (shadow_register_steps() != 0) {
        fprintf(stderr, "batch: shadow step registration failed\n");
        log_shutdown();
        return 1;
    }
    if (buildings_register_steps() != 0) {
        fprintf(stderr, "batch: buildings step registration failed\n");
        log_shutdown();
        return 1;
    }
#if DIORAMAP_V7
    if (v7_register_steps() != 0) {
        fprintf(stderr, "batch: v7 step registration failed\n");
        log_shutdown();
        return 1;
    }
#endif

    char err[256] = {0};
    pipeline_t* pipeline = NULL;
    for (unsigned int i = 0; i < v3_embedded_pipelines_count; i++) {
        const embedded_pipeline_t* ep = &v3_embedded_pipelines[i];
        if (strcmp(ep->name, pipeline_path) == 0) {
            pipeline = pipeline_load_json_string((const char*)ep->json, ep->json_len,
                                                 err, sizeof(err));
            break;
        }
    }
    if (!pipeline) {
        pipeline = pipeline_load_json_file(pipeline_path, err, sizeof(err));
    }
    if (!pipeline) {
        fprintf(stderr, "batch: pipeline load failed: %s\n", err);
        log_shutdown();
        return 1;
    }
    if (pipeline_validate(pipeline) != 0) {
        fprintf(stderr, "batch: pipeline validation failed\n");
        pipeline_free(pipeline);
        log_shutdown();
        return 1;
    }

    produce_list_t pl;
    if (produce_list_parse(&pl, produce_csv) != 0) {
        fprintf(stderr, "batch: --produce parse error\n");
        pipeline_free(pipeline);
        log_shutdown();
        return 1;
    }

    store_config_t scfg = {.root_dir = cache_dir, .hot_budget_bytes = 512ULL * 1024 * 1024};
    store_t* store = store_open(&scfg);
    if (!store) {
        fprintf(stderr, "batch: cannot open store at '%s'\n", cache_dir);
        produce_list_free(&pl);
        pipeline_free(pipeline);
        log_shutdown();
        return 1;
    }

    char im_path[4096];
    int nn = snprintf(im_path, sizeof(im_path), "%s/intent.log", cache_dir);
    if (nn < 0 || (size_t)nn >= sizeof(im_path)) {
        fprintf(stderr, "batch: cache_dir too long for intent.log path\n");
        store_close(store);
        produce_list_free(&pl);
        pipeline_free(pipeline);
        log_shutdown();
        return 1;
    }
    intent_map_t* im = intent_map_open(im_path);
    if (!im) {
        fprintf(stderr, "batch: cannot open intent map at '%s'\n", im_path);
        store_close(store);
        produce_list_free(&pl);
        pipeline_free(pipeline);
        log_shutdown();
        return 1;
    }

    runner_t* runner = runner_new(&(runner_config_t){.store = store, .intent_map = im});

    batch_ctx_t* bc = batch_ctx_new(store, output_dir, produce_list_contains, &pl);
    if (!bc) {
        fprintf(stderr, "batch: oom\n");
        runner_free(runner);
        intent_map_close(im);
        store_close(store);
        produce_list_free(&pl);
        pipeline_free(pipeline);
        log_shutdown();
        return 1;
    }
    runner_callbacks_t cb;
    batch_ctx_install_callbacks(bc, &cb);

    // Canonicalise the user-supplied input JSON. Server's RUN handler
    // does the same (sort_keys + compact) so batch and serve modes
    // converge on byte-identical input → steps that read
    // PIPELINE_INPUT_ARTIFACT see one stable chain-hash regardless of
    // whether the input was typed in a CLI argv string or a UI textarea.
    char* canonical_input = NULL;
    size_t canonical_len = 0;
    {
        json_error_t je;
        // JSON_DECODE_ANY accepts top-level scalars (strings, numbers,
        // booleans, null) in addition to objects and arrays. The smoke
        // canary uses a JSON-quoted string; pipelines normally pass
        // objects or arrays. Either is fine here — we canonicalise via
        // json_dumps regardless.
        json_t* ji = json_loads(input_json, JSON_DECODE_ANY, &je);
        if (!ji) {
            fprintf(stderr, "batch: failed to parse --input-json: %s\n", je.text);
            batch_ctx_free(bc);
            runner_free(runner);
            intent_map_close(im);
            store_close(store);
            produce_list_free(&pl);
            pipeline_free(pipeline);
            log_shutdown();
            return 1;
        }
        canonical_input = json_dumps(ji, JSON_COMPACT | JSON_SORT_KEYS | JSON_ENCODE_ANY);
        json_decref(ji);
        if (!canonical_input) {
            fprintf(stderr, "batch: failed to canonicalise input\n");
            batch_ctx_free(bc);
            runner_free(runner);
            intent_map_close(im);
            store_close(store);
            produce_list_free(&pl);
            pipeline_free(pipeline);
            log_shutdown();
            return 1;
        }
        canonical_len = strlen(canonical_input);
    }

    int rc = runner_run(runner, pipeline, canonical_input, canonical_len,
                        (const char* const*)pl.names, pl.count, &cb);
    free(canonical_input);

    // Write manifest.json summarizing produced artifacts.
    char manifest_path[4096];
    int mn = snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", output_dir);
    FILE* mf = (mn < 0 || (size_t)mn >= sizeof(manifest_path)) ? NULL :
               fopen(manifest_path, "w");
    int errs = batch_ctx_error_count(bc);
    if (mf) {
        char run_id[24];
        batch_ctx_get_run_id(bc, run_id, sizeof(run_id));
        fprintf(mf, "{\n");
        fprintf(mf, "  \"pipeline\": \"%s\",\n", pipeline_name(pipeline));
        fprintf(mf, "  \"run_id\": \"%s\",\n", run_id);
        fprintf(mf, "  \"artifacts\": [\n");
        manifest_writer_ctx_t mw = { .mf = mf, .i = 0, .total = batch_ctx_n_produced(bc) };
        batch_ctx_for_each_produced(bc, manifest_visit, &mw);
        fprintf(mf, "  ],\n");
        fprintf(mf, "  \"ok\": %s\n", (rc == 0 && errs == 0) ? "true" : "false");
        fprintf(mf, "}\n");
        fclose(mf);
    }

    batch_ctx_free(bc);
    runner_free(runner);
    intent_map_close(im);
    store_close(store);
    produce_list_free(&pl);
    pipeline_free(pipeline);
    fetchers_shutdown();
    log_shutdown();
    return (rc == 0 && errs == 0) ? 0 : 1;
}

// ----------------------------------------------------------------------------
// Serve mode: pipeline registry + signal-driven shutdown
// ----------------------------------------------------------------------------

typedef struct pipeline_entry {
    char* name;
    pipeline_t* pipeline;
} pipeline_entry_t;

typedef struct pipeline_registry {
    pipeline_entry_t* entries;
    size_t n;
    size_t cap;
} pipeline_registry_t;

static int pipeline_registry_add(pipeline_registry_t* reg, const char* name, pipeline_t* p) {
    if (reg->n == reg->cap) {
        // Frama-C audit finding MAIN-2: cap-doubling overflow guard.
        if (reg->cap > SIZE_MAX / 2) return -1;
        size_t nc = reg->cap ? reg->cap * 2 : 8;
        if (nc > SIZE_MAX / sizeof(pipeline_entry_t)) return -1;
        pipeline_entry_t* na = realloc(reg->entries, nc * sizeof(*na));
        if (!na)
            return -1;
        reg->entries = na;
        reg->cap = nc;
    }
    reg->entries[reg->n].name = strdup(name);
    reg->entries[reg->n].pipeline = p;
    if (!reg->entries[reg->n].name) {
        pipeline_free(p);
        return -1;
    }
    reg->n++;
    return 0;
}

static const pipeline_t* pipeline_registry_resolve(void* vctx, const char* name) {
    pipeline_registry_t* reg = vctx;
    for (size_t i = 0; i < reg->n; i++) {
        if (strcmp(reg->entries[i].name, name) == 0) {
            return reg->entries[i].pipeline;
        }
    }
    return NULL;
}

static size_t pipeline_registry_list(void* vctx, const char** out_names, size_t cap) {
    pipeline_registry_t* reg = vctx;
    if (!out_names) {
        return reg->n;
    }
    size_t n = reg->n < cap ? reg->n : cap;
    for (size_t i = 0; i < n; i++) {
        out_names[i] = reg->entries[i].name;
    }
    return n;
}

static void pipeline_registry_free(pipeline_registry_t* reg) {
    for (size_t i = 0; i < reg->n; i++) {
        free(reg->entries[i].name);
        pipeline_free(reg->entries[i].pipeline);
    }
    free(reg->entries);
    reg->entries = NULL;
    reg->n = reg->cap = 0;
}

static int ends_with(const char* s, const char* suffix) {
    size_t ls = strlen(s), lf = strlen(suffix);
    if (lf > ls)
        return 0;
    return strcmp(s + ls - lf, suffix) == 0;
}

// Load pipelines that were baked into the binary at compile time. Returns
// the count loaded; logs each one to stderr. The embedded table lives in
// generated/embedded_pipelines.c, populated by `scripts/embed-pipelines.mjs`.
static int pipeline_registry_load_embedded(pipeline_registry_t* reg) {
    int loaded = 0;
    for (unsigned int i = 0; i < v3_embedded_pipelines_count; i++) {
        const embedded_pipeline_t* ep = &v3_embedded_pipelines[i];
        char err[256] = {0};
        pipeline_t* p = pipeline_load_json_string(
            (const char*)ep->json, (size_t)ep->json_len, err, sizeof(err));
        if (!p) {
            fprintf(stderr,
                    "serve: failed to load embedded pipeline '%s': %s\n",
                    ep->name, err);
            continue;
        }
        if (pipeline_validate(p) != 0) {
            fprintf(stderr,
                    "serve: embedded pipeline '%s' failed validation\n", ep->name);
            pipeline_free(p);
            continue;
        }
        const char* pname = pipeline_name(p);
        if (pipeline_registry_add(reg, pname, p) != 0) {
            fprintf(stderr, "serve: oom registering embedded pipeline\n");
            pipeline_free(p);
            continue;
        }
        fprintf(stderr, "[serve] loaded embedded pipeline '%s'\n", pname);
        loaded++;
    }
    return loaded;
}

static int pipeline_registry_load_dir(pipeline_registry_t* reg, const char* dir) {
    DIR* d = opendir(dir);
    if (!d) {
        fprintf(stderr, "serve: cannot open pipelines dir '%s'\n", dir);
        return -1;
    }
    struct dirent* e;
    int rc = 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        if (!ends_with(e->d_name, ".json"))
            continue;
        char path[4096];
        int n = snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (n < 0 || (size_t)n >= sizeof(path))
            continue;
        char err[256] = {0};
        pipeline_t* p = pipeline_load_json_file(path, err, sizeof(err));
        if (!p) {
            fprintf(stderr, "serve: failed to load '%s': %s\n", path, err);
            rc = -1;
            continue;
        }
        if (pipeline_validate(p) != 0) {
            fprintf(stderr, "serve: pipeline '%s' failed validation\n", path);
            pipeline_free(p);
            rc = -1;
            continue;
        }
        const char* pname = pipeline_name(p);
        if (pipeline_registry_add(reg, pname, p) != 0) {
            fprintf(stderr, "serve: oom registering pipeline\n");
            pipeline_free(p);
            rc = -1;
        } else {
            fprintf(stderr, "[serve] loaded pipeline '%s' from %s\n", pname, e->d_name);
        }
    }
    closedir(d);
    return rc;
}

static server_t* g_server = NULL;
static void handle_signal(int sig) {
    (void)sig;
    if (g_server)
        server_stop(g_server);
}

static int cmd_serve(int argc, char** argv) {
    const char* cache_dir = getenv("DIORAMAP_CACHE_DIR");
    if (!cache_dir || !*cache_dir)
        cache_dir = "/tmp/dioramap-cache";
    const char* pipelines_dir = NULL;
    // Network exposure: bind to loopback by default. Pass `--bind *` (all
    // interfaces) or an address to expose the server, ideally with --token.
    const char* bind_iface = "127.0.0.1";
    // Optional shared token (also via DIORAMAP_TOKEN). Empty = no auth.
    const char* token = getenv("DIORAMAP_TOKEN");
    int port = 7801;

    for (int i = 0; i < argc; i++) {
        const char* a = argv[i];
        if (strcmp(a, "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(a, "--cache-dir") == 0 && i + 1 < argc) {
            cache_dir = argv[++i];
        } else if (strcmp(a, "--pipelines-dir") == 0 && i + 1 < argc) {
            pipelines_dir = argv[++i];
        } else if (strcmp(a, "--bind") == 0 && i + 1 < argc) {
            bind_iface = argv[++i];
        } else if (strcmp(a, "--token") == 0 && i + 1 < argc) {
            token = argv[++i];
        } else {
            fprintf(stderr, "serve: unknown arg '%s'\n", a);
            return 2;
        }
    }
    // --pipelines-dir is optional. When absent, we fall back to the
    // pipelines baked into the binary at compile time
    // (v3_embedded_pipelines, generated by scripts/embed-pipelines.mjs).
    // The dir form remains supported for dev/staging use cases that swap
    // in custom pipelines without a rebuild.
    if (token && token[0] && strlen(token) < 16) {
        fprintf(stderr, "serve: --token must be at least 16 chars — refusing to start\n");
        return 2;
    }
    const v3_extension_t* ext = v3_extension();
    bool ext_auth = ext && ext->authenticate;
    if (strcmp(bind_iface, "127.0.0.1") != 0 && !(token && token[0]) && !ext_auth) {
        fprintf(stderr, "[serve] WARN listening on %s without --token: anyone who can reach "
                        "this port can run pipelines\n", bind_iface);
    }

    log_init();
    if (demo_register_steps() != 0 || gpu_smoke_register() != 0 ||
        fetchers_register_steps() != 0 || coord_steps_register() != 0 ||
        copc_steps_register() != 0 || normals_register_steps() != 0 ||
        registration_register_steps() != 0 || ortho_register_steps() != 0 ||
        classification_register_steps() != 0 || ground_register_steps() != 0 ||
        shadow_register_steps() != 0 || buildings_register_steps() != 0
#if DIORAMAP_V7
        || v7_register_steps() != 0
#endif
        ) {
        fprintf(stderr, "serve: step registration failed\n");
        log_shutdown();
        return 1;
    }

    pipeline_registry_t reg = {0};
    if (pipelines_dir) {
        if (pipeline_registry_load_dir(&reg, pipelines_dir) != 0 && reg.n == 0) {
            fprintf(stderr, "serve: no pipelines loaded\n");
            log_shutdown();
            return 1;
        }
    } else {
        if (pipeline_registry_load_embedded(&reg) == 0) {
            fprintf(stderr, "serve: no embedded pipelines available\n");
            log_shutdown();
            return 1;
        }
    }

    store_config_t scfg = {.root_dir = cache_dir, .hot_budget_bytes = 2ULL * 1024 * 1024 * 1024};
    store_t* store = store_open(&scfg);
    if (!store) {
        fprintf(stderr, "serve: cannot open store at '%s'\n", cache_dir);
        pipeline_registry_free(&reg);
        log_shutdown();
        return 1;
    }
    char im_path[4096];
    int sn = snprintf(im_path, sizeof(im_path), "%s/intent.log", cache_dir);
    if (sn < 0 || (size_t)sn >= sizeof(im_path)) {
        fprintf(stderr, "serve: cache_dir too long for intent.log path\n");
        store_close(store);
        pipeline_registry_free(&reg);
        log_shutdown();
        return 1;
    }
    intent_map_t* im = intent_map_open(im_path);
    if (!im) {
        fprintf(stderr, "serve: cannot open intent map at '%s'\n", im_path);
        store_close(store);
        pipeline_registry_free(&reg);
        log_shutdown();
        return 1;
    }

    // Phase B startup integrity audit. Cross-check every intent.log
    // entry against the store. A nonzero `missing_content` means the
    // cache desynced (typical cause: SIGKILL or external rm). The
    // runner self-heals at runtime via the resolve_manifest stale path
    // + F2 step_read recovery; this snapshot is operator-facing
    // observability, not a corrective action.
    {
        integrity_audit_t actx = {.store = store};
        intent_map_for_each(im, audit_intent_cb, &actx);
        LOG_EVENT(SUB_STORE, LOG_INFO, EV_STORE_SCAN_COMPLETE,
                  LF_STR("audit", "intent_vs_store"),
                  LF_UINT("intent_entries", actx.total),
                  LF_UINT("missing_content", actx.missing_content));
        if (actx.missing_content > 0) {
            fprintf(stderr,
                    "[serve] WARN cache integrity: %llu of %llu intent.log entries "
                    "point at content blobs that are missing from the store. The "
                    "runner will recover by re-running affected steps on first "
                    "use; expect slow first-loads.\n",
                    (unsigned long long)actx.missing_content,
                    (unsigned long long)actx.total);
        } else if (actx.total > 0) {
            fprintf(stderr,
                    "[serve] cache integrity: %llu intent.log entries, all content present\n",
                    (unsigned long long)actx.total);
        }
    }

    runner_t* runner = runner_new(&(runner_config_t){.store = store, .intent_map = im});

    // L6 — open the run journal alongside the store + intent map. It
    // lives at <cache_dir>/runs.sqlite and is best-effort: if it fails
    // to open (out of disk, fs permissions, etc.) we log and run with a
    // NULL journal — the server falls back to "feature unavailable" on
    // LIST_RUNS / REHYDRATE_RUN messages, but normal pipeline execution
    // is unaffected.
    char journal_path[4096];
    int jn = snprintf(journal_path, sizeof(journal_path), "%s/runs.sqlite", cache_dir);
    if (jn < 0 || (size_t)jn >= sizeof(journal_path)) {
        // Best-effort journal — treat over-long path as "unavailable" rather
        // than abort serve startup. The server already falls back to "feature
        // unavailable" on LIST_RUNS / REHYDRATE_RUN when the journal is NULL.
        journal_path[0] = '\0';
    }
    run_journal_t* journal = run_journal_open(journal_path);
    if (!journal) {
        fprintf(stderr, "serve: warning — run journal disabled (open '%s' failed)\n",
                journal_path);
    } else {
        run_journal_prune(journal);
    }

    // Processor extension (v3/extension.h; -DDIORAMAP_EXTENSION_DIR).
    void* ext_state = NULL;
    if (ext && ext->init) {
        v3_ext_env_t env = {.cache_dir = cache_dir, .processor_version = DIORAMAP_VERSION};
        ext_state = ext->init(&env);
        if (!ext_state) {
            fprintf(stderr, "serve: extension '%s' failed to initialise — refusing to start\n",
                    ext->name ? ext->name : "?");
            if (journal) run_journal_close(journal);
            runner_free(runner);
            intent_map_close(im);
            store_close(store);
            pipeline_registry_free(&reg);
            log_shutdown();
            return 1;
        }
    }

    server_config_t cfg = {
        .port = port,
        .runner = runner,
        .store = store,
        .journal = journal,
        .iface = (strcmp(bind_iface, "*") == 0) ? NULL : bind_iface,
        .token = token,
        .extension = ext,
        .extension_state = ext_state,
        .resolve = pipeline_registry_resolve,
        .list = pipeline_registry_list,
        .resolve_ctx = &reg,
    };
    server_t* server = server_new(&cfg);
    if (!server) {
        fprintf(stderr, "serve: server_new failed (port %d already in use?)\n", port);
        if (ext && ext->shutdown) ext->shutdown(ext_state);
        runner_free(runner);
        intent_map_close(im);
        store_close(store);
        pipeline_registry_free(&reg);
        log_shutdown();
        return 1;
    }

    g_server = server;
    struct sigaction sa = {0};
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    char auth_desc[96];
    snprintf(auth_desc, sizeof(auth_desc), "%s%s%s",
             (token && token[0]) ? "token" : (ext_auth ? "" : "none"),
             ((token && token[0]) && ext_auth) ? "+" : "",
             ext_auth ? (ext->name ? ext->name : "extension") : "");
    fprintf(stderr, "[serve] listening on %s:%d (pipelines: %zu, cache: %s, auth: %s)\n",
            bind_iface, port, reg.n, cache_dir, auth_desc);
    int rc = server_run(server);

    g_server = NULL;
    server_free(server);
    if (ext && ext->shutdown) ext->shutdown(ext_state);
    runner_free(runner);
    intent_map_close(im);
    if (journal) run_journal_close(journal);
    store_close(store);
    pipeline_registry_free(&reg);
    fetchers_shutdown();
    log_shutdown();
    return rc;
}

// ----------------------------------------------------------------------------
// Top-level dispatch
// ----------------------------------------------------------------------------

static void print_version(FILE* out) {
    fprintf(out, "dioramap-processor v%s\n", DIORAMAP_VERSION);
}

static void print_usage(FILE* out) {
    fprintf(out, "usage: dioramap-processor <subcommand> [args...]\n"
                 "\n"
                 "Subcommands:\n"
                 "  batch     Run a pipeline once and write artifacts to a directory.\n"
                 "  serve     Run the WebSocket server.\n"
                 "            [--port N] [--bind ADDR|*] [--token SECRET]\n"
                 "            [--cache-dir DIR] [--pipelines-dir DIR]\n"
                 "  --version | -v   Print version.\n"
                 "  --help    | -h   This help.\n"
                 "\n"
                 "Environment:\n"
                 "  DIORAMAP_LOG_LEVEL   debug|info|warn|error  (default: info)\n"
                 "  DIORAMAP_CACHE_DIR   path to cache root     (default: /tmp/dioramap-cache)\n"
                 "  DIORAMAP_TOKEN       shared token for serve (default: none = no auth)\n");
}

int main(int argc, char** argv) {
    if (argc >= 2) {
        const char* a = argv[1];
        if (strcmp(a, "--version") == 0 || strcmp(a, "-v") == 0) {
            print_version(stdout);
            return 0;
        }
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            print_version(stdout);
            print_usage(stdout);
            return 0;
        }
        if (strcmp(a, "batch") == 0) {
            return cmd_batch(argc - 2, argv + 2);
        }
        if (strcmp(a, "serve") == 0) {
            return cmd_serve(argc - 2, argv + 2);
        }
        fprintf(stderr, "dioramap-processor: unknown subcommand '%s'\n", a);
        print_usage(stderr);
        return 2;
    }

    print_version(stderr);
    print_usage(stderr);
    return 0;
}
