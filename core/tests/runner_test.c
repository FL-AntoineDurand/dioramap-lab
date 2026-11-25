// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "v3/runner.h"

static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        g_checks++;                                                                                \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                        \
            g_failed++;                                                                            \
        }                                                                                          \
    } while (0)

// ----------------------------------------------------------------------------
// Test dummy pipeline: 3 steps forming a small DAG.
//
//   pipeline-input ──► double-it ──► doubled ─┐
//                                             ├──► sum-two ──► total
//   pipeline-input ──► triple-it ──► tripled ─┘
//
// Every output is content-hashed (HASH-MODEL.md §5). double-it and triple-it
// both read pipeline-input; sum-two reads both outputs.
// ----------------------------------------------------------------------------

static intent_map_t* open_test_im(const char* root) {
    char im_path[512];
    snprintf(im_path, sizeof(im_path), "%s/intent.log", root);
    return intent_map_open(im_path);
}

static atomic_int g_double_runs = 0;
static atomic_int g_triple_runs = 0;
static atomic_int g_sum_runs = 0;

static int double_exec(step_context_t* ctx) {
    size_t n;
    const void* in = step_read(ctx, PIPELINE_INPUT_ARTIFACT, &n);
    if (!in || n < sizeof(int32_t)) {
        return -1;
    }
    int32_t v;
    memcpy(&v, in, sizeof(v));
    v *= 2;
    atomic_fetch_add_explicit(&g_double_runs, 1, memory_order_relaxed);
    return step_write_output(ctx, "doubled", &v, sizeof(v));
}

static int triple_exec(step_context_t* ctx) {
    size_t n;
    const void* in = step_read(ctx, PIPELINE_INPUT_ARTIFACT, &n);
    if (!in || n < sizeof(int32_t)) {
        return -1;
    }
    int32_t v;
    memcpy(&v, in, sizeof(v));
    v *= 3;
    atomic_fetch_add_explicit(&g_triple_runs, 1, memory_order_relaxed);
    return step_write_output(ctx, "tripled", &v, sizeof(v));
}

static int sum_exec(step_context_t* ctx) {
    size_t n_a, n_b;
    const void* a = step_read(ctx, "doubled", &n_a);
    const void* b = step_read(ctx, "tripled", &n_b);
    if (!a || !b) {
        return -1;
    }
    int32_t va, vb;
    memcpy(&va, a, sizeof(va));
    memcpy(&vb, b, sizeof(vb));
    int32_t sum = va + vb;
    atomic_fetch_add_explicit(&g_sum_runs, 1, memory_order_relaxed);
    return step_write_output(ctx, "total", &sum, sizeof(sum));
}

static const step_read_t double_reads[] = {{.name = PIPELINE_INPUT_ARTIFACT}};
static const step_write_t double_writes[] = {
    {.name = "doubled",
     .kind = 1,
     .visibility = STEP_VIS_PUBLIC},
};
static const step_def_t DOUBLE_DEF = {
    .name = "double-it",
    .version = 1,
    .reads = double_reads,
    .n_reads = 1,
    .writes = double_writes,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = double_exec,
};

static const step_read_t triple_reads[] = {{.name = PIPELINE_INPUT_ARTIFACT}};
static const step_write_t triple_writes[] = {
    {.name = "tripled",
     .kind = 1,
     .visibility = STEP_VIS_PUBLIC},
};
static const step_def_t TRIPLE_DEF = {
    .name = "triple-it",
    .version = 1,
    .reads = triple_reads,
    .n_reads = 1,
    .writes = triple_writes,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = triple_exec,
};

// ----------------------------------------------------------------------------
// Extra step defs for coverage gap tests.
// ----------------------------------------------------------------------------

#include <jansson.h>

// content-echo: reads pipeline-input, writes it verbatim.
// Used to verify every write is content-hashed (hash == hash_bytes(output)).
static atomic_int g_content_runs = 0;
static int content_echo_exec(step_context_t* ctx) {
    size_t n = 0;
    const void* in = step_read(ctx, PIPELINE_INPUT_ARTIFACT, &n);
    if (!in) {
        return -1;
    }
    atomic_fetch_add_explicit(&g_content_runs, 1, memory_order_relaxed);
    return step_write_output(ctx, "content-echoed", in, n);
}
static const step_read_t content_reads[] = {{.name = PIPELINE_INPUT_ARTIFACT}};
static const step_write_t content_writes[] = {
    {.name = "content-echoed",
     .kind = 7,
     .visibility = STEP_VIS_DEBUG},
};
static const step_def_t CONTENT_ECHO_DEF = {
    .name = "content-echo",
    .version = 1,
    .reads = content_reads,
    .n_reads = 1,
    .writes = content_writes,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = content_echo_exec,
};

// multi-algo: one step, params["algo"] ∈ {"square","cube"} selects behavior.
// Reads pipeline-input (int32), writes result (int32).
static atomic_int g_multi_runs = 0;
static int multi_algo_exec(step_context_t* ctx) {
    size_t n = 0;
    const void* in = step_read(ctx, PIPELINE_INPUT_ARTIFACT, &n);
    if (!in || n < sizeof(int32_t)) {
        return -1;
    }
    int32_t v;
    memcpy(&v, in, sizeof(v));

    const char* params = step_get_params(ctx);
    const char* algo = "square";
    json_error_t je;
    json_t* root = params ? json_loads(params, 0, &je) : NULL;
    if (root) {
        const json_t* j_algo = json_object_get(root, "algo");
        if (json_is_string(j_algo)) {
            algo = json_string_value(j_algo);
        }
    }
    int32_t out = 0;
    if (strcmp(algo, "square") == 0) {
        out = v * v;
    } else if (strcmp(algo, "cube") == 0) {
        out = v * v * v;
    } else {
        if (root) {
            json_decref(root);
        }
        return -1;
    }
    if (root) {
        json_decref(root);
    }
    atomic_fetch_add_explicit(&g_multi_runs, 1, memory_order_relaxed);
    return step_write_output(ctx, "algo-result", &out, sizeof(out));
}
static const step_read_t multi_reads[] = {{.name = PIPELINE_INPUT_ARTIFACT}};
static const step_write_t multi_writes[] = {
    {.name = "algo-result",
     .kind = 1,
     .visibility = STEP_VIS_DEBUG},
};
static const step_def_t MULTI_ALGO_DEF = {
    .name = "multi-algo",
    .version = 1,
    .reads = multi_reads,
    .n_reads = 1,
    .writes = multi_writes,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = multi_algo_exec,
};

// Two cyclic steps for cycle-detection test.
// cycle-a reads "b-art", writes "a-art"; cycle-b reads "a-art", writes "b-art".
// Neither produces anything useful — the point is the runner's topo sort rejects.
static int cycle_a_exec(step_context_t* ctx) {
    (void)ctx;
    return -1;
}
static const step_read_t cycle_a_reads[] = {{.name = "b-art"}};
static const step_write_t cycle_a_writes[] = {
    {.name = "a-art", .kind = 1, .visibility = STEP_VIS_DEBUG},
};
static const step_def_t CYCLE_A_DEF = {
    .name = "cycle-a",
    .version = 1,
    .reads = cycle_a_reads,
    .n_reads = 1,
    .writes = cycle_a_writes,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = cycle_a_exec,
};
static int cycle_b_exec(step_context_t* ctx) {
    (void)ctx;
    return -1;
}
static const step_read_t cycle_b_reads[] = {{.name = "a-art"}};
static const step_write_t cycle_b_writes[] = {
    {.name = "b-art", .kind = 1, .visibility = STEP_VIS_DEBUG},
};
static const step_def_t CYCLE_B_DEF = {
    .name = "cycle-b",
    .version = 1,
    .reads = cycle_b_reads,
    .n_reads = 1,
    .writes = cycle_b_writes,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = cycle_b_exec,
};

