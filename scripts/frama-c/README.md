# Frama-C Audit Runners

Tools that drive the `frama-c-audit` branch's static analysis. **Never compile or run the full `dioramap-processor` binary** (cache + port collisions with concurrent work). Test binaries built by unit-test targets are fine — they're sandboxed and short-lived.

## Files

- `gen-compile-db.sh` — runs `cmake -B build-framac` configure-only, copies `compile_commands.json` to repo root. No `ninja` step is invoked. Idempotent.
- `audit-module.sh <module>` — drives one module through parse → Eva → RTE → report. `<module>` is a key from `modules.tsv` (e.g. `hash`, `refcount`, `intent_map`, `steps/coords`).
- `modules.tsv` — `key<TAB>label<TAB>source-glob<TAB>deps-glob` per module. Single source of truth for what's analyzed under what name.
- `frama-c-stubs.h` — ACSL-friendly stubs for opaque platform / third-party functions that Eva cannot see through (`xxhash`, `pthread`, `proj`, …). Included via `-cpp-extra-args="-include …/frama-c-stubs.h"` when needed.

## Common Frama-C invocation

```bash
frama-c \
  -machdep x86_64 \
  -c11 \
  -no-frama-c-stdlib            # we have our own libc on the system
  -cpp-extra-args="$(pull_flags_from_compile_db <file>)" \
  <file> \
  -eva                          # value analysis
  -eva-precision 2              # 0..11; 2 is fast, 7+ is slow
  -rte                          # generate runtime-error annotations
  -then -report                 # human-readable summary
  -save .frama-c-out/<mod>/<mod>.sav
```

Per-module wrapper hides all that.

## What `audit-module.sh` produces

```
.frama-c-out/<module>/
├── parse.log
├── eva.log
├── rte.log
├── report.txt
└── <module>.sav
```

The curated digest goes to `the per-module ACSL contracts in core/include/v3/`,
new entries roll into `docs/quality/README.md`, and
`docs/quality/README.md` is updated.
