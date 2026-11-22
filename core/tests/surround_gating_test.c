// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// B10: subscription-gating validation for surround fetchers.
//
// PLAN-VIEWER-PORT §4.2 + §11 B10: `fetch-ortho-surround` and
// `fetch-elevation-surround` sit as leaves in the primary DAG; clients
// that don't subscribe to their outputs must never cause them to fire.
//
// The underlying DAG pruner is covered generically by
// `runner_test.c::test_dag_prunes_unneeded_steps`. This test adds a
// targeted assertion for the REAL surround step definitions, so a future
// refactor that accidentally adds a fan-in edge (e.g. making some
// downstream step read `ortho-surround-jpeg`) would regress here.
//
// The test doesn't hit the network — we feed canonicalize-location a
// deliberately invalid input so it fails at step 1. The runner's
// `on_run_started` callback fires BEFORE any step executes, so we can
// inspect the planned step list without making any HTTP requests.

#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "v3/runner.h"
#include "v3/store.h"

#include "coord-steps.h"
#include "fetchers.h"

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

static void rm_rf(const char* path) {
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    (void)!system(cmd);
}

static void make_tmpdir(char* out, size_t out_sz) {
    snprintf(out, out_sz, "/tmp/dioramap_b10_test_XXXXXX");
    if (mkdtemp(out) == NULL) exit(1);
}

// Captured plan from on_run_started.
typedef struct captured_plan {
    pthread_mutex_t lock;
    char** names;
    size_t n;
} captured_plan_t;

static void cb_started(void* vctx, const char* run_id, const char* pipeline_name,
                       const char* const* plan, size_t n_plan) {
    (void)run_id;
    (void)pipeline_name;
    captured_plan_t* cp = vctx;
    pthread_mutex_lock(&cp->lock);
    cp->names = calloc(n_plan, sizeof(char*));
    cp->n = n_plan;
    for (size_t i = 0; i < n_plan; i++) {
        cp->names[i] = strdup(plan[i]);
    }
    pthread_mutex_unlock(&cp->lock);
}

static bool plan_contains(const captured_plan_t* cp, const char* name) {
    for (size_t i = 0; i < cp->n; i++) {
        if (strcmp(cp->names[i], name) == 0) return true;
    }
    return false;
}

static void captured_plan_free(captured_plan_t* cp) {
    for (size_t i = 0; i < cp->n; i++) free(cp->names[i]);
    free(cp->names);
    cp->names = NULL;
    cp->n = 0;
}

// Build the test pipeline: canonicalize-location + one primary fetcher +
// both surround fetchers. Unpopulated downstream readers are omitted —
// we're testing the leaf-pruning invariant specifically.
static pipeline_t* build_test_pipeline(void) {
    pipeline_t* p = pipeline_new("b10-gating");
    if (pipeline_add_step(p, "canonicalize-location", NULL) != 0) return NULL;
    if (pipeline_add_step(p, "fetch-ortho-rgb", NULL) != 0) return NULL;
    if (pipeline_add_step(p, "fetch-ortho-surround", NULL) != 0) return NULL;
    if (pipeline_add_step(p, "fetch-elevation-surround", NULL) != 0) return NULL;
    return p;
}

// Run a single scenario. Uses bad input JSON so canonicalize-location fails
// at exec time — but on_run_started still captures the planned step list,
// which is what the gating assertion cares about.
static captured_plan_t run_scenario(runner_t* r, const char* const* subscribe, size_t n_sub) {
    pipeline_t* p = build_test_pipeline();
    CHECK(p != NULL);
    captured_plan_t cp = {0};
    pthread_mutex_init(&cp.lock, NULL);
    runner_callbacks_t cb = { .ctx = &cp, .on_run_started = cb_started };
    // Invalid input: non-JSON. canonicalize-location fails to parse,
    // returns -1, run fails before any fetcher dispatches.
    const char* bad_input = "not-json";
    (void)runner_run(r, p, bad_input, strlen(bad_input), subscribe, n_sub, &cb);
    pipeline_free(p);
    return cp;
}

