// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#ifndef V3_STEPS_BUILDINGS_H
#define V3_STEPS_BUILDINGS_H

#include "v3/step.h"

// Phase 1 of the roof-recon evaluation effort. Slices the disc-wide point
// cloud + cadastre footprints into per-building records consumable by
// each candidate roof-reconstruction pipeline (PolyFit, KSR, Roofer, V7).
// See v3/docs/research/roof-recon-survey.md.

extern const step_def_t extract_buildings_def;

int buildings_register_steps(void);

#endif
