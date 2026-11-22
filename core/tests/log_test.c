// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v3/log.h"

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

#define CHECK_CONTAINS(haystack, needle)                                                           \
    do {                                                                                           \
        g_checks++;                                                                                \
        const char* _h = (haystack);                                                               \
        const char* _n = (needle);                                                                 \
        if (!_h || !strstr(_h, _n)) {                                                              \
            fprintf(stderr, "FAIL %s:%d: output did not contain %s\n", __FILE__, __LINE__, _n);    \
            if (_h) {                                                                              \
                fprintf(stderr, "  got: %s\n", _h);                                                \
            }                                                                                      \
            g_failed++;                                                                            \
        }                                                                                          \
    } while (0)

#define CHECK_NOT_CONTAINS(haystack, needle)                                                       \
    do {                                                                                           \
        g_checks++;                                                                                \
        const char* _h = (haystack);                                                               \
        const char* _n = (needle);                                                                 \
        if (_h && strstr(_h, _n)) {                                                                \
            fprintf(stderr, "FAIL %s:%d: output contained %s (should not)\n", __FILE__, __LINE__,  \
                    _n);                                                                           \
            fprintf(stderr, "  got: %s\n", _h);                                                    \
            g_failed++;                                                                            \
        }                                                                                          \
    } while (0)

// ----------------------------------------------------------------------------

static void test_basic_event(void) {
    log_sink_t* buf = log_sink_new_buffer();
    log_sink_t* old = log_sink_swap_for_testing(buf);
    log_set_level(LOG_DEBUG);

    LOG_EVENT(SUB_LOG, LOG_INFO, EV_LOG_STARTUP, LF_UINT("pid", 12345), LF_STR("version", "0.0.0"));
    log_flush();
    const char* out = log_sink_buffer_contents(buf);
    CHECK_CONTAINS(out, "\"subsystem\":\"log\"");
    CHECK_CONTAINS(out, "\"event\":\"startup\"");
    CHECK_CONTAINS(out, "\"level\":\"info\"");
    CHECK_CONTAINS(out, "\"pid\":12345");
    CHECK_CONTAINS(out, "\"version\":\"0.0.0\"");
    CHECK_CONTAINS(out, "\"t\":\"");
    CHECK_CONTAINS(out, "\"mono_ns\":");

    log_sink_swap_for_testing(old);
    log_sink_free(buf);
}

static void test_level_filter(void) {
    log_sink_t* buf = log_sink_new_buffer();
    log_sink_t* old = log_sink_swap_for_testing(buf);
    log_set_level(LOG_WARN);

    LOG_EVENT(SUB_LOG, LOG_DEBUG, EV_LOG_STARTUP, LF_INT("x", 1));
    LOG_EVENT(SUB_LOG, LOG_INFO, EV_LOG_STARTUP, LF_INT("x", 2));
    LOG_EVENT(SUB_LOG, LOG_WARN, EV_LOG_STARTUP, LF_INT("x", 3));
    LOG_EVENT(SUB_LOG, LOG_ERROR, EV_LOG_STARTUP, LF_INT("x", 4));

    log_flush();
    const char* out = log_sink_buffer_contents(buf);
    CHECK_NOT_CONTAINS(out, "\"x\":1");
    CHECK_NOT_CONTAINS(out, "\"x\":2");
    CHECK_CONTAINS(out, "\"x\":3");
    CHECK_CONTAINS(out, "\"x\":4");

    log_sink_swap_for_testing(old);
    log_sink_free(buf);
}

static void test_json_escaping(void) {
    log_sink_t* buf = log_sink_new_buffer();
    log_sink_t* old = log_sink_swap_for_testing(buf);
    log_set_level(LOG_DEBUG);

    LOG_EVENT(SUB_LOG, LOG_INFO, EV_LOG_STARTUP, LF_STR("weird", "q\"b\\nl\nct\t"));

    log_flush();
    const char* out = log_sink_buffer_contents(buf);
    CHECK_CONTAINS(out, "q\\\"b\\\\nl\\nct\\t");

    log_sink_swap_for_testing(old);
    log_sink_free(buf);
}

static void test_control_char_escaping(void) {
    log_sink_t* buf = log_sink_new_buffer();
    log_sink_t* old = log_sink_swap_for_testing(buf);
    log_set_level(LOG_DEBUG);

    LOG_EVENT(SUB_LOG, LOG_INFO, EV_LOG_STARTUP, LF_STR("ctrl", "\x01\x02\x1f"));

    log_flush();
    const char* out = log_sink_buffer_contents(buf);
    CHECK_CONTAINS(out, "\\u0001");
    CHECK_CONTAINS(out, "\\u0002");
    CHECK_CONTAINS(out, "\\u001f");

    log_sink_swap_for_testing(old);
    log_sink_free(buf);
}

static void test_no_fields(void) {
    log_sink_t* buf = log_sink_new_buffer();
    log_sink_t* old = log_sink_swap_for_testing(buf);
    log_set_level(LOG_DEBUG);

    LOG_EVENT0(SUB_LOG, LOG_INFO, EV_LOG_SHUTDOWN);

    log_flush();
    const char* out = log_sink_buffer_contents(buf);
    CHECK_CONTAINS(out, "\"event\":\"shutdown\"");
    // Must be valid JSON: no trailing comma before closing brace.
    CHECK_NOT_CONTAINS(out, ",}");

    log_sink_swap_for_testing(old);
    log_sink_free(buf);
}

