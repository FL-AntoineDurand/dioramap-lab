---
id: "2026-03-10-wecs-diagnostic-gt-validation-reset"
date: 2026-03-10
title: "WECS demoted to diagnostic, all GT validation reset"
tags: [co-register, wecs, ground-truth, validation]
category: finding
status: concluded
refs: ["2026-03-09-boundary-co-registration"]
---

## WECS rejection gate removed

WECS (Wall-Edge Coincidence Score) was rejecting valid RMS-based shifts on large buildings where pre-alignment WECS was already near-ceiling (0.995) — the `searchRadius` covered the misalignment so WECS couldn't detect improvement.

Since eave/MR candidate filtering means WECS and RMS cannot meaningfully disagree, WECS is now computed for diagnostics only (`wecs-before`/`wecs-after` artifacts) and no longer gates acceptance.

Files: `co-register.js` (42 lines changed), `benchmark.test.js`.

## GT validation reset

All 87 previously-validated test cases set to `validated=false`. Reason: flight-strip generator changed — point clouds differ, GT needs re-validation.

Also fixed a race condition: validate GT button disappeared after pipeline auto-run because `loadPreview` response was discarded (stale generation) but `runPipeline` never called `updateValidateBtn`. Added the call in the `runPipeline` success path.

Files: 87 test case JSON files (1-line change each), `ui/public/app.js`.