// Duplicate writer: "double-copy" also writes "doubled" like DOUBLE_DEF,
// used to exercise pipeline_validate's duplicate-write rejection.
static int dup_write_exec(step_context_t* ctx) {
    size_t n;
    const void* in = step_read(ctx, PIPELINE_INPUT_ARTIFACT, &n);
    if (!in) {
        return -1;
    }
    int32_t v = 0;
    memcpy(&v, in, sizeof(v));
    v *= 2;
    return step_write_output(ctx, "doubled", &v, sizeof(v));
}
static const step_read_t dup_reads[] = {{.name = PIPELINE_INPUT_ARTIFACT}};
static const step_write_t dup_writes[] = {
    {.name = "doubled", .kind = 1, .visibility = STEP_VIS_DEBUG},
};
static const step_def_t DUP_WRITER_DEF = {
    .name = "double-copy",
    .version = 1,
    .reads = dup_reads,
    .n_reads = 1,
    .writes = dup_writes,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = dup_write_exec,
};

// Always-fails step for step-error tests.
static atomic_int g_fail_runs = 0;
static int fail_exec(step_context_t* ctx) {
    (void)ctx;
    atomic_fetch_add_explicit(&g_fail_runs, 1, memory_order_relaxed);
    return -1; // never writes its declared output
}
static const step_read_t fail_reads[] = {{.name = PIPELINE_INPUT_ARTIFACT}};
static const step_write_t fail_writes[] = {
    {.name = "fail-result",
     .kind = 1,
     .visibility = STEP_VIS_DEBUG},
};
static const step_def_t FAIL_DEF = {
    .name = "always-fails",
    .version = 1,
    .reads = fail_reads,
    .n_reads = 1,
    .writes = fail_writes,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = fail_exec,
};

// Fails WITH a reason, after recording upstream evidence the way the fetch
// layer does (docs/ERRORS.md).
static int reasoned_fail_exec(step_context_t* ctx) {
    v3_error_set_upstream(V3_ERR_UPSTREAM_HTTP,
                          "{\"url\":\"https://example.test/wfs\",\"http_status\":400}",
                          "HTTP 400 from example.test: Unknown namespace");
    return step_fail_detail(ctx, V3_ERR_UPSTREAM_HTTP, "{\"lat\":48.1}",
                            "tile index query failed");
}
static const step_write_t reasoned_fail_writes[] = {
    {.name = "reasoned-result", .kind = 1, .visibility = STEP_VIS_DEBUG},
};
static const step_def_t REASONED_FAIL_DEF = {
    .name = "fails-with-reason",
    .version = 3,
    .reads = fail_reads,
    .n_reads = 1,
    .writes = reasoned_fail_writes,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = reasoned_fail_exec,
};

// Tolerates an upstream failure and succeeds: the evidence must not leak
// into a later failure report.
static int tolerant_exec(step_context_t* ctx) {
    v3_error_set_upstream(V3_ERR_TIMEOUT, NULL, "optional fetch timed out");
    int32_t v = 7;
    return step_write_output(ctx, "tolerant-result", &v, sizeof(v));
}
static const step_write_t tolerant_writes[] = {
    {.name = "tolerant-result", .kind = 1, .visibility = STEP_VIS_DEBUG},
};
static const step_def_t TOLERANT_DEF = {
    .name = "tolerant",
    .version = 1,
    .reads = fail_reads,
    .n_reads = 1,
    .writes = tolerant_writes,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = tolerant_exec,
};

// Non-deterministic step: declares nondeterministic=true on its write so the
// runner bypasses the intent map and always re-runs. Writes a monotonically
// increasing counter as its output.
static atomic_int g_nondet_runs = 0;
static int nondet_exec(step_context_t* ctx) {
    int32_t count = atomic_fetch_add_explicit(&g_nondet_runs, 1, memory_order_relaxed) + 1;
    return step_write_output(ctx, "nondet-counter", &count, sizeof(count));
}
static const step_read_t nondet_reads[] = {{.name = PIPELINE_INPUT_ARTIFACT}};
static const step_write_t nondet_writes[] = {
    {.name = "nondet-counter",
     .kind = 1,
     .visibility = STEP_VIS_DEBUG,
     .nondeterministic = true},
};
static const step_def_t NONDET_DEF = {
    .name = "nondet-counter-step",
    .version = 1,
    .reads = nondet_reads,
    .n_reads = 1,
    .writes = nondet_writes,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = nondet_exec,
};

static const step_read_t sum_reads[] = {{.name = "doubled"}, {.name = "tripled"}};
static const step_write_t sum_writes[] = {
    {.name = "total", .kind = 1, .visibility = STEP_VIS_PUBLIC},
};
static const step_def_t SUM_DEF = {
    .name = "sum-two",
    .version = 1,
    .reads = sum_reads,
    .n_reads = 2,
    .writes = sum_writes,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = sum_exec,
};

// ----------------------------------------------------------------------------
// Streaming step (B9): writes 4 chunks of 1024 bytes via
// step_write_output_stream, the last with final=true. Used to exercise the
// runner accumulator + finalisation path.
// ----------------------------------------------------------------------------

static int stream_exec(step_context_t* ctx) {
    uint8_t chunk[1024];
    for (int i = 0; i < 4; i++) {
        for (size_t k = 0; k < sizeof(chunk); k++) {
            chunk[k] = (uint8_t)(i * 31 + k);
        }
        bool final = (i == 3);
        if (step_write_output_stream(ctx, "stream-out", chunk, sizeof(chunk), final) != 0) {
            return -1;
        }
    }
    return 0;
}
static const step_write_t stream_writes[] = {
    {.name = "stream-out", .kind = 1, .visibility = STEP_VIS_PUBLIC,
     .streaming = true},
};
static const step_read_t stream_reads[] = {{.name = PIPELINE_INPUT_ARTIFACT}};
static const step_def_t STREAM_DEF = {
    .name = "stream-fixture",
    .version = 1,
    .reads = stream_reads,
    .n_reads = 1,
    .writes = stream_writes,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = stream_exec,
};

// Reader that consumes the streamed artifact downstream — verifies the
// finalised bytes equal the concatenation of chunks the writer emitted.
static atomic_uint_fast32_t g_stream_total_len = 0;
static atomic_uint_fast32_t g_stream_first_byte = 0;
static atomic_uint_fast32_t g_stream_last_byte = 0;
static int stream_check_exec(step_context_t* ctx) {
    size_t n = 0;
    const uint8_t* p = step_read(ctx, "stream-out", &n);
    if (!p) return -1;
    atomic_store_explicit(&g_stream_total_len, (uint_fast32_t)n, memory_order_relaxed);
    if (n > 0) {
        atomic_store_explicit(&g_stream_first_byte, p[0], memory_order_relaxed);
        atomic_store_explicit(&g_stream_last_byte, p[n - 1], memory_order_relaxed);
    }
    uint32_t marker = 1;
    return step_write_output(ctx, "stream-checked", &marker, sizeof(marker));
}
static const step_read_t stream_check_reads[] = {{.name = "stream-out"}};
static const step_write_t stream_check_writes[] = {
    {.name = "stream-checked", .kind = 1, .visibility = STEP_VIS_DEBUG},
};
static const step_def_t STREAM_CHECK_DEF = {
    .name = "stream-check",
    .version = 1,
    .reads = stream_check_reads,
    .n_reads = 1,
    .writes = stream_check_writes,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = stream_check_exec,
};

// ----------------------------------------------------------------------------
// Callback that captures the run's artifacts and final summary.
// ----------------------------------------------------------------------------

typedef struct cb_state {
    atomic_size_t n_plan;
    atomic_size_t n_step_started;
    atomic_size_t n_step_complete;
    atomic_size_t n_cache_hits;
    atomic_size_t n_artifact_ready;
    atomic_size_t n_run_complete;
    atomic_size_t n_error;
    // on_run_failed capture (guarded by g_cb_lock).
    atomic_size_t n_run_failed;
    v3_error_code_t fail_code;
    char fail_msg[1024];
    char fail_step[64];
    char fail_detail[1024];
    // Stream callback counters (B9). n_stream_chunks counts total invocations
    // (final and non-final); n_stream_final counts only final=true; the bytes
    // counter sums sizes across all chunks for a sanity check against
    // step_read on the finalised artifact.
    atomic_size_t n_stream_chunks;
    atomic_size_t n_stream_final;
    atomic_size_t stream_bytes_seen;
    // Writes to last_total_hash + got_total guarded by g_cb_lock.
    hash_t last_total_hash;
    bool got_total;
} cb_state_t;

