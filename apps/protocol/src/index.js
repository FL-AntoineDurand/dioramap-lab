// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// @dioramap/protocol — public re-exports.
//
// Two consumers (lab-ui, viewer-sdk) import from this package via
// `file:` deps. See PLAN-VIEWER-SDK-PORT §3 / §5.8 for the contract:
// `sideEffects: false` is set in package.json so tree-shaking can
// eliminate unused exports in consumer bundles.

export { WSClient } from './ws-client.js';
export { AK, decodeArtifact } from './artifact-decoder.js';
export {
  DioramapError, DEFAULT_ISSUE_REPO,
  errorFromRunFailed, errorFromServerError, errorFromDisconnect, errorFromWatchdog,
  formatReport, issueUrl,
} from './errors.js';
