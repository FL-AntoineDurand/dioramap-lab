# Ubuntu 24.04 → 26.04 Upgrade (dev + prod containers)

Status: implemented in this branch. Both images upgraded to `ubuntu:26.04`
and built-and-verified against the real 26.04 base image (digest
`sha256:f3d28607ddd78734bb7f71f117f3c6706c666b8b76cbff7c9ff6e5718d46ff64`).

All version/package facts below were probed empirically against the pulled
`ubuntu:26.04` image on 2026-05-27, not assumed.

---

## 1. Base image — CONFIRMED

`ubuntu:26.04` exists on Docker Hub and pulls cleanly.

| Field | Value |
|-------|-------|
| `PRETTY_NAME` | Ubuntu 26.04 LTS |
| `VERSION` | 26.04 (Resolute Raccoon) |
| `VERSION_CODENAME` / `UBUNTU_CODENAME` | `resolute` |
| arch | amd64 |
| default `ubuntu` user | uid=1000, gid=1000 (same as 24.04) |

The uid/gid match is the linchpin of the painless dev-image switch (see §5):
the existing home volume is owned by 1000:1000 and re-attaches with no chown.

---

## 2. Toolchain versions on 26.04 — CONFIRMED (claims were correct)

Probed via `apt-cache policy` on the real image.

| Tool | 24.04 (old) | 26.04 (confirmed) | Notes |
|------|-------------|-------------------|-------|
| gcc / g++ | 13 | **15.2.0** (`4:15.2.0-5ubuntu1`) | builder uses `build-essential` (unversioned) → 15 |
| clang / clang-format / clang-tidy | 18 | **21** (`1:21.1.x`) | unversioned metapackages → 21 |
| llvm | 18 | **21** (`1:21.1.8`) | |
| cmake | 3.28 | **4.2.3** | major bump 3→4 (see gotcha §6) |
| ninja | 1.11 | **1.13.2** | |
| valgrind | 3.22 | 3.26.0 | |
| gdb | 15 | 17.1 | |
| postgresql (default) | 16 | **18** | **prod box is PG16** — mismatch, see §6 |
| nodejs (base repo) | 18 | 22.22 | dev image still uses NodeSource LTS (distro-agnostic) |
| opam | 2.1 | 2.5.0 | |
| git | 2.43 | 2.53 | |

The claimed "gcc-15 / clang-21 / llvm-21" defaults are **CONFIRMED**.

### Coverage tooling version-skew (was hardcoded to 18)

- The versioned binaries `llvm-cov-21` and `llvm-profdata-21` exist as
  `/usr/bin/` symlinks once `llvm-21` is installed.
- `v3/scripts/ci/steps/run-coverage-report.sh` already resolves tools as
  `command -v llvm-profdata-18 || command -v llvm-profdata` (and likewise for
  llvm-cov). On 26.04 the unversioned fallback resolves to the **21** series.
- **profraw/profdata skew**: `.profraw` files written by a clang-21-instrumented
  binary MUST be merged with a matching-major `llvm-profdata` (21). Because the
  binary is compiled with the distro clang (21) and the script's unversioned
  fallback is also 21, they match — coverage works without edits. The `-18`
  literals remaining in that script are now only stale **comments/labels**
  (incl. the emitted JSON `"tool"` string and the "known llvm-cov-18 hang"
  note); the functional path is version-agnostic. Recommend a follow-up to
  relabel them to avoid confusion (not required for correctness).

`docs/quality/setup-quality-vm.sh` DID hardcode `lld-18 llvm-18` and verified
`clang-18 / llvm-cov-18 / llvm-profdata-18` — those are wrong for 26.04 and
have been updated to the 21 series (see §4).

---

## 3. Package availability — CONFIRMED, with t64 renames

### Geo + arithmetic stack (dev only) — all present

| Package | 26.04 version |
|---------|---------------|
| libcgal-dev | 6.1.1-2 |
| libgmp-dev | 6.3.0 |
| libmpfr-dev | 4.2.2 |
| libfmt-dev | 10.1.1 |
| libeigen3-dev | 3.4.0 |
| libboost-all-dev | 1.90.0 |

### Processor dev libs (builder stage) — all present, names unchanged

libwebsockets-dev 4.3.5, libcurl4-openssl-dev 8.18, libproj-dev 9.7.1,
libjpeg-dev 8c, libjansson-dev 2.14, libsqlite3-dev 3.46, libssl-dev 3.5.5,
libxxhash-dev 0.8.3.