static pthread_mutex_t g_cb_lock = PTHREAD_MUTEX_INITIALIZER;

static void cb_started(void* ctx, const char* run_id, const char* pipeline_name,
                       const char* const* plan, size_t n_plan) {
    (void)run_id;
    (void)pipeline_name;
    (void)plan;
    cb_state_t* s = ctx;
    atomic_store_explicit(&s->n_plan, n_plan, memory_order_relaxed);
}

static void cb_step_started(void* ctx, const char* run_id, const char* step) {
    (void)run_id;
    (void)step;
    atomic_fetch_add_explicit(&((cb_state_t*)ctx)->n_step_started, 1, memory_order_relaxed);
}

static void cb_step_complete(void* ctx, const char* run_id, const char* step, bool cache_hit,
                             uint64_t dt) {
    (void)run_id;
    (void)step;
    (void)dt;
    cb_state_t* s = ctx;
    atomic_fetch_add_explicit(&s->n_step_complete, 1, memory_order_relaxed);
    if (cache_hit) {
        atomic_fetch_add_explicit(&s->n_cache_hits, 1, memory_order_relaxed);
    }
}

static void cb_artifact(void* ctx, const char* run_id, const char* name, const hash_t* h) {
    (void)run_id;
    cb_state_t* s = ctx;
    atomic_fetch_add_explicit(&s->n_artifact_ready, 1, memory_order_relaxed);
    if (strcmp(name, "total") == 0) {
        pthread_mutex_lock(&g_cb_lock);
        s->last_total_hash = *h;
        s->got_total = true;
        pthread_mutex_unlock(&g_cb_lock);
    }
}

static void cb_stream_chunk(void* ctx, const char* run_id, const char* name, uint32_t seq,
                             bool final, const void* bytes, size_t len, const hash_t* hash) {
    (void)run_id;
    (void)name;
    (void)seq;
    (void)bytes;
    (void)hash;
    cb_state_t* s = ctx;
    atomic_fetch_add_explicit(&s->n_stream_chunks, 1, memory_order_relaxed);
    if (final) {
        atomic_fetch_add_explicit(&s->n_stream_final, 1, memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&s->stream_bytes_seen, len, memory_order_relaxed);
}

static void cb_run_complete(void* ctx, const char* run_id, uint64_t dt, size_t executed,
                            size_t hits) {
    (void)run_id;
    (void)dt;
    (void)executed;
    (void)hits;
    atomic_fetch_add_explicit(&((cb_state_t*)ctx)->n_run_complete, 1, memory_order_relaxed);
}

static void cb_error(void* ctx, const char* run_id, const char* step, const char* msg) {
    (void)run_id;
    (void)step;
    fprintf(stderr, "runner error: step=%s msg=%s\n", step ? step : "(none)", msg);
    atomic_fetch_add_explicit(&((cb_state_t*)ctx)->n_error, 1, memory_order_relaxed);
}

static void cb_run_failed(void* ctx, const char* run_id, const v3_failure_t* f, uint64_t dt) {
    (void)run_id;
    (void)dt;
    cb_state_t* s = ctx;
    pthread_mutex_lock(&g_cb_lock);
    s->fail_code = f->code;
    snprintf(s->fail_msg, sizeof(s->fail_msg), "%s", f->message);
    snprintf(s->fail_step, sizeof(s->fail_step), "%s", f->step ? f->step : "");
    snprintf(s->fail_detail, sizeof(s->fail_detail), "%s", f->detail_json ? f->detail_json : "");
    pthread_mutex_unlock(&g_cb_lock);
    atomic_fetch_add_explicit(&s->n_run_failed, 1, memory_order_relaxed);
}

static runner_callbacks_t make_cb(cb_state_t* s) {
    runner_callbacks_t cb = {
        .ctx = s,
        .on_run_started = cb_started,
        .on_step_started = cb_step_started,
        .on_step_complete = cb_step_complete,
        .on_artifact_ready = cb_artifact,
        .on_stream_chunk = cb_stream_chunk,
        .on_run_complete = cb_run_complete,
        .on_error = cb_error,
        .on_run_failed = cb_run_failed,
    };
    return cb;
}

// ----------------------------------------------------------------------------
// Test helpers
// ----------------------------------------------------------------------------

static void rm_rf(const char* path) {
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    (void)!system(cmd);
}

static void make_tmpdir(char* out, size_t out_sz) {
    snprintf(out, out_sz, "/tmp/dioramap_runner_test_XXXXXX");
    if (mkdtemp(out) == NULL) {
        exit(1);
    }
}

static void register_all(void) {
    step_registry_reset();
    CHECK(step_register(&DOUBLE_DEF) == 0);
    CHECK(step_register(&TRIPLE_DEF) == 0);
    CHECK(step_register(&SUM_DEF) == 0);
    // Extras for coverage-gap tests.
    CHECK(step_register(&CONTENT_ECHO_DEF) == 0);
    CHECK(step_register(&MULTI_ALGO_DEF) == 0);
    CHECK(step_register(&CYCLE_A_DEF) == 0);
    CHECK(step_register(&CYCLE_B_DEF) == 0);
    CHECK(step_register(&DUP_WRITER_DEF) == 0);
    CHECK(step_register(&FAIL_DEF) == 0);
    CHECK(step_register(&REASONED_FAIL_DEF) == 0);
    CHECK(step_register(&TOLERANT_DEF) == 0);
    CHECK(step_register(&NONDET_DEF) == 0);
    CHECK(step_register(&STREAM_DEF) == 0);
    CHECK(step_register(&STREAM_CHECK_DEF) == 0);
}

static void reset_counters(void) {
    atomic_store_explicit(&g_double_runs, 0, memory_order_relaxed);
    atomic_store_explicit(&g_triple_runs, 0, memory_order_relaxed);
    atomic_store_explicit(&g_sum_runs, 0, memory_order_relaxed);
}

// ----------------------------------------------------------------------------
// Tests
// ----------------------------------------------------------------------------

static void test_basic_run(void) {
    register_all();
    reset_counters();

    char root[256];
    make_tmpdir(root, sizeof(root));
    store_config_t scfg = {.root_dir = root};
    store_t* store = store_open(&scfg);

    intent_map_t* im = open_test_im(root);
    runner_config_t rcfg = {.store = store, .intent_map = im};
    runner_t* r = runner_new(&rcfg);

    pipeline_t* p = pipeline_new("demo");
    CHECK(pipeline_add_step(p, "double-it", NULL) == 0);
    CHECK(pipeline_add_step(p, "triple-it", NULL) == 0);
    CHECK(pipeline_add_step(p, "sum-two", NULL) == 0);
    CHECK(pipeline_validate(p) == 0);

    int32_t input = 7;
    const char* subs[] = {"total"};
    cb_state_t cbs = {0};
    runner_callbacks_t cb = make_cb(&cbs);

    CHECK(runner_run(r, p, &input, sizeof(input), subs, 1, &cb) == 0);

    CHECK(atomic_load_explicit(&cbs.n_error, memory_order_relaxed) == 0);
    CHECK(atomic_load_explicit(&cbs.n_run_complete, memory_order_relaxed) == 1);
    CHECK(atomic_load_explicit(&cbs.n_plan, memory_order_relaxed) == 3); // double + triple + sum
    CHECK(atomic_load_explicit(&cbs.n_step_complete, memory_order_relaxed) == 3);
    CHECK(atomic_load_explicit(&cbs.n_cache_hits, memory_order_relaxed) == 0);
    CHECK(cbs.got_total);

    CHECK(atomic_load_explicit(&g_double_runs, memory_order_relaxed) == 1);
    CHECK(atomic_load_explicit(&g_triple_runs, memory_order_relaxed) == 1);
    CHECK(atomic_load_explicit(&g_sum_runs, memory_order_relaxed) == 1);

    // Verify "total" = 2*7 + 3*7 = 35
    store_handle_t h;
    CHECK(store_lookup(store, &cbs.last_total_hash, &h) == 0);
    int32_t total;
    memcpy(&total, h.bytes, sizeof(total));
    CHECK(total == 35);
    store_release(store, &h);

    pipeline_free(p);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
}

static void test_cache_hit_on_second_run(void) {
    register_all();
    reset_counters();

    char root[256];
    make_tmpdir(root, sizeof(root));
    store_config_t scfg = {.root_dir = root};
    store_t* store = store_open(&scfg);
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});

    pipeline_t* p = pipeline_new("demo");
    pipeline_add_step(p, "double-it", NULL);
    pipeline_add_step(p, "triple-it", NULL);
    pipeline_add_step(p, "sum-two", NULL);

    int32_t input = 7;
    const char* subs[] = {"total"};

    // First run: all 3 steps execute.
    cb_state_t c1 = {0};
    runner_callbacks_t cb1 = make_cb(&c1);
    CHECK(runner_run(r, p, &input, sizeof(input), subs, 1, &cb1) == 0);
    CHECK(atomic_load_explicit(&c1.n_cache_hits, memory_order_relaxed) == 0);

    // Second run with same input: all 3 steps cache-hit, no exec.
    cb_state_t c2 = {0};
    runner_callbacks_t cb2 = make_cb(&c2);
    reset_counters();
    CHECK(runner_run(r, p, &input, sizeof(input), subs, 1, &cb2) == 0);
    CHECK(atomic_load_explicit(&c2.n_cache_hits, memory_order_relaxed) == 3);
    CHECK(atomic_load_explicit(&g_double_runs, memory_order_relaxed) == 0);
    CHECK(atomic_load_explicit(&g_triple_runs, memory_order_relaxed) == 0);
    CHECK(atomic_load_explicit(&g_sum_runs, memory_order_relaxed) == 0);

    pipeline_free(p);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
}

