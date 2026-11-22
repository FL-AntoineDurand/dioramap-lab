// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>

#include "v3/artifact.h"
#include "v3/mmap_file.h"

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

static char g_tmp_path[] = "/tmp/dioramap_mmap_test_XXXXXX";

static int write_all(int fd, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = write(fd, p, remaining);
        if (n < 0)
            return -1;
        p += n;
        remaining -= (size_t)n;
    }
    return 0;
}

static void test_open_and_read(void) {
    const char* contents = "hello, world!";
    size_t len = strlen(contents);

    int fd = mkstemp(g_tmp_path);
    CHECK(fd >= 0);
    CHECK(write_all(fd, contents, len) == 0);
    close(fd);

    mmap_file_t mf;
    CHECK(mmap_file_open(g_tmp_path, &mf) == 0);
    CHECK(mf.len == len);
    CHECK(memcmp(mf.bytes, contents, len) == 0);
    mmap_file_close(&mf);
    unlink(g_tmp_path);
}

static void test_open_artifact_via_mmap(void) {
    // Build a small artifact, write to a file, mmap it, open with av_open.
    artifact_builder_t* ab = ab_new(AK_PER_POINT_ARRAY, 3);
    const int32_t v[] = {7, 8, 9};
    ab_add_column(ab, "v", DT_I32, v, sizeof(v));
    uint8_t* bytes;
    size_t len;
    hash_t h;
    CHECK(ab_finalize(ab, &bytes, &len, &h) == 0);
    ab_free(ab);

    char path[] = "/tmp/dioramap_mmap_art_XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    CHECK(write_all(fd, bytes, len) == 0);
    close(fd);
    free(bytes);

    mmap_file_t mf;
    CHECK(mmap_file_open(path, &mf) == 0);

    artifact_view_t view;
    CHECK(av_open(mf.bytes, mf.len, &view) == 0);

    size_t nb = 0;
    const int32_t* col = av_column(&view, "v", &nb);
    CHECK(col != NULL);
    CHECK(nb == sizeof(v));
    CHECK(memcmp(col, v, nb) == 0);

    // Round-trip hash matches.
    hash_t h2 = av_hash(&view);
    CHECK(hash_eq(&h, &h2));

    mmap_file_close(&mf);
    unlink(path);
}

static void test_missing_file_fails(void) {
    mmap_file_t mf;
    CHECK(mmap_file_open("/tmp/dioramap_definitely_not_a_real_file", &mf) == -1);
}

int main(void) {
    test_open_and_read();
    test_open_artifact_via_mmap();
    test_missing_file_fails();

    printf("%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}
