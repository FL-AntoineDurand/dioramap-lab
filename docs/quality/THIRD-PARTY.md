# Third-party dependency manifest

Every library under `third_party/` is fetched by
`scripts/fetch-third-party.sh` from upstream at a pinned commit, with
inline patches applied at clone time. Nothing under `third_party/` is
tracked in git (only the top-level `.gitignore`).

## The pins

| Dep | Upstream | Pinned ref | Patched? | Disk after clone |
|---|---|---|---|---|
| `copc-lib` | `github.com/RockRobotic/copc-lib.git` | `v2.6.3` | yes — `CMakeLists.txt:91` (`CMAKE_SOURCE_DIR` → `CMAKE_CURRENT_SOURCE_DIR`) | 16 MB |
| `laz-perf` | `github.com/hobu/laz-perf.git` | `14522addcb01125499119990cc8fbc6b1e43b148` | no | (nested in copc-lib) |
| `PolyFit` | `github.com/LiangliangNan/PolyFit.git` | `a30f9bfe3f2e0a200e9892bbfc4c722125fc8daf` | no | 66 MB |
| `cgal` | `github.com/CGAL/cgal.git` | `v6.1.1` (`08b27d3db140`) | yes — `CGALConfig.cmake` (alias-target guard) | 506 MB |
| `roofer` | `github.com/3DBAG/roofer.git` | `570eb95b15cefccb4965d6f02b7bbd382c9f9faa` | yes — `CMakeLists.txt` (`CMAKE_SOURCE_DIR` → `PROJECT_SOURCE_DIR`) | 48 MB |

**Total on disk after fetch: 634 MB.** CGAL alone is 80 % of that (header-only library; never compiled).

Source of truth is the `DEPS` array in `scripts/fetch-third-party.sh`. The table above is informational — bump it when you bump a pin.

## How to fetch (the everyday case)

```bash
./scripts/fetch-third-party.sh           # fetches everything missing; idempotent
./scripts/fetch-third-party.sh --check   # verifies all 5 are at the right SHA
./scripts/fetch-third-party.sh --force   # nuke and re-fetch (use when upgrading)
```

Cold fetch over the network is ~1 min 26 s. Re-runs are instant (< 0.1 s).

## How to bump a pin

1. Find the new upstream commit you want (`git -C third_party/<dep> log` if the dep is already cloned).
2. Update the SHA in the `DEPS` array of `scripts/fetch-third-party.sh`.
3. `./scripts/fetch-third-party.sh --force` (or `rm -rf third_party/<dep>` then `./scripts/fetch-third-party.sh`).
4. `./scripts/ci/run-tier.sh full` to verify nothing broke.
5. Update the table above.

That's it. There's no `.gitmodules` to sync, no separate lockfile.

## How to add a patch

When upstream needs a tweak to work with our build (e.g. a CMake macro mismatch, a header-only include that assumes top-level project, a strict-warning escape):

1. Apply the change by hand in `third_party/<dep>/` to confirm the build works.
2. Capture the change as a `sed`-able transformation (one-liner) or, if it's larger than ~5 lines, generate a `.patch` file via `git diff` against the upstream HEAD.
3. Add an `apply_patches_<dep_name>()` function in `scripts/fetch-third-party.sh`. Guard with `if grep -q '<sentinel>' "$file"; then return 0; fi` so the function is idempotent.
4. `./scripts/fetch-third-party.sh --force` to verify the patch applies cleanly from a clean clone.
5. Update the "Patched?" column above.

Inline `sed` is acceptable for short patches; promote to `scripts/patches/<dep>-<topic>.patch` if the change exceeds a few lines.

## Build-flag gotchas

Two project-wide flags that we explicitly **un-set** on third-party targets:

1. **`-fvisibility=hidden`** (added globally in Release builds — see `core/CMakeLists.txt`). PolyFit's shared libs don't annotate classes with `__attribute__((visibility("default")))`, so under the global flag every `Logger`, `Counted`, `AttributeStore` etc. symbol becomes hidden in `libbasic.so`/`libmath.so`/`libmodel.so`/`libmethod.so`. The override `target_compile_options(... PRIVATE -fvisibility=default)` on PolyFit's targets in `core/CMakeLists.txt` restores them. **Without this, the final `dioramap-processor` link fails with hundreds of undefined-reference errors under `-DDIORAMAP_RESEARCH=ON -DCMAKE_BUILD_TYPE=Release`.**

2. **`-fprofile-instr-generate -fcoverage-mapping`** (added globally when `DIORAMAP_COVERAGE=ON`). Instrumenting every third-party TU bloats the binary's `__llvm_prf_data` section to several MB and llvm-cov-18 effectively hangs trying to process it. The helper `dioramap_opt_out_of_coverage(<target>)` (defined alongside the coverage block in `core/CMakeLists.txt`) adds `-fno-profile-instr-generate -fno-coverage-mapping` to each third-party target. Without this opt-out, `./scripts/ci/run-tier.sh full` never finishes the coverage step.

If a future third-party dep needs to be opted out of coverage (or visibility), add it to the `foreach(_3p_target ...)` loops in `core/CMakeLists.txt` next to the existing copc-lib / lazperf / PolyFit / Roofer entries.

## Why not vendor

We considered committing third-party sources directly. Rejected because:

- CGAL alone is 506 MB. It would dominate the repo's git history.
- Upstream URLs + pinned commits are explicit in one bash file; that's easier to reason about than a giant vendored tree.
- Patches are version-controlled in the script, reviewable line-by-line.
- A fresh CI runner pays a one-time ~1 min 26 s fetch — comparable to a `git clone` of a vendored tree on a slow network.

The escape hatch (for offline builds, or for guaranteeing reproducibility if upstream disappears) is to add a step that uploads the vendored tarball to an internal mirror. Not needed today.

## Lifecycle when removing a dep

1. Remove its add_subdirectory + target_link_libraries calls from `core/CMakeLists.txt`.
2. Remove its entry from the `DEPS` array in `scripts/fetch-third-party.sh`.
3. Remove its `apply_patches_<dep>()` function if any.
4. Remove its line from `third_party/.gitignore`.
5. `rm -rf third_party/<dep>` locally.
6. Update the table above.