static void test_dag_prunes_unneeded_steps(void) {
    register_all();
    reset_counters();

    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});

    pipeline_t* p = pipeline_new("demo");
    pipeline_add_step(p, "double-it", NULL);
    pipeline_add_step(p, "triple-it", NULL);
    pipeline_add_step(p, "sum-two", NULL);

    // Subscribe to "doubled" only — should run double-it alone.
    int32_t input = 7;
    const char* subs[] = {"doubled"};
    cb_state_t c = {0};
    runner_callbacks_t cb = make_cb(&c);
    CHECK(runner_run(r, p, &input, sizeof(input), subs, 1, &cb) == 0);
    CHECK(c.n_plan == 1);
    CHECK(atomic_load_explicit(&g_double_runs, memory_order_relaxed) == 1);
    CHECK(atomic_load_explicit(&g_triple_runs, memory_order_relaxed) == 0);
    CHECK(atomic_load_explicit(&g_sum_runs, memory_order_relaxed) == 0);

    pipeline_free(p);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
}

static void test_different_inputs_rerun(void) {
    register_all();
    reset_counters();

    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});

    pipeline_t* p = pipeline_new("demo");
    pipeline_add_step(p, "double-it", NULL);
    pipeline_add_step(p, "triple-it", NULL);
    pipeline_add_step(p, "sum-two", NULL);

    int32_t in1 = 7;
    int32_t in2 = 11;
    const char* subs[] = {"total"};

    cb_state_t c1 = {0};
    runner_callbacks_t cb1 = make_cb(&c1);
    CHECK(runner_run(r, p, &in1, sizeof(in1), subs, 1, &cb1) == 0);

    cb_state_t c2 = {0};
    runner_callbacks_t cb2 = make_cb(&c2);
    reset_counters();
    CHECK(runner_run(r, p, &in2, sizeof(in2), subs, 1, &cb2) == 0);

    // Different input → different pipeline-input hash → different
    // derivation hashes downstream → cache miss everywhere.
    CHECK(atomic_load_explicit(&c2.n_cache_hits, memory_order_relaxed) == 0);
    CHECK(atomic_load_explicit(&g_double_runs, memory_order_relaxed) == 1);

    pipeline_free(p);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
}

static void test_load_pipeline_from_json_string(void) {
    register_all();

    const char* json = "{\n"
                       "  \"name\": \"demo-from-json\",\n"
                       "  \"steps\": [\n"
                       "    {\"step\": \"double-it\"},\n"
                       "    {\"step\": \"triple-it\"},\n"
                       "    {\"step\": \"sum-two\", \"params\": {\"note\": \"hello\"}}\n"
                       "  ]\n"
                       "}\n";

    char err[256] = {0};
    pipeline_t* p = pipeline_load_json_string(json, strlen(json), err, sizeof(err));
    CHECK(p != NULL);
    CHECK(strcmp(pipeline_name(p), "demo-from-json") == 0);
    CHECK(pipeline_step_count(p) == 3);
    CHECK(pipeline_validate(p) == 0);

    // Third step had params; should be re-serialized compact JSON.
    const char* params = pipeline_step_params_at(p, 2);
    CHECK(params != NULL);
    CHECK(strstr(params, "\"note\"") != NULL);
    CHECK(strstr(params, "\"hello\"") != NULL);

    pipeline_free(p);
}

static void test_pipeline_attributions(void) {
    register_all();

    // No `attributions` field → empty.
    {
        const char* json = "{\"name\":\"x\",\"steps\":[{\"step\":\"double-it\"}]}";
        char err[256] = {0};
        pipeline_t* p = pipeline_load_json_string(json, strlen(json), err, sizeof(err));
        CHECK(p != NULL);
        CHECK(pipeline_attribution_count(p) == 0);
        CHECK(pipeline_attribution_at(p, 0) == NULL);
        pipeline_free(p);
    }

    // Two entries: one with url, one without (null url is allowed and means
    // no public homepage — the SDK renders the label as plain text, no link).
    {
        const char* json =
            "{\n"
            "  \"name\": \"x\",\n"
            "  \"attributions\": [\n"
            "    { \"label\": \"IGN\", \"url\": \"https://geoservices.ign.fr/\" },\n"
            "    { \"label\": \"Local\", \"url\": null }\n"
            "  ],\n"
            "  \"steps\": [{\"step\":\"double-it\"}]\n"
            "}\n";
        char err[256] = {0};
        pipeline_t* p = pipeline_load_json_string(json, strlen(json), err, sizeof(err));
        CHECK(p != NULL);
        CHECK(pipeline_attribution_count(p) == 2);

        const pipeline_attribution_t* a0 = pipeline_attribution_at(p, 0);
        CHECK(a0 != NULL);
        CHECK(strcmp(a0->label, "IGN") == 0);
        CHECK(a0->url != NULL && strcmp(a0->url, "https://geoservices.ign.fr/") == 0);

        const pipeline_attribution_t* a1 = pipeline_attribution_at(p, 1);
        CHECK(a1 != NULL);
        CHECK(strcmp(a1->label, "Local") == 0);
        CHECK(a1->url == NULL);

        // Out-of-bounds returns NULL.
        CHECK(pipeline_attribution_at(p, 2) == NULL);

        pipeline_free(p);
    }

    // Malformed: empty label rejected.
    {
        const char* json =
            "{\"name\":\"x\","
            "\"attributions\":[{\"label\":\"\",\"url\":\"x\"}],"
            "\"steps\":[{\"step\":\"double-it\"}]}";
        char err[256] = {0};
        pipeline_t* p = pipeline_load_json_string(json, strlen(json), err, sizeof(err));
        CHECK(p == NULL);
        CHECK(strstr(err, "label") != NULL);
    }

    // Malformed: non-array attributions rejected.
    {
        const char* json =
            "{\"name\":\"x\","
            "\"attributions\":{\"label\":\"x\"},"
            "\"steps\":[{\"step\":\"double-it\"}]}";
        char err[256] = {0};
        pipeline_t* p = pipeline_load_json_string(json, strlen(json), err, sizeof(err));
        CHECK(p == NULL);
        CHECK(strstr(err, "attributions") != NULL);
    }

    // Programmatic builder: many adds force a realloc; views must stay valid.
    {
        pipeline_t* p = pipeline_new("x");
        CHECK(p != NULL);
        for (int i = 0; i < 16; i++) {
            char label[16];
            snprintf(label, sizeof(label), "src%d", i);
            CHECK(pipeline_add_attribution(p, label, NULL) == 0);
        }
        CHECK(pipeline_attribution_count(p) == 16);
        CHECK(strcmp(pipeline_attribution_at(p, 0)->label, "src0") == 0);
        CHECK(strcmp(pipeline_attribution_at(p, 15)->label, "src15") == 0);
        pipeline_free(p);
    }
}

