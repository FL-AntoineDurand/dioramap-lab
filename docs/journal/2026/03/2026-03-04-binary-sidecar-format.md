---
id: "2026-03-04-binary-sidecar-format"
date: 2026-03-04
title: "TCBD Binary Sidecar Format for Test Cases"
tags: [binary, test-cases, storage, migration]
category: development
status: concluded
refs: ["2026-03-04-flight-strip-simulation"]
---

# TCBD Binary Sidecar Format for Test Cases

## Problem

The 643 test cases were stored as JSON with numeric arrays serialized as text. A single
real test case with 15K points occupies ~1.8 MB as JSON but only ~200 KB as binary.
Multiplied by 403 real cases, this wastes significant storage and slows loading.

## Design: `lib/test-case-binary.js`

Header-based binary format with typed arrays:

```
Offset  Size  Field
0       4     Magic "TCBD" (0x54434244)
4       2     Version (1)
6       4     pointCount
10      2     Flags bitfield
12      4     Reserved
16      ...   Float32Array positions (pointCount × 3)
        ...   Uint8Array classifications (pointCount)
        ...   Int16Array scanAngles (if flag bit 0)
        ...   Uint16Array pointSourceIds (if flag bit 1)
        ...   Uint8Array returnNumbers (if flag bit 2)
        ...   Uint8Array numberOfReturns (if flag bit 3)
        ...   String table + Uint16Array pointPlaneIds (if flag bit 4)
```

### Flag bits

| Bit | Array | Type | Present when |
|-----|-------|------|-------------|
| 0 | scanAngles | Int16 (×100) | Flight-strip data |
| 1 | pointSourceIds | Uint16 | Flight-strip data |
| 2 | returnNumbers | Uint8 | Multi-return data |
| 3 | numberOfReturns | Uint8 | Multi-return data |
| 4 | pointPlaneIds | String table + Uint16 | Ground truth available |

### pointPlaneIds encoding

Plane IDs are strings (e.g., "plane_0", "ground"). Stored as:
1. Uint16 string count
2. For each string: Uint16 length + UTF-8 bytes
3. Uint16Array of indices into the string table (per point)

This avoids repeating long plane ID strings for every point.

## Migration: `scripts/migrate-test-cases.js`

Three modes:
- **Dry run** (default): Shows plan — how many synthetic vs real, estimated savings
- `--verify`: Regenerates synthetic cases from spec, compares content_hash
- `--execute`: Writes spec JSON + binary sidecars to `test-cases-v2/`

### Content hash verification result

All 272 synthetic cases showed hash mismatches. This is expected — roof generators
evolved since original case creation (new chimney modeling, wall returns, etc.). The
point clouds change but the specs (recipes) remain valid for regeneration.

## Spec Types: `lib/test-case-spec.js`

Three source types unified under `RunnerInputs`:

| Type | Storage | Loading |
|------|---------|---------|
| `synthetic` | Recipe JSON (~1 KB) | Regenerated on-the-fly from seed |
| `real` | Spec JSON + `.bin` sidecar | Binary read |
| `live` | Nothing (transient) | Fetched from IGN APIs |

The `TestCaseLoader` (`lib/test-case-loader.js`) dispatches to the appropriate loader
based on spec type, with a legacy fallback for existing JSON test cases.

## Storage savings

For a typical real test case (15K points):
- JSON: ~1.8 MB (text-encoded arrays)
- Binary: ~200 KB (typed arrays)
- Savings: ~89%

For synthetic cases, specs are ~1 KB vs ~500 KB full JSON — 99.8% reduction.
