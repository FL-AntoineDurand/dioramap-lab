# v7 model validity tests

The "does the v7 pipeline produce a valid 3D model?" test layer.
Different from `core/tests/v7_*_test.c` (which test implementation
correctness — `docs/quality/V7-TEST-STRATEGY.md` §3).

This layer drives the v7 pipeline against synthetic point clouds with
analytically known ground truth (bench-v2's roof-type generators),
scores the v3 output on metrics defined in `docs/quality/V7-TEST-STRATEGY.md`
§4, and emits a scorecard JSON.

## How to run

```bash
./tests/v7-model-validity/run.sh easy
./tests/v7-model-validity/run.sh standard
./tests/v7-model-validity/run.sh hard
./tests/v7-model-validity/run.sh all
```

Per-tier case lists live in `cases/<tier>.txt` (one case id per line, refers
to a JSON spec in `../../../data/test-cases/<id>.json`).

Outputs:
- `/tmp/v7mv-run-<ts>/scorecard.json`  — machine-readable per-case results
- `/tmp/v7mv-run-<ts>/<case-id>/`     — materialized input + v3 output

When you're happy with a tier's scorecard, copy it to
`baselines/<tier>.json` and commit. Subsequent runs diff against the
committed baseline.

## Current status — B.1 skeleton, paused

- Cases: `easy.txt` contains 1 entry — `flat-default`.
- Metrics: 1 implemented — `plane_count_match`.
- Baseline: none committed.  First run on the `easy` tier prints the
  metric result without diffing against a baseline.

**B.2 onward is paused** until the v3 v7 port matches bench-v2 on real
inputs.  Scoring algorithm-level model validity on a half-ported
pipeline would conflate "v3 differs from v2 because the port is
incomplete" with "the algorithm produces a bad model" — both are
real, but they need to be debugged separately.  See
`docs/quality/V7-TEST-STRATEGY.md` §6.2 for the resume conditions.

The B.1 skeleton stays in place; B.2+ resume by extending it.

## What this is NOT

- Not a regression net against bench-v2 (those baselines are
  port-during-debug, not validated). The oracle is the bench-v2
  generator's analytic ground truth, not bench-v2's output.
- Not a per-PR gate. Lives at the `release` tier (when wired) +
  manual local invocation.
- Not implementation tests. If something fails here, the cause is
  usually algorithm quality, not C code bugs.
