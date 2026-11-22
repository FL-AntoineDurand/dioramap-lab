// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#include "v3/fetch.h"
#include <__fc_builtin.h>
#include <stddef.h>

int main(void) {
    fetch_client_config_t cfg = {0};
    fetch_client_t* c = fetch_client_new(&cfg);
    if (c) {
        fetch_response_t r;
        (void)fetch_get(c, "https://example.com", &r);
        fetch_stats_t st;
        fetch_client_get_stats(c, &st);
        fetch_client_free(c);
    }
    (void)fetch_client_new(NULL);
    fetch_client_free(NULL);
    return 0;
}
