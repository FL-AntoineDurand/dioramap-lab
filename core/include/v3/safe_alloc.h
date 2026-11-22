// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Overflow-safe replacements for `malloc(n * sizeof(T))` and similar
// shape allocations. Introduced as part of the Frama-C audit pass 3
// hardening — see docs/quality/README.md "malloc(N*sizeof(T))"
// pattern note for the rationale and the migration history.
//
// The C standard says the result of `n * elem_size` wrapping size_t is
// well-defined (modular wrap) but the resulting allocation is then
// smaller than the caller intended, and the subsequent memcpy/loop
// runs past the allocation → heap buffer overflow. These helpers
// detect the wrap before reaching the allocator and return NULL so
// the caller's existing OOM check fires.
#ifndef V3_SAFE_ALLOC_H
#define V3_SAFE_ALLOC_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

// Equivalent to `malloc(n * elem_size)`, except returns NULL if the
// multiplication would overflow `size_t` (rather than wrapping and
// returning a tiny allocation). When elem_size is 0 the multiplication
// can't overflow; we forward `malloc(0)` which is implementation-
// defined — same behavior as the open-coded form.
/*@ assigns \nothing;
    ensures \result == \null
         || \valid(((char*)\result) + (0 .. n * elem_size - 1));
*/
static inline void* safe_malloc_array(size_t n, size_t elem_size) {
    if (elem_size != 0 && n > (size_t)PTRDIFF_MAX / elem_size) return NULL;
    return malloc(n * elem_size);
}

// Same shape but for calloc. glibc's calloc already checks for the
// n*elem_size overflow (returns NULL on wrap), but POSIX doesn't
// require it, and some legacy / embedded libc implementations don't.
// This wrapper ensures the audit-visible guard regardless of libc.
/*@ assigns \nothing;
    ensures \result == \null
         || (\valid(((char*)\result) + (0 .. n * elem_size - 1))
             && \initialized(((char*)\result) + (0 .. n * elem_size - 1)));
*/
static inline void* safe_calloc_array(size_t n, size_t elem_size) {
    if (elem_size != 0 && n > (size_t)PTRDIFF_MAX / elem_size) return NULL;
    return calloc(n, elem_size);
}

// Equivalent to `realloc(p, n * elem_size)` with the same overflow
// guard. `p` may be NULL (then this behaves like safe_malloc_array).
/*@ assigns *(((char*)p) + (0 .. n * elem_size - 1));
    ensures \result == \null
         || \valid(((char*)\result) + (0 .. n * elem_size - 1));
*/
static inline void* safe_realloc_array(void* p, size_t n, size_t elem_size) {
    if (elem_size != 0 && n > (size_t)PTRDIFF_MAX / elem_size) return NULL;
    return realloc(p, n * elem_size);
}

#ifdef __cplusplus
}
#endif

#endif // V3_SAFE_ALLOC_H
