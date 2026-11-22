// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: model-score-postwall — identical wire layout to
// model-score (both are V7_SCORE_INTRINSIC_RECORD_BYTES, packed via
// v7_score_intrinsic_pack), so the rendering is identical. We just
// re-export the named render from the model-score visualizer.
//
// The pipeline-level distinction matters: model-score (from
// v7-score-intrinsic on grm-*) is the pre-wall-detect roof score;
// model-score-postwall (from v7-score-intrinsic-postwall on the post-
// wall-detect roof-graph + wd-owner) is the full post-wall model score.
// Reading both side-by-side in the lab-ui inspector — same card layout,
// labelled by artifact — makes the wall-detect delta legible.

export { render } from './model-score.viz.js';