static void test_load_pipeline_json_errors(void) {
    register_all();
    char err[256];

    // Missing "name"
    CHECK(pipeline_load_json_string("{\"steps\":[]}", 12, err, sizeof(err)) == NULL);
    CHECK(strstr(err, "name") != NULL);

    // Missing "steps"
    CHECK(pipeline_load_json_string("{\"name\":\"x\"}", 12, err, sizeof(err)) == NULL);
    CHECK(strstr(err, "steps") != NULL);

    // Unknown step name
    const char* bad = "{\"name\":\"x\",\"steps\":[{\"step\":\"nope\"}]}";
    CHECK(pipeline_load_json_string(bad, strlen(bad), err, sizeof(err)) == NULL);

    // Malformed JSON
    CHECK(pipeline_load_json_string("{ not json", 10, err, sizeof(err)) == NULL);
}

static void test_load_pipeline_from_json_file(void) {
    register_all();

    // The v3/pipelines/smoke/three-step-smoke.json file is committed at
    // repo-relative path; find it via an env var that CMake sets, falling
    // back to a relative path for manual runs.
    const char* pipelines_dir = getenv("V3_PIPELINES_DIR");
    char path[512];
    snprintf(path, sizeof(path), "%s/smoke/three-step-smoke.json",
             pipelines_dir ? pipelines_dir : "../pipelines");

    char err[256] = {0};
    pipeline_t* p = pipeline_load_json_file(path, err, sizeof(err));
    if (!p) {
        fprintf(stderr, "load_json_file error: %s (path=%s)\n", err, path);
    }
    CHECK(p != NULL);
    if (!p)
        return;

    CHECK(strcmp(pipeline_name(p), "three-step-smoke") == 0);
    CHECK(pipeline_step_count(p) == 3);
    CHECK(pipeline_validate(p) == 0);

    // Run it end-to-end to prove a JSON-loaded pipeline actually works.
    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});

    int32_t input = 10;
    const char* subs[] = {"total"};
    cb_state_t cbs = {0};
    runner_callbacks_t cb = make_cb(&cbs);
    reset_counters();
    CHECK(runner_run(r, p, &input, sizeof(input), subs, 1, &cb) == 0);
    CHECK(atomic_load_explicit(&cbs.n_error, memory_order_relaxed) == 0);
    CHECK(cbs.got_total);

    // 2*10 + 3*10 = 50
    store_handle_t h;
    CHECK(store_lookup(store, &cbs.last_total_hash, &h) == 0);
    int32_t total;
    memcpy(&total, h.bytes, sizeof(total));
    CHECK(total == 50);
    store_release(store, &h);

    pipeline_free(p);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
}

static void test_pipeline_validation(void) {
    register_all();

    // Missing producer for a read.
    pipeline_t* p1 = pipeline_new("bad");
    pipeline_add_step(p1, "sum-two", NULL); // reads doubled+tripled; no producers
    CHECK(pipeline_validate(p1) == -1);
    pipeline_free(p1);

    // Duplicate writer.
    // Can't test directly with current defs since each step writes a unique name;
    // duplicate writers would require two steps writing "doubled" etc.
    // Covered by unit tests for pipeline_validate if needed.

    // Unknown step name.
    pipeline_t* p2 = pipeline_new("bad2");
    CHECK(pipeline_add_step(p2, "does-not-exist", NULL) == -1);
    pipeline_free(p2);
}

// ----------------------------------------------------------------------------
// Coverage-gap tests added as part of the audit follow-up.
// ----------------------------------------------------------------------------

struct captured_cb_state {
    hash_t out_hash;
    bool got;
    const char* wanted_name;
};
static void captured_artifact(void* ctx, const char* run_id, const char* name, const hash_t* h) {
    (void)run_id;
    struct captured_cb_state* s = ctx;
    if (strcmp(name, s->wanted_name) == 0) {
        s->out_hash = *h;
        s->got = true;
    }
}

static void test_content_hashed_output(void) {
    register_all();
    atomic_store_explicit(&g_content_runs, 0, memory_order_relaxed);

    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});

    pipeline_t* p = pipeline_new("content-test");
    pipeline_add_step(p, "content-echo", NULL);

    const char* input_bytes = "content-addressed-payload";
    size_t input_len = strlen(input_bytes);
    const char* subs[] = {"content-echoed"};

    struct captured_cb_state cb_state = {.wanted_name = "content-echoed"};
    runner_callbacks_t cb = {
        .ctx = &cb_state,
        .on_artifact_ready = captured_artifact,
    };

    CHECK(runner_run(r, p, input_bytes, input_len, subs, 1, &cb) == 0);
    CHECK(cb_state.got);
    hash_t captured = cb_state.out_hash;

    // The artifact's store key must equal hash_bytes(output bytes), not the
    // step's derivation hash.
    hash_t expected = hash_bytes(input_bytes, input_len);
    CHECK(hash_eq(&captured, &expected));

    // And the store should contain that hash.
    store_handle_t h;
    CHECK(store_lookup(store, &captured, &h) == 0);
    CHECK(h.len == input_len);
    CHECK(memcmp(h.bytes, input_bytes, input_len) == 0);
    store_release(store, &h);

    pipeline_free(p);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
}

static void test_multi_algorithm_step(void) {
    register_all();
    atomic_store_explicit(&g_multi_runs, 0, memory_order_relaxed);

    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});

    int32_t input = 4;
    const char* subs[] = {"algo-result"};

    // First run with algo=square: 4^2 = 16.
    pipeline_t* p_sq = pipeline_new("mas-square");
    pipeline_add_step(p_sq, "multi-algo", "{\"algo\":\"square\"}");
    cb_state_t cbs = {0};
    runner_callbacks_t cb = make_cb(&cbs);
    CHECK(runner_run(r, p_sq, &input, sizeof(input), subs, 1, &cb) == 0);
    CHECK(atomic_load_explicit(&g_multi_runs, memory_order_relaxed) == 1);
    pipeline_free(p_sq);

    // Second run with algo=cube: 4^3 = 64. Different params → different
    // derivation hash → cache miss, step runs again.
    pipeline_t* p_cu = pipeline_new("mas-cube");
    pipeline_add_step(p_cu, "multi-algo", "{\"algo\":\"cube\"}");
    cb_state_t cbs2 = {0};
    runner_callbacks_t cb2 = make_cb(&cbs2);
    CHECK(runner_run(r, p_cu, &input, sizeof(input), subs, 1, &cb2) == 0);
    CHECK(atomic_load_explicit(&g_multi_runs, memory_order_relaxed) == 2);
    pipeline_free(p_cu);

    // Third run: same as first (square). Different pipeline name doesn't
    // affect hash; params+step+reads are the same → cache hit.
    pipeline_t* p_sq2 = pipeline_new("mas-square-again");
    pipeline_add_step(p_sq2, "multi-algo", "{\"algo\":\"square\"}");
    cb_state_t cbs3 = {0};
    runner_callbacks_t cb3 = make_cb(&cbs3);
    CHECK(runner_run(r, p_sq2, &input, sizeof(input), subs, 1, &cb3) == 0);
    CHECK(atomic_load_explicit(&cbs3.n_cache_hits, memory_order_relaxed) == 1);
    // Counter should NOT have advanced — cache hit.
    CHECK(atomic_load_explicit(&g_multi_runs, memory_order_relaxed) == 2);
    pipeline_free(p_sq2);

    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
}

