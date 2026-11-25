#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
"""Golden-query regression net for the journal/docs RAG (BUILD-PLAN.md §7).

Each case asserts that at least one known-correct entry shows up within top-k
for a query we KNOW is answered in the corpus. Tolerant by design (accepts any
of a small acceptable set) so it catches real retrieval regressions without
being flaky about exact rank-1 ordering. Exits non-zero if any case fails.
"""

import sys

import common
import rag_query

# (label, query, keywords, top_k, {acceptable journal_id / doc path / commit sha})
GOLDEN = [
    ("coplanar-import is a bug?",
     "is the coplanar-import a bug", "", 8,
     {"2026-06-24-coplanar-import-plane-ids-audit"}),

    ("make-shared cascade",
     "what is the make-shared cascade", "", 8,
     {"2026-06-20-in-loop-make-shared-cascade-complete",
      "2026-06-14-in-loop-make-shared-design"}),

    ("DOF follows topology not proximity",
     "DOF follows topology not proximity", "", 8,
     {"2026-06-20-in-loop-make-shared-cascade-complete",
      "2026-06-14-in-loop-make-shared-design",
      "2026-06-11-merge-regularize-convergence-pathb"}),

    ("planes_dyn single source of truth",
     "planes_dyn deleted single source of truth", "planes_dyn", 8,
     {"2026-06-14-in-loop-make-shared-design",
      "2026-06-20-in-loop-make-shared-cascade-complete",
      "2026-06-11-merge-regularize-convergence-pathb"}),

    ("flashing->coplanar ridge rename",
     "why was the flashing ridge type renamed to coplanar", "", 8,
     {"2026-06-11-merge-regularize-convergence-pathb",
      "2026-06-20-in-loop-make-shared-cascade-complete",
      "docs/plan/ROOF-GRAPH-FORMAT.md"}),   # its type table says 'Formerly "flashing"'

    ("exact identifier (BM25 path)",
     "ridge type coplanar regularize topology", "RIDGE_TYPE_COPLANAR", 8,
     {"2026-06-11-merge-regularize-convergence-pathb",
      "3a6bf19"}),
]


def run(db_path):
    passed = 0
    for label, q, kw, k, accept in GOLDEN:
        keywords = kw.split() if kw else []
        out = rag_query.query(db_path, q, keywords, k, None)
        hits = [(r["rank"], r["journal_id"] or r["path"]) for r in out["results"]
                if (r["journal_id"] or r["path"]) in accept]
        ok = bool(hits)
        passed += ok
        mark = "PASS" if ok else "FAIL"
        where = f"rank {hits[0][0]} ({hits[0][1]})" if hits else \
            "not in top-%d" % k
        print(f"  [{mark}] {label:38s} -> {where}")
        if not ok:
            got = ", ".join((r["journal_id"] or r["path"])
                            for r in out["results"][:5])
            print(f"         top-5 were: {got}")
    total = len(GOLDEN)
    print(f"\n{passed}/{total} golden queries passed")
    return passed == total


def main():
    common.reexec_into_venv()
    db_path = sys.argv[1] if len(sys.argv) > 1 else common.DEFAULT_DB
    print(f"Validating {db_path}\n")
    sys.exit(0 if run(db_path) else 1)


if __name__ == "__main__":
    main()
