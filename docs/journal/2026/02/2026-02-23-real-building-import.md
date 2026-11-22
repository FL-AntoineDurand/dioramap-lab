---
id: "2026-02-23-real-building-import"
date: 2026-02-23
title: "Real Building Import from AgriMap API"
tags: [real-buildings, import, agrimap-api, leaflet]
category: development
status: concluded
---

# Real Building Import from AgriMap API

- Created `agrimap-importer.js`: `importFromAgriMap()`, `parseLidarBinary()`, `saveTestCase()`
- CLI: `scripts/import-real-building.js` (`--lat 48.15 --lng -2.78 --name dairy-shed`)
- Web UI: `import.html` with Leaflet map (IGN orthophoto tiles), click-to-set, session history
- Real buildings show V2 scores (overshoot, overlap, completeness) without ground truth