static void test_cycle_detected_by_runner(void) {
    register_all();

    pipeline_t* p = pipeline_new("cycle");
    CHECK(pipeline_add_step(p, "cycle-a", NULL) == 0);
    CHECK(pipeline_add_step(p, "cycle-b", NULL) == 0);
    // pipeline_validate accepts this (every read has a producer), but the
    // runner's topo-sort must fail.
    CHECK(pipeline_validate(p) == 0);

    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});

    int dummy = 0;
    const char* subs[] = {"a-art"};
    cb_state_t cbs = {0};
    runner_callbacks_t cb = make_cb(&cbs);
    CHECK(runner_run(r, p, &dummy, sizeof(dummy), subs, 1, &cb) == -1);
    CHECK(atomic_load_explicit(&cbs.n_error, memory_order_relaxed) == 1);
    CHECK(atomic_load_explicit(&cbs.n_run_complete, memory_order_relaxed) == 0);

    pipeline_free(p);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
}

static void test_duplicate_writer_rejected(void) {
    register_all();

    pipeline_t* p = pipeline_new("dup");
    CHECK(pipeline_add_step(p, "double-it", NULL) == 0);
    CHECK(pipeline_add_step(p, "double-copy", NULL) == 0); // also writes "doubled"
    // pipeline_validate must reject: two writers of "doubled".
    CHECK(pipeline_validate(p) == -1);

    pipeline_free(p);
}

static void test_step_exec_error_mid_run(void) {
    register_all();
    atomic_store_explicit(&g_fail_runs, 0, memory_order_relaxed);

    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});

    pipeline_t* p = pipeline_new("fail");
    pipeline_add_step(p, "always-fails", NULL);
    const char* subs[] = {"fail-result"};
    int32_t dummy = 0;
    cb_state_t cbs = {0};
    runner_callbacks_t cb = make_cb(&cbs);

    CHECK(runner_run(r, p, &dummy, sizeof(dummy), subs, 1, &cb) == -1);
    CHECK(atomic_load_explicit(&cbs.n_error, memory_order_relaxed) == 1);
    CHECK(atomic_load_explicit(&cbs.n_run_complete, memory_order_relaxed) == 0);
    CHECK(atomic_load_explicit(&g_fail_runs, memory_order_relaxed) == 1);
    // Terminal failure report: bare `return -1` → INTERNAL naming the step.
    CHECK(atomic_load_explicit(&cbs.n_run_failed, memory_order_relaxed) == 1);
    CHECK(cbs.fail_code == V3_ERR_INTERNAL);
    CHECK(strcmp(cbs.fail_step, "always-fails") == 0);
    CHECK(strstr(cbs.fail_msg, "always-fails failed without reporting a reason") != NULL);

    // The failing step's output must not be in the store.
    store_stats_t stats;
    store_get_stats(store, &stats);
    // Only the pipeline-input artifact was stored.
    CHECK(stats.entries == 1);

    pipeline_free(p);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
}

// docs/ERRORS.md: step_fail reason + upstream evidence reach on_run_failed.
static void test_step_failure_report(void) {
    register_all();
    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});
    int32_t dummy = 0;

    // 1) Reasoned failure with upstream evidence.
    pipeline_t* p = pipeline_new("reasoned");
    CHECK(pipeline_add_step(p, "fails-with-reason", NULL) == 0);
    const char* subs[] = {"reasoned-result"};
    cb_state_t cbs = {0};
    runner_callbacks_t cb = make_cb(&cbs);
    CHECK(runner_run(r, p, &dummy, sizeof(dummy), subs, 1, &cb) == -1);
    CHECK(atomic_load_explicit(&cbs.n_run_failed, memory_order_relaxed) == 1);
    CHECK(atomic_load_explicit(&cbs.n_run_complete, memory_order_relaxed) == 0);
    CHECK(cbs.fail_code == V3_ERR_UPSTREAM_HTTP);
    CHECK(strcmp(cbs.fail_step, "fails-with-reason") == 0);
    CHECK(strcmp(cbs.fail_msg,
                 "tile index query failed: HTTP 400 from example.test: Unknown namespace") == 0);
    CHECK(strstr(cbs.fail_detail, "\"lat\":48.1") != NULL);
    CHECK(strstr(cbs.fail_detail, "\"upstream\"") != NULL);
    CHECK(strstr(cbs.fail_detail, "\"http_status\":400") != NULL);
    CHECK(strstr(cbs.fail_detail, "\"step_version\":3") != NULL);
    pipeline_free(p);

    // 2) A tolerated upstream failure in a successful step leaves no trace.
    p = pipeline_new("tolerant");
    CHECK(pipeline_add_step(p, "tolerant", NULL) == 0);
    const char* subs2[] = {"tolerant-result"};
    cb_state_t cbs2 = {0};
    runner_callbacks_t cb2 = make_cb(&cbs2);
    CHECK(runner_run(r, p, &dummy, sizeof(dummy), subs2, 1, &cb2) == 0);
    CHECK(atomic_load_explicit(&cbs2.n_run_failed, memory_order_relaxed) == 0);
    CHECK(atomic_load_explicit(&cbs2.n_run_complete, memory_order_relaxed) == 1);
    pipeline_free(p);

    // 3) No sink bound outside a run: recording is a safe no-op.
    CHECK(v3_error_current() == NULL);
    v3_error_set_step(V3_ERR_NO_DATA, NULL, "ignored %d", 1);

    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
}

// ----------------------------------------------------------------------------
// B3.5 hash-model tests: intent-map-driven cache skip, non-determinism
// escape hatch, validate-on-use manifest cascade.
// ----------------------------------------------------------------------------

// On a second run with identical inputs the runner must produce zero
// body_fn executions for every step (all intent-map hits).
static void test_second_run_zero_body_executions(void) {
    register_all();
    reset_counters();

    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root, .hot_budget_bytes = 1024 * 1024});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});

    pipeline_t* p = pipeline_new("demo");
    pipeline_add_step(p, "double-it", NULL);
    pipeline_add_step(p, "triple-it", NULL);
    pipeline_add_step(p, "sum-two", NULL);

    int32_t input = 9;
    const char* subs[] = {"total"};

    cb_state_t c1 = {0};
    runner_callbacks_t cb1 = make_cb(&c1);
    CHECK(runner_run(r, p, &input, sizeof(input), subs, 1, &cb1) == 0);
    CHECK(atomic_load_explicit(&g_double_runs, memory_order_relaxed) == 1);
    CHECK(atomic_load_explicit(&g_triple_runs, memory_order_relaxed) == 1);
    CHECK(atomic_load_explicit(&g_sum_runs, memory_order_relaxed) == 1);

    reset_counters();
    cb_state_t c2 = {0};
    runner_callbacks_t cb2 = make_cb(&c2);
    CHECK(runner_run(r, p, &input, sizeof(input), subs, 1, &cb2) == 0);
    // ZERO body_fn invocations across all steps.
    CHECK(atomic_load_explicit(&g_double_runs, memory_order_relaxed) == 0);
    CHECK(atomic_load_explicit(&g_triple_runs, memory_order_relaxed) == 0);
    CHECK(atomic_load_explicit(&g_sum_runs, memory_order_relaxed) == 0);
    CHECK(atomic_load_explicit(&c2.n_cache_hits, memory_order_relaxed) == 3);

    pipeline_free(p);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
}

