// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#include "v3/batch_ctx.h"
#include "v3/runner.h"
#include "v3/store.h"

#include <__fc_builtin.h>
#include <stddef.h>

static bool subscribe_all(void* ctx, const char* name) { (void)ctx; (void)name; return true; }

int main(void) {
    store_t* s = (store_t*)1; // opaque pointer
    batch_ctx_t* bc = batch_ctx_new(s, "/tmp/audit-batch", subscribe_all, NULL);
    if (bc) {
        runner_callbacks_t cb;
        batch_ctx_install_callbacks(bc, &cb);
        (void)batch_ctx_error_count(bc);
        (void)batch_ctx_n_produced(bc);
        char run_id[32];
        batch_ctx_get_run_id(bc, run_id, sizeof(run_id));
        batch_ctx_free(bc);
    }
    // NULLs
    (void)batch_ctx_new(NULL, "/tmp", subscribe_all, NULL);
    (void)batch_ctx_new(s, NULL, subscribe_all, NULL);
    (void)batch_ctx_new(s, "/tmp", NULL, NULL);
    batch_ctx_free(NULL);
    return 0;
}
