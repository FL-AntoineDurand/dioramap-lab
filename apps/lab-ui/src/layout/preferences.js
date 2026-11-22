// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Tiny namespaced wrapper around localStorage for viewer UI preferences.
// Namespacing lets other apps-on-the-same-origin coexist without collision,
// and failures (private-mode quota, corrupted JSON) degrade silently to the
// provided default — preferences are convenience, never load-bearing.

const NS = 'dioramap.lab.';

export function loadPref(key, fallback) {
  try {
    const raw = localStorage.getItem(NS + key);
    if (raw == null) return fallback;
    const value = JSON.parse(raw);
    // One-shot migration 2026-05-12: viewer.pointSize unit changed from
    // screen pixels (1–10) to world metres (0.02–0.5) when point clouds
    // moved to sizeAttenuation:true. Stale pixel-unit values look like
    // 1+ which would render points as 1+ metre balls. Reset to default.
    if (key === 'viewer.pointSize' && typeof value === 'number' && value > 1) {
      return fallback;
    }
    return value;
  } catch {
    return fallback;
  }
}

export function savePref(key, value) {
  try {
    localStorage.setItem(NS + key, JSON.stringify(value));
  } catch {
    // quota exceeded / disabled / private mode — swallow; preferences are
    // best-effort, not required for correctness.
  }
}

// Convenience for features that want the full bucket of viewer prefs.
export const VIEWER_PREFS = {
  projection: { key: 'viewer.projection', default: 'perspective' },
  pointSize:  { key: 'viewer.pointSize',  default: 0.1 },  // world metres
  axes:       { key: 'viewer.axes',       default: true  },
  grid:       { key: 'viewer.grid',       default: true  },
  compass:    { key: 'viewer.compass',    default: false },
  sky:        { key: 'viewer.sky',        default: true  },
  fog:        { key: 'viewer.fog',        default: false },
};

export function loadViewerPrefs() {
  const out = {};
  for (const [name, spec] of Object.entries(VIEWER_PREFS)) {
    out[name] = loadPref(spec.key, spec.default);
  }
  return out;
}

// Session-level prefs: the last selected pipeline + the last-typed input
// JSON per pipeline. The input map key is the pipeline name; value is
// the raw textarea string so formatting (whitespace, partial edits) is
// preserved across reloads.
export const SESSION_PREFS = {
  selectedPipeline: 'session.selectedPipeline',
  inputByPipeline:  'session.inputByPipeline',
};

export function loadSelectedPipeline() {
  return loadPref(SESSION_PREFS.selectedPipeline, null);
}

export function saveSelectedPipeline(name) {
  savePref(SESSION_PREFS.selectedPipeline, name);
}

export function loadInputForPipeline(pipeline) {
  const map = loadPref(SESSION_PREFS.inputByPipeline, {}) || {};
  return map[pipeline];
}

export function saveInputForPipeline(pipeline, json) {
  const map = loadPref(SESSION_PREFS.inputByPipeline, {}) || {};
  map[pipeline] = json;
  savePref(SESSION_PREFS.inputByPipeline, map);
}
