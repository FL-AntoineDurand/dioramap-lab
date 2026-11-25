# Contributing

Thanks for your interest! Dioramap Lab is a research release maintained on a
best-effort basis, so responses may take a while.

## Ground rules

- **Licence.** Contributions are accepted under the Apache License 2.0
  (inbound = outbound).
- **Developer Certificate of Origin.** Sign off every commit (`git commit -s`) to
  certify the [DCO](https://developercertificate.org/): you wrote the change, or
  you have the right to submit it under the project licence.
- **AI-assisted contributions** are welcome. You are responsible for reviewing and
  understanding every line you submit, as with any other code. Much of this
  codebase was written with AI assistance under the author's direction.

## Workflow

1. Open an issue or a Discussion first for anything non-trivial.
2. Build and test: `cmake -S core -B build -G Ninja && ninja -C build && ctest --test-dir build`.
3. Keep `-Werror` clean. Format C with `clang-format` (`.clang-format` at the root).
4. New or changed algorithms: bump the step's `version`, update its `*.doc.json`,
   and add or adjust tests under `core/tests/`.
5. Research findings and design decisions go in `docs/journal/YYYY/MM/` (see
   `docs/journal/README.md` for the entry format).

## Working with an AI assistant

The research journal (`docs/journal/`, ~190 entries) records what was tried,
measured and decided. `tools/rag/` is a local, CPU-only search over it and the
rest of the docs, meant to be queried by your coding assistant before it
re-derives a past decision:

```bash
python3 tools/rag/build_index.py                       # once, then after pulling
python3 tools/rag/rag_query.py "why are coplanar ridges excluded from make-shared" --format text
```

Setup and the snippet to add to your assistant's instructions:
[tools/rag/README.md](tools/rag/README.md).

## Code layout

See the *Repository layout* section of the README and `docs/GLOSSARY.md`.
