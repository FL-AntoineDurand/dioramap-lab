// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Per-pipeline step-param overrides.
//
// Stored under a single localStorage key (`dioramap.lab.overrides`) as
//   { [pipelineName]: { [stepName]: { [paramName]: value, ... }, ... } }
//
// Overrides are "shallow merges on top of the step's base params": only
// the keys present here replace the pipeline's default values. The server
// applies this same merge (see clone_pipeline_with_overrides in server.c).

import { loadPref, savePref } from '../layout/preferences.js';

const KEY = 'overrides';

export class Overrides extends EventTarget {
  constructor() {
    super();
    this.data = loadPref(KEY, {}) || {};
  }

  // Read the override value for (pipeline, step, param). Returns undefined
  // if no override set.
  get(pipeline, step, param) {
    return this.data?.[pipeline]?.[step]?.[param];
  }

  has(pipeline, step, param) {
    const step_obj = this.data?.[pipeline]?.[step];
    return step_obj != null && Object.prototype.hasOwnProperty.call(step_obj, param);
  }

  // Set an override. Pass `undefined` value to clear.
  set(pipeline, step, param, value) {
    if (value === undefined) { this.clear(pipeline, step, param); return; }
    const byPipeline = this.data[pipeline] ??= {};
    const byStep = byPipeline[step] ??= {};
    byStep[param] = value;
    this._persist();
    this.dispatchEvent(new CustomEvent('override-changed', {
      detail: { pipeline, step, param, value, cleared: false },
    }));
  }

  clear(pipeline, step, param) {
    const byStep = this.data?.[pipeline]?.[step];
    if (!byStep || !Object.prototype.hasOwnProperty.call(byStep, param)) return;
    delete byStep[param];
    if (Object.keys(byStep).length === 0) delete this.data[pipeline][step];
    if (Object.keys(this.data[pipeline] ?? {}).length === 0) delete this.data[pipeline];
    this._persist();
    this.dispatchEvent(new CustomEvent('override-changed', {
      detail: { pipeline, step, param, value: undefined, cleared: true },
    }));
  }

  clearPipeline(pipeline) {
    if (!this.data[pipeline]) return;
    delete this.data[pipeline];
    this._persist();
    this.dispatchEvent(new CustomEvent('override-changed', {
      detail: { pipeline, bulk: true, cleared: true },
    }));
  }

  // Counts of overrides for a pipeline (total + steps with overrides).
  summary(pipeline) {
    const byPipeline = this.data?.[pipeline];
    if (!byPipeline) return { count: 0, steps: 0 };
    let count = 0;
    const steps = Object.keys(byPipeline).length;
    for (const s of Object.values(byPipeline)) count += Object.keys(s).length;
    return { count, steps };
  }

  // Emit the shape the RUN message expects: { [stepName]: { ... } }.
  forRunMessage(pipeline) {
    return { ...(this.data?.[pipeline] ?? {}) };
  }

  // Expose the step-level map so renderers can compute effective params.
  forStep(pipeline, step) {
    return this.data?.[pipeline]?.[step] ?? null;
  }

  _persist() { savePref(KEY, this.data); }
}