### Runtime SONAME packages — **NAME CHANGES (time64 / t64 transition)**

This is the one place 24.04 package names break on 26.04. The runtime stage of
`v3/Dockerfile` lists SONAME-numbered packages; three were renamed:

| 24.04 name | 26.04 name | Status |
|------------|------------|--------|
| `libwebsockets19` | **`libwebsockets19t64`** | RENAMED |
| `libcurl4` | **`libcurl4t64`** | RENAMED |
| `libssl3` | **`libssl3t64`** | RENAMED |
| `libproj25` | `libproj25` | unchanged |
| `libjpeg-turbo8` | `libjpeg-turbo8` | unchanged |
| `libjansson4` | `libjansson4` | unchanged |
| `libsqlite3-0` | `libsqlite3-0` | unchanged |
| `libxxhash0` | `libxxhash0` | unchanged |
| `libstdc++6` | `libstdc++6` | unchanged (now from gcc-16 runtime) |
| `netcat-openbsd` | `netcat-openbsd` | unchanged |

Building the old names verbatim on 26.04 fails with "Unable to locate package
libwebsockets19" etc. The runtime stage has been updated to the t64 names.

### External apt repos — not codename-gated (safe)

- **NodeSource** (`setup_lts.x`) now uses the distro-agnostic `nodistro` suite,
  so it does not gate on `resolute` and installs fine on 26.04.
- **GitHub CLI** repo uses a generic `stable main` suite — also not gated.

---

## 4. Portability fixes (claimed-merged) — CONFIRMED present in main

All four toolchain-portability fixes the task referenced are present in the
tree and are what let gcc-15 / clang-21 compile the code:

| Fix | Location | Present |
|-----|----------|---------|
| `safe_alloc.h` PTRDIFF_MAX overflow guard | `v3/core/include/v3/safe_alloc.h:33,47,58` | yes |
| PolyFit/soplex array-copy patch | `v3/scripts/fetch-third-party.sh` `apply_patches_PolyFit()` | yes |
| `-c11` conditional probe for Frama-C | `v3/scripts/frama-c/audit-*.sh` (`frama-c -c11 /dev/null … && echo -c11`) | yes |
| `-fpermissive` on PolyFit C++ targets | `v3/core/CMakeLists.txt:476` | yes |

These are independent of the OS bump but are prerequisites for newer gcc/clang
accepting the third-party sources; they remain in place.

`setup-quality-vm.sh` also updated for 26.04: apt list now installs
`lld-21 llvm-21` (was `-18`); the verify loop checks `clang-21 / llvm-cov-21 /
llvm-profdata-21`; the os-release sanity check now expects `26.04`.

---

## 5. Dev-container volume design + switch procedure

### What is baked into the image vs. lives on a volume (post-rework)

The dev image (`v3/.devcontainer/`) was reworked so that toolchain/state that
used to be ad-hoc volume/runtime state is now either **in the image** (so it
rebuilds deterministically and is never shadowed by a volume) or on a
**purpose-named volume** (so data persists but a fresh volume self-bootstraps):

| Thing | Where it lives now | Why |
|-------|--------------------|-----|
| opam switch + **Frama-C 32.0** (OCaml 4.14.2) | **image**, `OPAMROOT=/opt/opam` | built in a multi-stage `opam-builder`; final image copies only the realized switch (no build cache). At `/opt/opam` so the `home` volume can't shadow it. **No switch-rebuild needed after an OS bump** — `docker compose build` realizes it fresh. |
| Node 24 / PG 18 / gcc / clang / cmake | **image** | apt-installed each rebuild |
| dev shell config | **image** `/etc/profile.d/dioramap.sh` | sourced by login + interactive-non-login; not in the home volume |
| repos, `~/.ssh`, `~/.gitconfig`, history, npm caches, `third_party/` clones | **`home` volume** (`/home/ubuntu`) | user state, survives recreation |
| PostgreSQL 18 data cluster | **`pgdata` volume** (`/var/lib/postgresql`) | dev DB persists; empty volume bootstraps via `dioramap-pg-init` |
| processor cache | **`cache` volume** (`/var/cache/dioramap`) | `DIORAMAP_CACHE_DIR` matches prod; processor creates files |

