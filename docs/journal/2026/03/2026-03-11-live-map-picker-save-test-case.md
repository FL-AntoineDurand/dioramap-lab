---
id: "2026-03-11-live-map-picker-save-test-case"
date: 2026-03-11
title: "Live map picker fix + save-as-test-case from UI"
tags: [ui, live-mode, test-cases, building-importer]
category: development
status: concluded
refs: ["2026-03-04-live-mode-map-picker"]
---

## Fixes

- Live map picker: `selectedTestCase` was not set after live fetch, so artifact clicks used the previously selected test case instead of the live building.
- `importBuilding()` fixed to use `type:'real'` + `.bin` sidecar format (was `type:'real_lidar'` with inline arrays, which the loader rejected).

## New: save-as-test-case

- "Save as TC" button appears in status bar after a live fetch
- Calls new `/api/save-test-case` endpoint which runs `importBuilding()` server-side
- Ortho fetch included in save flow (non-fatal on failure)
- Test case list auto-refreshes after save

## Server changes

- Cache live building data (max 5 entries) so `/api/artifact` can serve artifacts for live buildings without a JSON file on disk

Files: `building-importer.js`, `ui/public/app.js`, `ui/public/index.html`, `ui/server.js`. 8 files, +389 −28 lines.
