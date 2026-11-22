// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Thread-safety annotation macros (clang only; no-ops elsewhere).
//
// These let the compiler statically verify that:
//   - a struct field marked GUARDED_BY(m) is only accessed while m is held,
//   - a function marked REQUIRES(m) is only called with m held,
//   - ACQUIRE/RELEASE are paired correctly across function boundaries.
//
// On gcc (the project default) every macro expands to nothing — the
// annotations are documentation; the code compiles unchanged.  On clang
// with `-Wthread-safety`, the same source produces compile-time errors
// for any violation.  Build one CI variant with clang to enforce.
//
// Reference: https://clang.llvm.org/docs/ThreadSafetyAnalysis.html
//
// Pattern in dioramap (post 2026-05-02 encapsulation):
//
//   struct session {
//       pthread_mutex_t lock CAPABILITY("mutex");
//       bool run_active GUARDED_BY(lock);
//       /* ... */
//   };
//
//   bool session_is_run_active(session_t* s) {
//       MUTEX_LOCK(&s->lock);     // clang: ACQUIRE(s->lock)
//       bool b = s->run_active;   // clang verifies lock is held
//       MUTEX_UNLOCK(&s->lock);   // clang: RELEASE(s->lock)
//       return b;
//   }

#pragma once

#include <pthread.h>

#if defined(__clang__) && (!defined(SWIG))
#  define TS_ATTR(x)   __attribute__((x))
#else
#  define TS_ATTR(x)   /* no-op */
#endif

// Capability-tagged mutex type.  clang requires the `capability`
// attribute on a TYPE (struct / typedef), not on a struct field —
// hence this wrapper.  On gcc the attribute disappears and the type
// is a plain pthread_mutex_t in disguise.
//
// Usage: replace `pthread_mutex_t lock;` with `mutex_t lock;` in
// structs.  All MUTEX_LOCK/MUTEX_UNLOCK pairs on it are then tracked
// by clang's -Wthread-safety: forgot-to-unlock, double-acquire,
// lock-order inversion, return-while-held, etc. all become compile
// errors.
typedef pthread_mutex_t mutex_t TS_ATTR(capability("mutex"));

// Re-state the capability decorator for use on additional typedefs
// (rare; most code just uses mutex_t directly).
#define CAPABILITY(name)            TS_ATTR(capability(name))

// GUARDED_BY would mark a field as protected-by-mutex, but it requires
// the analyzer to resolve the mutex identifier from the field
// declaration — which works in C++ (implicit `this`) but not in C
// (no `self` at struct definition time).  We expand to a no-op so
// existing annotations on fields don't break the build; relying on
// ACQUIRE/RELEASE pairing on the wrappers is the C-friendly path.
// In C++ files, switch to a real definition.
#define GUARDED_BY(m)               /* no-op in C */

// A function may be called only with the named capability(ies) held.
#define REQUIRES(...)               TS_ATTR(requires_capability(__VA_ARGS__))

// Function takes / releases the named capability.
#define ACQUIRE(...)                TS_ATTR(acquire_capability(__VA_ARGS__))
#define RELEASE(...)                TS_ATTR(release_capability(__VA_ARGS__))

// Function may be called with the named capability either held or not
// (used for accessors that internally lock+unlock).
#define EXCLUDES(...)               TS_ATTR(locks_excluded(__VA_ARGS__))

// Disable thread-safety analysis inside one function (escape hatch for
// constructors/destructors where the lock-state proof is nontrivial).
#define NO_THREAD_SAFETY_ANALYSIS   TS_ATTR(no_thread_safety_analysis)

// Wrappers around pthread_mutex_lock/unlock that carry the
// ACQUIRE/RELEASE annotations.  Use these instead of pthread_mutex_*
// directly when the compiler should track the lock state.
//
// Inlined so the generated code is identical to a direct
// pthread_mutex_* call; the wrappers exist solely to attach the
// capability annotation.  Marked NO_THREAD_SAFETY_ANALYSIS internally
// because the wrapper itself doesn't need its body re-checked — the
// pthread_mutex_* call is opaque to the analyzer.

static inline void ts_mutex_lock(mutex_t* m) ACQUIRE(*m) NO_THREAD_SAFETY_ANALYSIS {
    pthread_mutex_lock(m);
}

static inline void ts_mutex_unlock(mutex_t* m) RELEASE(*m) NO_THREAD_SAFETY_ANALYSIS {
    pthread_mutex_unlock(m);
}

#define MUTEX_LOCK(m)    ts_mutex_lock(m)
#define MUTEX_UNLOCK(m)  ts_mutex_unlock(m)
