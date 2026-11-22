// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#include "v3/run_journal.h"
#include <__fc_builtin.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    run_journal_t* j = run_journal_open("/tmp/audit-runs.sqlite");
    if (!j) return 0;
    run_record_t rec = {0};
    rec.run_id = "abc";
    rec.pipeline_name = "p";
    rec.success = true;
    (void)run_journal_record(j, &rec);
    run_journal_row_t* rows = NULL;
    size_t n = 0;
    (void)run_journal_list(j, 10, 0, NULL, NULL, -1, &rows, &n);
    run_journal_rows_free(rows, n);
    run_journal_prune(j);
    run_journal_close(j);
    run_journal_close(NULL);
    return 0;
}