// Intent map persists across process restart → second dioramap-processor
// invocation (simulated by close+reopen) must cache-hit without running.
static void test_intent_map_survives_restart(void) {
    register_all();
    reset_counters();

    char root[256];
    make_tmpdir(root, sizeof(root));

    store_config_t scfg = {.root_dir = root, .hot_budget_bytes = 1024 * 1024};
    char im_path[512];
    snprintf(im_path, sizeof(im_path), "%s/intent.log", root);

    int32_t input = 21;
    const char* subs[] = {"total"};

    // Run A.
    {
        store_t* store = store_open(&scfg);
        intent_map_t* im = intent_map_open(im_path);
        runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});
        pipeline_t* p = pipeline_new("demo");
        pipeline_add_step(p, "double-it", NULL);
        pipeline_add_step(p, "triple-it", NULL);
        pipeline_add_step(p, "sum-two", NULL);
        cb_state_t c = {0};
        runner_callbacks_t cb = make_cb(&c);
        CHECK(runner_run(r, p, &input, sizeof(input), subs, 1, &cb) == 0);
        pipeline_free(p);
        runner_free(r);
        intent_map_close(im);
        store_close(store);
    }
    CHECK(atomic_load_explicit(&g_double_runs, memory_order_relaxed) == 1);

    reset_counters();

    // Run B: fresh process-lifecycle. Intent map + warm store must drive a
    // full cache hit.
    {
        store_t* store = store_open(&scfg);
        intent_map_t* im = intent_map_open(im_path);
        runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});
        pipeline_t* p = pipeline_new("demo");
        pipeline_add_step(p, "double-it", NULL);
        pipeline_add_step(p, "triple-it", NULL);
        pipeline_add_step(p, "sum-two", NULL);
        cb_state_t c = {0};
        runner_callbacks_t cb = make_cb(&c);
        CHECK(runner_run(r, p, &input, sizeof(input), subs, 1, &cb) == 0);
        CHECK(atomic_load_explicit(&c.n_cache_hits, memory_order_relaxed) == 3);
        pipeline_free(p);
        runner_free(r);
        intent_map_close(im);
        store_close(store);
    }
    CHECK(atomic_load_explicit(&g_double_runs, memory_order_relaxed) == 0);

    rm_rf(root);
}

// F2 (D6.5 follow-up) — runner self-heals when a downstream step's read
// fails because the upstream content blob was unlinked underneath the
// store (manual rm, container restart with tmpfs, eviction race, etc.).
//
// Setup: run the demo pipeline once so everything caches. Walk the store
// root and unlink every .bin file behind the store's back. Run again —
// the runner should DETECT the desync at the first step_read and
// trigger F2's recovery (purge stale entry → invalidate producer's
// intent → re-execute producer → retry the read).
//
// Pre-fix: this scenario fails with step_failed because the runner
// reports the upstream cache_hit=true (in-memory index lies about
// presence) but downstream step_read returns NULL.
static int unlink_all_bin_under(const char* dir) {
    DIR* d = opendir(dir);
    if (!d) return 0;
    struct dirent* e;
    int count = 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' && (e->d_name[1] == 0 || e->d_name[1] == '.')) continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            count += unlink_all_bin_under(path);
        } else if (S_ISREG(st.st_mode)) {
            size_t nlen = strlen(e->d_name);
            if (nlen > 4 && strcmp(e->d_name + nlen - 4, ".bin") == 0) {
                if (unlink(path) == 0) count++;
            }
        }
    }
    closedir(d);
    return count;
}

static void test_runner_self_heals_on_evicted_content(void) {
    register_all();
    reset_counters();

    char root[256];
    make_tmpdir(root, sizeof(root));
    store_config_t scfg = {.root_dir = root, .hot_budget_bytes = 1024 * 1024};
    char im_path[512];
    snprintf(im_path, sizeof(im_path), "%s/intent.log", root);

    int32_t input = 7;
    const char* subs[] = {"total"};

    // Phase 1: warm cache.
    {
        store_t* store = store_open(&scfg);
        intent_map_t* im = intent_map_open(im_path);
        runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});
        pipeline_t* p = pipeline_new("demo");
        pipeline_add_step(p, "double-it", NULL);
        pipeline_add_step(p, "triple-it", NULL);
        pipeline_add_step(p, "sum-two", NULL);
        cb_state_t c = {0};
        runner_callbacks_t cb = make_cb(&c);
        CHECK(runner_run(r, p, &input, sizeof(input), subs, 1, &cb) == 0);
        pipeline_free(p);
        runner_free(r);
        intent_map_close(im);
        store_close(store);
    }
    CHECK(atomic_load_explicit(&g_double_runs, memory_order_relaxed) == 1);
    reset_counters();

    // Simulate the bug: unlink every .bin file underneath the store
    // root, BEHIND the store's back. The intent.log + run journal stay,
    // so on next open the in-memory index is empty (scan_disk finds no
    // files), but intent_map entries still point at the now-missing
    // content hashes.
    int unlinked = unlink_all_bin_under(root);
    CHECK(unlinked >= 3); // at least 3 .bin files: doubled, tripled, total

    // Phase 2: run again. With the store fresh-opened and the intent
    // map carrying stale entries, the runner SHOULD self-heal:
    //   - first run of double-it cache-misses (intent stale → invalidate
    //     + body re-runs)
    //   - same for triple-it and sum-two
    // OR — depending on timing — the upstream cache-hit reports succeed
    // but step_read fails downstream, triggering F2 recovery.
    // Either path produces correct output. The asserting invariant:
    // the run completes successfully despite the on-disk wipe.
    {
        store_t* store = store_open(&scfg);
        intent_map_t* im = intent_map_open(im_path);
        runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});
        pipeline_t* p = pipeline_new("demo");
        pipeline_add_step(p, "double-it", NULL);
        pipeline_add_step(p, "triple-it", NULL);
        pipeline_add_step(p, "sum-two", NULL);
        cb_state_t c = {0};
        runner_callbacks_t cb = make_cb(&c);
        CHECK(runner_run(r, p, &input, sizeof(input), subs, 1, &cb) == 0);
        // The bodies must have re-executed at least once each — recovery
        // path (whether F2 self-heal or store-reopen self-heal) ran.
        CHECK(atomic_load_explicit(&g_double_runs, memory_order_relaxed) >= 1);
        pipeline_free(p);
        runner_free(r);
        intent_map_close(im);
        store_close(store);
    }

    rm_rf(root);
}

// A step that declares a nondeterministic write bypasses the intent map:
// every runner_run re-executes it, and the intent map never records an entry.
static void test_nondeterministic_step_always_runs(void) {
    register_all();
    atomic_store_explicit(&g_nondet_runs, 0, memory_order_relaxed);

    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root, .hot_budget_bytes = 1024 * 1024});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});

    pipeline_t* p = pipeline_new("nondet");
    pipeline_add_step(p, "nondet-counter-step", NULL);
    int32_t input = 0;
    const char* subs[] = {"nondet-counter"};

    for (int i = 0; i < 5; i++) {
        cb_state_t c = {0};
        runner_callbacks_t cb = make_cb(&c);
        CHECK(runner_run(r, p, &input, sizeof(input), subs, 1, &cb) == 0);
        // nondeterministic → never a cache hit.
        CHECK(atomic_load_explicit(&c.n_cache_hits, memory_order_relaxed) == 0);
    }
    // Step ran 5 times.
    CHECK(atomic_load_explicit(&g_nondet_runs, memory_order_relaxed) == 5);

    // Intent map recorded nothing for this step (bypass).
    intent_map_stats_t stats;
    intent_map_get_stats(im, &stats);
    CHECK(stats.live_entries == 0);

    pipeline_free(p);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
}

