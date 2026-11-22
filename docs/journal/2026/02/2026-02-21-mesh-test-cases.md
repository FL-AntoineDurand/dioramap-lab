---
id: "2026-02-21-mesh-test-cases"
date: 2026-02-21
title: "Real Building Test Cases: Mesh-to-Test-Case Pipeline"
tags: [test-cases, mesh, procedural, kenney]
category: development
status: concluded
---

# Real Building Test Cases: Mesh-to-Test-Case Pipeline

## Pipeline

Built mesh-to-test-case pipeline with four components:

- `stl-parser.js` -- STL mesh parsing
- `obj-parser.js` -- OBJ mesh parsing
- `plane-extractor.js` -- extract planes from mesh faces
- `test-case-builder.js` -- assemble parsed geometry into benchmark test cases

## Procedural Generators

15 procedural building generators covering diverse roof types: gambrel, A-frame, church, butterfly, hexagonal, silo, dormer, and more.

## 3D Model Sources

Downloaded 3D models from:

- Kenney Building Kit (80 models)
- Kenney City Kit Suburban (21 models)
- ladybug-tools (2 models)

## Test Suite Expansion

**Test suite: 80 to 305 cases (3.8x), 25 distinct roof types, 1-46 planes per building.**
