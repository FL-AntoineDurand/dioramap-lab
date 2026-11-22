// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#ifndef V3_MMAP_FILE_H
#define V3_MMAP_FILE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>   // ACSL valid_read_string

#ifdef __cplusplus
extern "C" {
#endif

// Read-only mmap of a regular file. Zero-copy access to artifact bytes via
// `bytes` + `len`. Close when done to unmap and close the fd.
typedef struct mmap_file {
    const uint8_t* bytes;
    size_t len;
    // Private:
    int fd;
    void* _map;
} mmap_file_t;

/*@ requires path == \null || valid_read_string(path);
    requires out == \null || \valid(out);
    assigns *out;
    ensures \result == 0 || \result == -1;
*/
int mmap_file_open(const char* path, mmap_file_t* out);

/*@ requires f == \null || \valid(f);
    assigns *f;
*/
void mmap_file_close(mmap_file_t* f);

#ifdef __cplusplus
}
#endif

#endif
