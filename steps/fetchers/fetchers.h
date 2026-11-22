// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#ifndef V3_STEPS_FETCHERS_H
#define V3_STEPS_FETCHERS_H

#include "v3/step.h"

extern const step_def_t fetch_ortho_rgb_def;
extern const step_def_t fetch_ortho_irc_def;
extern const step_def_t fetch_elevation_def;
extern const step_def_t fetch_cadastre_parcels_def;
extern const step_def_t fetch_cadastre_buildings_def;
extern const step_def_t fetch_ortho_surround_def;
extern const step_def_t fetch_elevation_surround_def;

// Register all fetcher steps. Each step internally resolves a fetch_client_t
// via a process-global handle (created on first call, destroyed on
// fetchers_shutdown). The runner and main.c are unchanged — fetchers look
// like any other step from their perspective.
int fetchers_register_steps(void);

// Release the shared fetch client on process teardown. Idempotent.
void fetchers_shutdown(void);

#endif
