# Data licence

The test cases in `data/test-cases/` are extracts and derivatives of public datasets
published by the **Institut national de l'information géographique et forestière
(IGN)**:

| Dataset | Use in this repo |
|---|---|
| **LiDAR HD** (classified point clouds, COPC) | `real-*.bin` point extracts |
| **BD ORTHO** (orthophotos) | `real-*.ortho.png` |
| **BD TOPO** (building attributes) | `bdtopoMetadata` in `real-*.json` |
| **Parcellaire Express / cadastre** (building footprints) | footprints in `real-*.json` |

Source: IGN, *LiDAR HD, BD ORTHO, BD TOPO, Parcellaire Express*, extracted 2026-03.
These datasets are distributed under the
[Licence Ouverte / Open Licence 2.0 (Etalab)](https://www.etalab.gouv.fr/licence-ouverte-open-licence/),
which permits reuse, including commercial reuse, provided the source and the date
of last update are cited.

At run time the processor fetches the same datasets from the IGN Géoplateforme
(`data.geopf.fr`). Outputs derived from them carry the same attribution requirement.
The viewer displays it automatically from each pipeline's `attributions` field.

The code in this repository is licensed separately under Apache-2.0 (see `LICENSE`).