static void test_all_value_types(void) {
    log_sink_t* buf = log_sink_new_buffer();
    log_sink_t* old = log_sink_swap_for_testing(buf);
    log_set_level(LOG_DEBUG);

    LOG_EVENT(SUB_LOG, LOG_INFO, EV_LOG_STARTUP, LF_INT("i", -42), LF_UINT("u", 100000),
              LF_FLOAT("f", 3.14), LF_STR("s", "hello"), LF_BOOL("b_true", true),
              LF_BOOL("b_false", false));

    log_flush();
    const char* out = log_sink_buffer_contents(buf);
    CHECK_CONTAINS(out, "\"i\":-42");
    CHECK_CONTAINS(out, "\"u\":100000");
    CHECK_CONTAINS(out, "\"f\":3.14");
    CHECK_CONTAINS(out, "\"s\":\"hello\"");
    CHECK_CONTAINS(out, "\"b_true\":true");
    CHECK_CONTAINS(out, "\"b_false\":false");

    log_sink_swap_for_testing(old);
    log_sink_free(buf);
}

static void test_null_string(void) {
    log_sink_t* buf = log_sink_new_buffer();
    log_sink_t* old = log_sink_swap_for_testing(buf);
    log_set_level(LOG_DEBUG);

    LOG_EVENT(SUB_LOG, LOG_INFO, EV_LOG_STARTUP, LF_STR("nothing", NULL));

    log_flush();
    const char* out = log_sink_buffer_contents(buf);
    CHECK_CONTAINS(out, "\"nothing\":null");

    log_sink_swap_for_testing(old);
    log_sink_free(buf);
}

static void test_level_from_env(void) {
    // Simulate reading env var.
    setenv("DIORAMAP_LOG_LEVEL", "warn", 1);
    log_init();
    CHECK(log_get_level() == LOG_WARN);

    setenv("DIORAMAP_LOG_LEVEL", "DEBUG", 1);
    log_init();
    CHECK(log_get_level() == LOG_DEBUG);

    setenv("DIORAMAP_LOG_LEVEL", "bogus", 1);
    log_init();
    CHECK(log_get_level() == LOG_INFO);

    unsetenv("DIORAMAP_LOG_LEVEL");
    log_init();
    CHECK(log_get_level() == LOG_INFO);
}

static void test_subsystem_and_event_names(void) {
    log_sink_t* buf = log_sink_new_buffer();
    log_sink_t* old = log_sink_swap_for_testing(buf);
    log_set_level(LOG_DEBUG);

    LOG_EVENT0(SUB_RUNNER, LOG_INFO, EV_LOG_STARTUP);
    LOG_EVENT0(SUB_STORE, LOG_INFO, EV_LOG_STARTUP);
    LOG_EVENT0(SUB_FETCH, LOG_INFO, EV_LOG_STARTUP);

    log_flush();
    const char* out = log_sink_buffer_contents(buf);
    CHECK_CONTAINS(out, "\"subsystem\":\"runner\"");
    CHECK_CONTAINS(out, "\"subsystem\":\"store\"");
    CHECK_CONTAINS(out, "\"subsystem\":\"fetch\"");

    log_sink_swap_for_testing(old);
    log_sink_free(buf);
}

// ----------------------------------------------------------------------------
// Concurrent emission: exercises the ring buffer under contention.
// ----------------------------------------------------------------------------

#include <pthread.h>

#define EMIT_THREADS 4
#define EMITS_PER_THREAD 500

static void* emit_thread(void* arg) {
    int tid = *(int*)arg;
    for (int i = 0; i < EMITS_PER_THREAD; i++) {
        LOG_EVENT(SUB_RUNNER, LOG_INFO, EV_LOG_STARTUP, LF_INT("tid", tid), LF_INT("i", i));
    }
    return NULL;
}

static void test_concurrent_emit(void) {
    log_sink_t* buf = log_sink_new_buffer();
    log_sink_t* old = log_sink_swap_for_testing(buf);
    log_set_level(LOG_DEBUG);

    pthread_t threads[EMIT_THREADS];
    int ids[EMIT_THREADS];
    for (int i = 0; i < EMIT_THREADS; i++) {
        ids[i] = i;
        CHECK(pthread_create(&threads[i], NULL, emit_thread, &ids[i]) == 0);
    }
    for (int i = 0; i < EMIT_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    log_flush();

    // Count occurrences of "subsystem":"runner" to confirm all events made it through.
    const char* out = log_sink_buffer_contents(buf);
    int count = 0;
    const char* needle = "\"subsystem\":\"runner\"";
    const char* p = out;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += strlen(needle);
    }
    CHECK(count == EMIT_THREADS * EMITS_PER_THREAD);

    log_sink_swap_for_testing(old);
    log_sink_free(buf);
}

// ----------------------------------------------------------------------------

int main(void) {
    log_init();

    test_basic_event();
    test_level_filter();
    test_json_escaping();
    test_control_char_escaping();
    test_no_fields();
    test_all_value_types();
    test_null_string();
    test_level_from_env();
    test_subsystem_and_event_names();
    test_concurrent_emit();

    log_shutdown();

    printf("%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}
