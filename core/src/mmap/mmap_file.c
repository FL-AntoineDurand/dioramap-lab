// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#define _POSIX_C_SOURCE 200809L

#include "v3/mmap_file.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/stat.h>

int mmap_file_open(const char* path, mmap_file_t* out) {
    if (!path || !out)
        return -1;
    memset(out, 0, sizeof(*out));

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;

    struct stat st;
    if (fstat(fd, &st) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        errno = EINVAL;
        return -1;
    }

    size_t len = (size_t)st.st_size;
    void* map = NULL;
    if (len > 0) {
        map = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
        if (map == MAP_FAILED) {
            int saved = errno;
            close(fd);
            errno = saved;
            return -1;
        }
    }

    out->fd = fd;
    out->_map = map;
    out->bytes = (const uint8_t*)map;
    out->len = len;
    return 0;
}

void mmap_file_close(mmap_file_t* f) {
    if (!f)
        return;
    if (f->_map && f->len > 0) {
        munmap(f->_map, f->len);
    }
    if (f->fd >= 0) {
        close(f->fd);
    }
    f->_map = NULL;
    f->bytes = NULL;
    f->len = 0;
    f->fd = -1;
}
