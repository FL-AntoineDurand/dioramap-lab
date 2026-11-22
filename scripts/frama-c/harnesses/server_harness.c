// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#include "v3/server.h"
#include <__fc_builtin.h>
#include <stddef.h>

int main(void) {
    server_config_t cfg = {0};
    cfg.port = 9999;
    server_t* s = server_new(&cfg);
    if (s) {
        // server_run blocks — we don't call it, only the lifecycle pieces.
        server_stop(s);
        server_free(s);
    }
    server_free(NULL);
    server_stop(NULL);
    return 0;
}