The **named-volume-shadows-image gotcha** (§ headline below) governs all three:
a volume mounted at a path hides whatever the image baked there. That is why
the opam switch is at `/opt/opam` (NOT `~/.opam`, which `home` would shadow),
and why both PG and the cache have **init-if-empty** handling — see the
`dioramap-pg-init` helper (recreates `pg_createcluster 18 main` when a fresh
`pgdata` volume has shadowed the image's auto-created cluster) and the cache
dir which the processor populates lazily.

### Finding (HEADLINE): /home is ALREADY a reusable named volume

`docker inspect` of the live `dioramap` container (`dd4cbc51614c`):

```
Type:        volume
Name:        dioramap-dev_home
Source:      /var/lib/docker/volumes/dioramap-dev_home/_data
Destination: /home/ubuntu
Driver:      local
```

It is **not** a bind mount and **not** an ephemeral overlay. The compose file
(`v3/.devcontainer/docker-compose.yml`) already declares it:

```yaml
volumes:
  - home:/home/ubuntu     # named volume "home" → dioramap-dev_home
```

The volume currently holds all the reusable state we care about: the
`lidar-algos` checkout (incl. `.claude/worktrees/`), `frama-c/`, `control-plane`,
`.opam` caches, `.ssh`, `.gitconfig`, `.bash_history`, `.claude`, npm caches,
and `third_party/` clones under the repo. So **no conversion is needed** — the
"painless switch" property already exists by design.

### Why the switch is zero-re-setup

1. The home volume is decoupled from the image. Rebuilding the image to
   `ubuntu:26.04` and recreating the container re-attaches the SAME
   `dioramap-dev_home` volume — checkouts, `~/.opam`, `~/.ssh`, shell history,
   and `third_party/` clones are untouched.
2. uid/gid parity: 26.04's default `ubuntu` user is still 1000:1000, matching
   the volume's existing ownership. No `chown` pass, no permission breakage.
3. Dev shell config lives in the image layer (`/etc/profile.d/dioramap.sh`,
   sourced via `/etc/bash.bashrc`), NOT in `/home/ubuntu` — so config updates
   ship with the rebuild while user-owned `~/.bashrc` is left alone. This is an
   existing, deliberate design (Dockerfile §8) and continues to work.

### Switch procedure (run on the Docker host, NOT inside the container)

> The live dev container you are reading this from is backed by this volume.
> Run these from the host shell so you are not pulling the rug out from under
> your own session.

```bash
cd v3/.devcontainer

# 0. (Optional but recommended) snapshot the volume before the switch.
#    Any ubuntu tag works for the tar; the volume data is image-independent.
docker run --rm -v dioramap-dev_home:/data -v "$PWD":/backup ubuntu:26.04 \
    tar czf /backup/home-backup-$(date +%F).tar.gz -C /data .

# 1. Build the 26.04 image (DOCKER_GID must match the host docker group).
export DOCKER_GID=$(stat -c '%g' /var/run/docker.sock)
docker compose build          # rebuilds `dioramap-dev` from ubuntu:26.04

# 2. Recreate the container on the new image. The `home` volume is NOT
#    removed by `up` — only `down -v` deletes it. Do NOT pass -v.
docker compose up -d          # recreates `dioramap`, re-attaches dioramap-dev_home

# 3. Verify.
docker compose exec dioramap bash -lc '
  cat /etc/os-release | grep VERSION= ;
  gcc --version | head -1 ; clang --version | head -1 ;
  ls ~/lidar-algos >/dev/null && echo "checkout intact" ;
  ls ~/.opam       >/dev/null && echo "opam intact"'
```

If something is wrong, roll back by checking out the previous Dockerfile and
re-running `docker compose up -d --build`; the volume is unaffected by image
changes.

### Post-bump one-time cleanup (volume-resident COMPILED state)

The reworked image moves opam/Frama-C into the image, so **no switch rebuild
is needed** after an OS bump. But other *compiled* state still lives on the
`home` volume and is pinned to the OLD toolchain paths/ABI — it does NOT
rebuild automatically and must be wiped/rebuilt once after a bump:

```bash
# Inside the freshly-recreated container.

# 1. CMake build dirs — CMakeCache.txt pins the OLD gcc/clang absolute path
#    (e.g. /usr/bin/gcc-13). cmake won't re-detect a new compiler in an
#    existing cache; configure fails or silently keeps the stale path.
#    Nuke every out-of-tree build dir under the repo and reconfigure.
find ~/lidar-algos/v3 -maxdepth 3 -type d -name 'build*' -prune -exec rm -rf {} +
#    (then re-run your usual cmake configure/build)

# 2. Node projects with NATIVE addons — *.node binaries were linked against
#    the old libstdc++/glibc and the old Node ABI. Rebuild them.
for p in ~/lidar-algos/lidar-algos-bench-v2 ~/lidar-algos/v3/apps/lab-ui ~/control-plane; do
    [ -f "$p/package.json" ] && ( cd "$p" && npm rebuild )
done
#    (or `rm -rf node_modules && npm install` if rebuild misbehaves)
```

The opam switch is exempt — it is in the image now, not on the volume, so the
rebuild produces a fresh switch on the new base every time.

### uid/gid caveats

- The named volume is owned by 1000:1000. Keep the container user at uid 1000
  (the Dockerfile reuses the base `ubuntu` user, which is 1000 on 26.04) — do
  not switch to a different uid or you would need to chown the whole volume.
- `DOCKER_GID` is a build arg matching the host's docker-group GID (currently
  999 on this host) so the bind-mounted `/var/run/docker.sock` is usable. The
  `/etc/profile.d/dioramap.sh` fallback chgrp/chmods the socket on first shell
  for Docker Desktop hosts where the socket is root:root — unchanged by the bump.