static void test_subscribe_canonical_prunes_all_fetchers(void) {
    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root,
                                                   .hot_budget_bytes = 64 * 1024});
    char im_path[512];
    snprintf(im_path, sizeof(im_path), "%s/intent.log", root);
    intent_map_t* im = intent_map_open(im_path);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});

    const char* subs[] = {"canonical-location"};
    captured_plan_t cp = run_scenario(r, subs, 1);

    // Only canonicalize-location should be planned. All fetchers pruned.
    CHECK(cp.n == 1);
    CHECK(plan_contains(&cp, "canonicalize-location"));
    CHECK(!plan_contains(&cp, "fetch-ortho-rgb"));
    CHECK(!plan_contains(&cp, "fetch-ortho-surround"));
    CHECK(!plan_contains(&cp, "fetch-elevation-surround"));

    captured_plan_free(&cp);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
}

static void test_subscribe_ortho_rgb_prunes_only_surround(void) {
    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root,
                                                   .hot_budget_bytes = 64 * 1024});
    char im_path[512];
    snprintf(im_path, sizeof(im_path), "%s/intent.log", root);
    intent_map_t* im = intent_map_open(im_path);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});

    const char* subs[] = {"ortho-rgb-jpeg"};
    captured_plan_t cp = run_scenario(r, subs, 1);

    // canonicalize-location + fetch-ortho-rgb ONLY. Both surround fetchers
    // pruned because nothing (subscribed) depends on them.
    CHECK(cp.n == 2);
    CHECK(plan_contains(&cp, "canonicalize-location"));
    CHECK(plan_contains(&cp, "fetch-ortho-rgb"));
    CHECK(!plan_contains(&cp, "fetch-ortho-surround"));
    CHECK(!plan_contains(&cp, "fetch-elevation-surround"));

    captured_plan_free(&cp);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
}

static void test_subscribe_surround_ortho_fires_only_that_fetcher(void) {
    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root,
                                                   .hot_budget_bytes = 64 * 1024});
    char im_path[512];
    snprintf(im_path, sizeof(im_path), "%s/intent.log", root);
    intent_map_t* im = intent_map_open(im_path);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});

    const char* subs[] = {"ortho-surround-jpeg"};
    captured_plan_t cp = run_scenario(r, subs, 1);

    // canonicalize-location + fetch-ortho-surround. The primary ortho RGB
    // and the elevation surround are BOTH pruned — they're siblings in
    // the DAG with no reader of the requested output.
    CHECK(cp.n == 2);
    CHECK(plan_contains(&cp, "canonicalize-location"));
    CHECK(plan_contains(&cp, "fetch-ortho-surround"));
    CHECK(!plan_contains(&cp, "fetch-ortho-rgb"));
    CHECK(!plan_contains(&cp, "fetch-elevation-surround"));

    captured_plan_free(&cp);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
}

static void test_subscribe_both_surrounds_fires_both(void) {
    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root,
                                                   .hot_budget_bytes = 64 * 1024});
    char im_path[512];
    snprintf(im_path, sizeof(im_path), "%s/intent.log", root);
    intent_map_t* im = intent_map_open(im_path);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});

    const char* subs[] = {"ortho-surround-jpeg", "elevation-surround-f32"};
    captured_plan_t cp = run_scenario(r, subs, 2);

    // canonicalize-location + both surround fetchers; ortho-rgb still pruned.
    CHECK(cp.n == 3);
    CHECK(plan_contains(&cp, "canonicalize-location"));
    CHECK(plan_contains(&cp, "fetch-ortho-surround"));
    CHECK(plan_contains(&cp, "fetch-elevation-surround"));
    CHECK(!plan_contains(&cp, "fetch-ortho-rgb"));

    captured_plan_free(&cp);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
}

int main(void) {
    step_registry_reset();
    CHECK(coord_steps_register() == 0);
    CHECK(fetchers_register_steps() == 0);

    test_subscribe_canonical_prunes_all_fetchers();
    test_subscribe_ortho_rgb_prunes_only_surround();
    test_subscribe_surround_ortho_fires_only_that_fetcher();
    test_subscribe_both_surrounds_fires_both();

    printf("%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}