// Validate-on-use: evict the content referenced by a cached manifest; the
// next run must invalidate the intent entry, re-execute the step, and
// succeed transparently.
static void test_validate_on_use_recovers_from_eviction(void) {
    register_all();
    reset_counters();

    char root[256];
    make_tmpdir(root, sizeof(root));
    // No hot tier: everything goes to warm so store_evict_to() actually removes it.
    store_t* store = store_open(&(store_config_t){.root_dir = root, .hot_budget_bytes = 0});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});

    pipeline_t* p = pipeline_new("evict");
    pipeline_add_step(p, "double-it", NULL);
    int32_t input = 7;
    const char* subs[] = {"doubled"};

    // Run 1: double-it executes once.
    cb_state_t c1 = {0};
    runner_callbacks_t cb1 = make_cb(&c1);
    CHECK(runner_run(r, p, &input, sizeof(input), subs, 1, &cb1) == 0);
    CHECK(atomic_load_explicit(&g_double_runs, memory_order_relaxed) == 1);

    // Force-evict everything. This wipes the "doubled" content AND the manifest.
    store_evict_to(store, 0);

    // Run 2: cached_compute hits the intent map but resolve_manifest detects
    // the missing content, invalidates, retries, and re-runs the body.
    reset_counters();
    cb_state_t c2 = {0};
    runner_callbacks_t cb2 = make_cb(&c2);
    CHECK(runner_run(r, p, &input, sizeof(input), subs, 1, &cb2) == 0);
    CHECK(atomic_load_explicit(&g_double_runs, memory_order_relaxed) == 1);
    // It's NOT a cache hit from the runner's POV: the intent entry was stale
    // → invalidated → body ran.
    CHECK(atomic_load_explicit(&c2.n_cache_hits, memory_order_relaxed) == 0);

    // Run 3: fresh intent entry, proper cache hit.
    reset_counters();
    cb_state_t c3 = {0};
    runner_callbacks_t cb3 = make_cb(&c3);
    CHECK(runner_run(r, p, &input, sizeof(input), subs, 1, &cb3) == 0);
    CHECK(atomic_load_explicit(&g_double_runs, memory_order_relaxed) == 0);

    pipeline_free(p);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
}

// Different pipelines that produce the same bytes at a "funnel" step must
// converge: downstream cache is shared (HASH-MODEL.md §7.3).
static void test_funnel_convergence(void) {
    register_all();
    reset_counters();

    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root, .hot_budget_bytes = 1024 * 1024});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});

    int32_t input = 11;
    const char* subs[] = {"content-echoed"};

    // Pipeline A: direct content-echo.
    pipeline_t* p1 = pipeline_new("funnel-a");
    pipeline_add_step(p1, "content-echo", NULL);
    cb_state_t c1 = {0};
    runner_callbacks_t cb1 = make_cb(&c1);
    CHECK(runner_run(r, p1, &input, sizeof(input), subs, 1, &cb1) == 0);
    pipeline_free(p1);

    atomic_store_explicit(&g_content_runs, 0, memory_order_relaxed);

    // Pipeline B: another content-echo instance (different pipeline name,
    // same step identity — name doesn't affect identity). The intent map hit
    // should skip exec even though this is a "new" pipeline.
    pipeline_t* p2 = pipeline_new("funnel-b-different-name");
    pipeline_add_step(p2, "content-echo", NULL);
    cb_state_t c2 = {0};
    runner_callbacks_t cb2 = make_cb(&c2);
    CHECK(runner_run(r, p2, &input, sizeof(input), subs, 1, &cb2) == 0);
    pipeline_free(p2);

    // Zero extra executions — full cache hit.
    CHECK(atomic_load_explicit(&g_content_runs, memory_order_relaxed) == 0);
    CHECK(atomic_load_explicit(&c2.n_cache_hits, memory_order_relaxed) == 1);

    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
}

// ----------------------------------------------------------------------------
// B9: streaming primitive coverage.
// ----------------------------------------------------------------------------

// stream-fixture writes 4×1024-byte chunks. A downstream stream-check step
// reads the finalised artifact and stores its length + sentinel bytes for
// comparison. Verifies (a) the on_stream_chunk callback fires once per
// chunk including the final one, (b) the bytes seen by the callback sum
// to the artifact length, (c) downstream sees the concatenated bytes
// exactly.
static void test_streaming_writes_concatenate(void) {
    register_all();

    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root,
                                                   .hot_budget_bytes = 1024 * 1024});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});

    pipeline_t* p = pipeline_new("stream-test");
    pipeline_add_step(p, "stream-fixture", NULL);
    pipeline_add_step(p, "stream-check", NULL);

    cb_state_t cs = {0};
    runner_callbacks_t cb = make_cb(&cs);
    int dummy = 0;
    const char* subs[] = {"stream-checked"};
    atomic_store_explicit(&g_stream_total_len, 0, memory_order_relaxed);
    CHECK(runner_run(r, p, &dummy, sizeof(dummy), subs, 1, &cb) == 0);
    pipeline_free(p);

    // 4 chunks total, exactly one final.
    CHECK(atomic_load_explicit(&cs.n_stream_chunks, memory_order_relaxed) == 4);
    CHECK(atomic_load_explicit(&cs.n_stream_final, memory_order_relaxed) == 1);
    CHECK(atomic_load_explicit(&cs.stream_bytes_seen, memory_order_relaxed) == 4 * 1024);
    // Downstream saw the full concatenated artifact.
    CHECK(atomic_load_explicit(&g_stream_total_len, memory_order_relaxed) == 4 * 1024);
    // First byte is chunk 0 byte 0 (i*31 + k = 0); last byte is chunk 3
    // byte 1023 (3*31 + 1023 = 1116, wrapped to uint8 = 1116 % 256 = 92).
    CHECK(atomic_load_explicit(&g_stream_first_byte, memory_order_relaxed) == 0);
    CHECK(atomic_load_explicit(&g_stream_last_byte, memory_order_relaxed) == (1116u & 0xFFu));

    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
}

// Second run with same input → intent-map cache hit → stream-fixture's
// exec is NOT called again, so on_stream_chunk does NOT fire for the
// cached run. Validates that streaming writes integrate with the hash-
// model cache exactly like normal writes (PLAN-VIEWER-PORT §7.2).
static void test_streaming_caches_like_normal_writes(void) {
    register_all();

    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root,
                                                   .hot_budget_bytes = 1024 * 1024});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});

    int dummy = 0;
    const char* subs[] = {"stream-checked"};

    pipeline_t* p1 = pipeline_new("stream-cache-1");
    pipeline_add_step(p1, "stream-fixture", NULL);
    pipeline_add_step(p1, "stream-check", NULL);
    cb_state_t cs1 = {0};
    runner_callbacks_t cb1 = make_cb(&cs1);
    CHECK(runner_run(r, p1, &dummy, sizeof(dummy), subs, 1, &cb1) == 0);
    pipeline_free(p1);
    CHECK(atomic_load_explicit(&cs1.n_stream_chunks, memory_order_relaxed) == 4);

    // Second identical run — full cache hit.
    pipeline_t* p2 = pipeline_new("stream-cache-2");
    pipeline_add_step(p2, "stream-fixture", NULL);
    pipeline_add_step(p2, "stream-check", NULL);
    cb_state_t cs2 = {0};
    runner_callbacks_t cb2 = make_cb(&cs2);
    CHECK(runner_run(r, p2, &dummy, sizeof(dummy), subs, 1, &cb2) == 0);
    pipeline_free(p2);
    // Cache hit → exec was NOT called → no chunk callbacks fired.
    CHECK(atomic_load_explicit(&cs2.n_stream_chunks, memory_order_relaxed) == 0);
    CHECK(atomic_load_explicit(&cs2.n_cache_hits, memory_order_relaxed) >= 1);

    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
}

// ----------------------------------------------------------------------------

int main(void) {
    test_basic_run();
    test_cache_hit_on_second_run();
    test_dag_prunes_unneeded_steps();
    test_different_inputs_rerun();
    test_load_pipeline_from_json_string();
    test_pipeline_attributions();
    test_load_pipeline_json_errors();
    test_load_pipeline_from_json_file();
    test_pipeline_validation();
    test_content_hashed_output();
    test_multi_algorithm_step();
    test_cycle_detected_by_runner();
    test_duplicate_writer_rejected();
    test_step_exec_error_mid_run();
    test_step_failure_report();

    // B3.5 hash-model regression coverage.
    test_second_run_zero_body_executions();
    test_intent_map_survives_restart();
    test_runner_self_heals_on_evicted_content();
    test_nondeterministic_step_always_runs();
    test_validate_on_use_recovers_from_eviction();
    test_funnel_convergence();

    // B9 streaming primitive coverage.
    test_streaming_writes_concatenate();
    test_streaming_caches_like_normal_writes();

    printf("%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}