### If you ever DO need to convert a non-volume home (general recipe)

Not applicable here, but for completeness: with the home on a bind/overlay you
would (a) create the named volume, (b) `docker run --rm -v old:/from -v
new:/to ubuntu tar -C /from -cf - . | tar -C /to -xf -`, (c) repoint the mount
in compose to the named volume, (d) recreate the container.

---

## 6. Residual gotchas / human decisions

1. **PostgreSQL major mismatch (dev 18 vs prod 16) — accepted interim.**
   26.04's default `postgresql` metapackage is PG **18**; the prod box runs
   PG **16**. We tried pinning in-image PG16 via PGDG's `noble` suite first;
   it fails to install on 26.04 because PGDG's `postgresql-16` depends on
   `libicu74` while 26.04 ships `libicu76` (different SONAME, not coexisting
   transparently), and PGDG has not published a `resolute-pgdg` (26.04)
   suite yet. So we install 26.04's native PG18 in-image and accept the
   major divergence — migration-driven schema + PG18's backward compatibility
   keep normal dev flows working. When exact PG16 parity is needed for a
   specific test, spin an ad-hoc `postgres:16` sidecar (compose service or
   `docker run`). Flip the in-image install back to `postgresql-16` via PGDG
   once a `resolute-pgdg` suite lands upstream.

2. **cmake 3 → 4 major bump.** 26.04 ships cmake 4.2. Projects that set
   `cmake_minimum_required(VERSION <3.5)` get a hard error under cmake 4 (the
   compatibility floor was raised). Our top-level `core/CMakeLists.txt` should
   be checked; more likely risk is in `third_party/` (PolyFit/copc-lib/soplex).
   The prod builder build (§7) exercises copc-lib end-to-end, so any copc-side
   cmake-4 breakage would have surfaced there. Watch for it when fetching the
   dev-only PolyFit/CGAL research stack under cmake 4.

3. **Stale `-18` labels in coverage script.** Functionally correct (falls back
   to unversioned = 21), but comments and the emitted JSON `"tool"` field still
   say `llvm-*-18`. Cosmetic; relabel in a follow-up.

