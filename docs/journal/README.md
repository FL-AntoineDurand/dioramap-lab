# Research journal

188 dated entries (2026-02 → 2026-07) recording experiments, findings, bug hunts,
designs and abandoned ideas behind the algorithms in this repository. The journal is
published unedited as a lab notebook: negative results and superseded designs are
kept on purpose.

- **Index:** [`_index.md`](_index.md), by category and status.
- **Entry format:** YAML front matter (`id`, `date`, `title`, `tags`, `category`,
  `status`, `refs`) followed by free text.

**Reading notes**

- Early entries (Feb–Apr 2026) describe the **JavaScript prototype** ("v2" bench,
  pipelines v4–v6x), which is not included in this repository. Paths like
  `lib/pipeline/...` or `lidar-algos-bench-v2/...` refer to it. The C
  implementation in `steps/v7/` is the current, canonical one.
- Many entries were written with AI assistance during development sessions.
- Internal terms are defined in [`../GLOSSARY.md`](../GLOSSARY.md).