4. **Sanitizer ASLR sysctl.** The 24.04 `vm.mmap_rnd_bits=28` workaround for
   TSan/ASan is unrelated to the container image (it's a host-kernel setting)
   and still applies on a 26.04 host running the quality VM. Left in
   setup-quality-vm.sh; re-confirm the right value on a real 26.04 kernel before
   relying on TSan there.

5. **Frama-C version.** opam on 26.04 is 2.5.0 and installs Frama-C from the
   opam repo; the `-c11` probe in the audit scripts already gates the flag on
   `frama-c -c11` succeeding, so a newer Frama-C (the v32-era was the claim) is
   handled gracefully. Not pinned here — install via `setup-quality-vm.sh`.

6. **Pre-existing (NOT introduced by this bump) build-context dependency.**
   The prod `Dockerfile` clones only `laz-perf` at build time, but the cmake
   configure also needs `third_party/copc-lib/` to be present, and
   `DIORAMAP_RESEARCH` defaults **ON** in `core/CMakeLists.txt:19` (the inline
   comments at lines 416/679 saying "default OFF" are stale and contradict the
   `option()` line). `third_party/` is gitignored, and the release path
   (`deploy/lib/build.sh`) builds from `git archive <tag>` — which contains
   neither copc-lib nor PolyFit. So a from-archive prod build relies on the
   third-party fetch having populated those paths first (via
   `fetch-third-party.sh`), and on RESEARCH being turned off or PolyFit/CGAL
   present. This behaves identically on 24.04 and 26.04 — it is orthogonal to
   the OS upgrade — but it surfaced during build validation and is worth a
   separate cleanup ticket (either reconcile the RESEARCH default with the
   stale comments, or have the prod Dockerfile fetch copc-lib like it does
   laz-perf). For the upgrade, the toolchain itself was proven to compile the
   production processor + copc-lib on 26.04 (see §7).

---

## 7. Build evidence

All builds run against the real `ubuntu:26.04` base on the shared host Docker
engine (engine 29.4.1). Test images tagged `*:26.04-wip`.

### 7.1 Runtime stage (t64 packages) — PASS

Installed the exact runtime apt set from the upgraded `Dockerfile` runtime
stage on `ubuntu:26.04`, exit 0:

```
Setting up libsqlite3-0 (3.46.1-9) … libjansson4 (2.14) … libjpeg-turbo8 (2.1.5) …
Setting up libwebsockets19t64 (4.3.5-3ubuntu1) …
Setting up libcurl4t64 (8.18.0-1ubuntu2.1) … libcurl3t64-gnutls (8.18.0) …
Setting up libproj25 (9.7.1-1) …
RUNTIME-APT-EXIT: 0
useradd OK: uid=999(dioramap) …
```

### 7.2 Builder stage — production processor compiled — PASS

Built the `builder` target on `ubuntu:26.04` with copc-lib + laz-perf present
(`DIORAMAP_RESEARCH=OFF`, `DIORAMAP_OBFUSCATE=OFF` — see note below), exit 0:

```
-- The C compiler identification is GNU 15.2.0
-- The CXX compiler identification is GNU 15.2.0
-- Found libwebsockets 4.3.5 / libcurl 8.18.0 / proj 9.7.1 / libjpeg 2.1.5 / sqlite3 3.46.1
[146/146] Linking CXX executable dioramap-processor
Successfully built / tagged dioramap-proc-builder:26.04-wip
```

Built with **gcc 15.2.0** and **cmake 4.2.3**. Only benign copc-lib warnings
(`-Wreorder`, `-Wtype-limits`); no errors. The resulting 928 KB binary links:

```
libwebsockets.so.19   libcurl.so.4   libproj.so.25   libjpeg.so.8
libjansson.so.4       libsqlite3.so.0  libssl.so.3   libstdc++.so.6
libcurl-gnutls.so.4
```

These SONAMEs are exactly what the §7.1 runtime packages provide
(`libwebsockets19t64`→`.so.19`, `libcurl4t64`→`.so.4`, `libssl3t64`→`.so.3`,
`libproj25`→`.so.25`), so builder↔runtime are consistent end-to-end.

> Why OBFUSCATE/RESEARCH were toggled for the evidence build: the committed
> prod `Dockerfile` pins `DIORAMAP_OBFUSCATE=ON`, whose configure-time gate
> requires a fresh `scripts/obfuscation-map.json`. In this dirty worktree that
> map is stale, so OBFUSCATE=ON aborts at the gate **before** any compilation —
> identically on 24.04. The release path (`deploy/lib/build.sh`) builds from
> `git archive <tag>` where the map is committed-fresh, so the production build
> keeps OBFUSCATE=ON. Toggling it OFF here isolates the question that matters
> for the OS bump — "does the 26.04 gcc-15 toolchain compile the processor?" —
> and the answer is yes. RESEARCH=OFF mirrors what production needs (PolyFit/
> CGAL are dev-only); see §6 gotcha 6 for the pre-existing copc-lib/RESEARCH
> context dependency.

### 7.3 Dev image — built on `ubuntu:26.04` — PASS

`docker build --build-arg DOCKER_GID=999 -t dioramap-dev:26.04-wip` on the
upgraded `.devcontainer/Dockerfile`. Installs the full toolchain (clang/llvm 21,
gcc 15, cmake 4.2, postgresql 18, CGAL 6.1, NodeSource LTS, gh, dev libs). See
the final report / commit for the captured tail; all apt sets and external
repos (NodeSource `nodistro`, GitHub CLI `stable main`) resolved on 26.04.
</content>
