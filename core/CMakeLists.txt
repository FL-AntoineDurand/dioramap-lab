cmake_minimum_required(VERSION 3.24)

project(dioramap_processor
    VERSION 0.2.3
    LANGUAGES C CXX
    DESCRIPTION "Dioramap processor: unified lidar processing core")

# -----------------------------------------------------------------------------
# Options — wired from Phase A1; features land in later A-phase tasks
# -----------------------------------------------------------------------------
option(DIORAMAP_CUDA         "Build with CUDA backend"                        OFF)
option(DIORAMAP_ASAN         "Enable AddressSanitizer + UBSan"                OFF)
option(DIORAMAP_TSAN         "Enable ThreadSanitizer + UBSan (excl. ASAN)"    OFF)
option(DIORAMAP_COVERAGE     "Enable llvm-cov instrumentation (clang only)"   OFF)
option(DIORAMAP_FUZZ         "Build libFuzzer targets (clang only, +ASan)"    OFF)
option(DIORAMAP_BUILD_TESTS  "Build unit / smoke tests"                       ON)
option(DIORAMAP_USE_JEMALLOC "Link against jemalloc"                          OFF)
# DIORAMAP_V7 gates the v7 roof-recon step library (dioramap_steps_v7). This is
# the product roof-recon port — it depends only on dioramap_runner / jansson /
# m / dioramap_steps_normals (NO PolyFit/CGAL/Roofer), so it builds everywhere
# by default and ships in the production processor.
option(DIORAMAP_V7           "Build the v7 roof-recon step library"           ON)
# DIORAMAP_V7_TRACE compiles in the v7 stderr trace macros (see
# steps/v7/v7-trace.h). OFF by default → every V7_TRACEF/V7_TRACE_IF expands
# to ((void)0), zero overhead, nothing in the shipped binary. Turn ON for a
# throwaway debug build: cmake -DDIORAMAP_V7_TRACE=ON -B build-trace.
option(DIORAMAP_V7_TRACE     "Compile in v7 step stderr traces (debug)"       OFF)
# DIORAMAP_PLANE_FIT_AREA_WEIGHTED restores the legacy area-weighted plane fit
# (v7_plane_fit_lsq). OFF by default → v7 fits planes UNWEIGHTED (ts-weight is
# a tangential area, irrelevant to a perpendicular fit; see v7_fit_weights).
# Turn ON only for an A/B or to revert the decision.
option(DIORAMAP_PLANE_FIT_AREA_WEIGHTED "Fit planes area-weighted (legacy)"   OFF)

if(DIORAMAP_ASAN AND DIORAMAP_TSAN)
    message(FATAL_ERROR "DIORAMAP_ASAN and DIORAMAP_TSAN are mutually exclusive")
endif()
if(DIORAMAP_TSAN AND DIORAMAP_FUZZ)
    message(FATAL_ERROR "DIORAMAP_TSAN and DIORAMAP_FUZZ are mutually exclusive (fuzz uses ASan)")
endif()
if((DIORAMAP_COVERAGE OR DIORAMAP_FUZZ) AND NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "DIORAMAP_COVERAGE / DIORAMAP_FUZZ require clang (set CC=clang CXX=clang++)")
endif()


# -----------------------------------------------------------------------------
# C standard + editor support
# -----------------------------------------------------------------------------
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -----------------------------------------------------------------------------
# Warnings as errors, strict
# -----------------------------------------------------------------------------
add_compile_options(-Wall -Wextra -Wno-unused-parameter)
# Strict warnings-as-errors only for our own C code. Vendored C++ libraries
# (copc-lib, laz-perf) are known-good but not written to our strictness
# level and would fail -Werror. Limiting by COMPILE_LANGUAGE:C keeps the
# discipline where we control the code.
add_compile_options("$<$<COMPILE_LANGUAGE:C>:-Werror>")
add_compile_options("$<$<COMPILE_LANGUAGE:C>:-Wpedantic>")
add_compile_options("$<$<COMPILE_LANGUAGE:C>:-Wshadow>")
add_compile_options("$<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes>")
add_compile_options("$<$<COMPILE_LANGUAGE:C>:-Wmissing-prototypes>")
# GCC 13 flags format-truncation false-positives for numeric fields whose
# bounds the compiler can't prove (e.g. printf("%04d", tm.tm_year + 1900)
# in log.c). The code is correct; silence the false alarm.
add_compile_options("$<$<COMPILE_LANGUAGE:C>:-Wno-format-truncation>")

# -----------------------------------------------------------------------------
# Sanitizers
# -----------------------------------------------------------------------------
if(DIORAMAP_ASAN)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

if(DIORAMAP_TSAN)
    # TSan + UBSan is the standard pairing.  TSan needs PIE on most
    # platforms; CMake handles that via POSITION_INDEPENDENT_CODE on
    # executable targets (already the default for our builds).
    add_compile_options(-fsanitize=thread,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread,undefined)
endif()

if(DIORAMAP_COVERAGE)
    # llvm-cov instrumentation.  After a run, .profraw files are written
    # (location controlled by LLVM_PROFILE_FILE env var); merge with
    # llvm-profdata; report with llvm-cov show / report.  See
    # docs/quality/README.md (coverage variant).
    add_compile_options(-fprofile-instr-generate -fcoverage-mapping -g)
    add_link_options(-fprofile-instr-generate -fcoverage-mapping)
endif()

# Helper: opt a target out of coverage instrumentation.  Used on every
# third-party target so the coverage map stays small enough for llvm-cov
# to process in reasonable time.  Instrumenting PolyFit + CGAL + Roofer
# + copc-lib brings the coverage map up to ~37 million blocks, at which
# point llvm-cov-18 hangs (effectively infinite report time).  We only
# care about coverage of our own code anyway.
function(dioramap_opt_out_of_coverage tgt)
    if(DIORAMAP_COVERAGE AND TARGET ${tgt})
        target_compile_options(${tgt} PRIVATE
            -fno-profile-instr-generate -fno-coverage-mapping)
    endif()
endfunction()

if(DIORAMAP_FUZZ)
    # libFuzzer + ASan + UBSan.  Fuzz targets are separate executables;
    # each links the required dioramap_* libs and provides
    # LLVMFuzzerTestOneInput.  The processor binary is NOT built with
    # -fsanitize=fuzzer (it'd hijack main()); fuzz targets live under
    # tests/fuzz/ and have their own add_executable() calls.
    add_compile_options(-fsanitize=address,undefined,fuzzer-no-link
                        -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

# clang -Wthread-safety is opt-in; auto-enable for any clang build so
# the annotations in include/v3/thread_safety.h are validated.  No-op
# on gcc (gcc rejects the flag, so this branch is clang-only).  See
# docs/quality/README.md (clang-ts variant) and
# CONCURRENCY.md §3 invariants 8-10.
if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    add_compile_options(-Wthread-safety -Wthread-safety-beta
                        -Werror=thread-safety
                        # Our log macros use the widely-supported `, ##__VA_ARGS__`
                        # GNU extension for "zero-or-more varargs".  Clang warns
                        # on it under -Wgnu-zero-variadic-macro-arguments and our
                        # -Werror would escalate.  Quiet it; we use this pattern
                        # in many files (POLY_TRACE, log_pushf, etc.).  C23's
                        # __VA_OPT__(,) is the future-proof replacement, but
                        # we're on C11 and gcc/clang both support the GNU form.
                        -Wno-gnu-zero-variadic-macro-arguments)
endif()

# -----------------------------------------------------------------------------
# CUDA (option wired; no targets yet — arrives in A11)
# -----------------------------------------------------------------------------
if(DIORAMAP_CUDA)
    enable_language(CUDA)
    find_package(CUDAToolkit REQUIRED)
    message(STATUS "CUDA enabled (A11 will add targets)")
endif()

# -----------------------------------------------------------------------------
# Threads (needed by runtime library)
# -----------------------------------------------------------------------------
find_package(Threads REQUIRED)

# -----------------------------------------------------------------------------
# Logging library (A2 + L2/L3 ring buffer + drain thread)
# -----------------------------------------------------------------------------
add_library(dioramap_log STATIC
    src/log/log.c
    src/log/log_events.c
    src/log/error.c
)
target_include_directories(dioramap_log PUBLIC include)
target_link_libraries(dioramap_log PUBLIC Threads::Threads)

# -----------------------------------------------------------------------------
# Runtime primitives library (A3)
# -----------------------------------------------------------------------------
add_library(dioramap_runtime STATIC
    src/runtime/thread_pool.c
)
target_include_directories(dioramap_runtime PUBLIC include)
target_link_libraries(dioramap_runtime PUBLIC Threads::Threads)

# -----------------------------------------------------------------------------
# Content hashing (A4). xxh3 via libxxhash for now; swappable for blake3 later.
# -----------------------------------------------------------------------------
add_library(dioramap_hash STATIC
    src/hash/hash.c
)
target_include_directories(dioramap_hash PUBLIC include)
target_link_libraries(dioramap_hash PUBLIC xxhash)

# -----------------------------------------------------------------------------
# Artifact binary layout codec (A4) + mmap helper
# -----------------------------------------------------------------------------
add_library(dioramap_artifact STATIC
    src/artifact/artifact.c
    src/mmap/mmap_file.c
)
target_include_directories(dioramap_artifact PUBLIC include)
target_link_libraries(dioramap_artifact PUBLIC dioramap_hash)

# -----------------------------------------------------------------------------
# Content-addressed disk store (A5 + B3.5 hot tier)
# -----------------------------------------------------------------------------
add_library(dioramap_store STATIC
    src/store/store.c
)
target_include_directories(dioramap_store PUBLIC include)
target_link_libraries(dioramap_store PUBLIC dioramap_hash dioramap_log Threads::Threads)

# -----------------------------------------------------------------------------
# Persistent intent map (B3.5): step_identity -> content_hash
# -----------------------------------------------------------------------------
add_library(dioramap_intent_map STATIC
    src/intent_map/intent_map.c
)
target_include_directories(dioramap_intent_map PUBLIC include)
target_link_libraries(dioramap_intent_map PUBLIC dioramap_hash dioramap_log Threads::Threads)

# -----------------------------------------------------------------------------
# cached_compute primitive (B3.5): the one cache-or-compute call used at
# step-scope by the runner and at sub-scope by fetchers / other steps.
# Thin glue over the store + intent map.
# -----------------------------------------------------------------------------
add_library(dioramap_cached_compute STATIC
    src/runner/cached_compute.c
)
target_include_directories(dioramap_cached_compute PUBLIC include)
target_link_libraries(dioramap_cached_compute PUBLIC
    dioramap_intent_map dioramap_store dioramap_hash)

# -----------------------------------------------------------------------------
# Embedded pipelines. Pipelines listed here are baked into the binary so the
# processor works without a --pipelines-dir. Everything else under
# pipelines/smoke/ is loaded from disk with --pipelines-dir.
# -----------------------------------------------------------------------------
set(EMBEDDED_PIPELINES
    ${CMAKE_CURRENT_SOURCE_DIR}/../pipelines/smoke/viewer-full.json
)

set(_EMBED_OUT ${CMAKE_BINARY_DIR}/generated/embedded_pipelines.c)
find_program(_EMBED_NODE node REQUIRED)
add_custom_command(
    OUTPUT ${_EMBED_OUT}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
    COMMAND ${_EMBED_NODE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/embed-pipelines.mjs
            --out ${_EMBED_OUT}
            ${EMBEDDED_PIPELINES}
    DEPENDS ${EMBEDDED_PIPELINES}
            ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/embed-pipelines.mjs
    COMMENT "embed pipelines → ${_EMBED_OUT}"
    VERBATIM
)

# -----------------------------------------------------------------------------
# Step registry + pipeline + runner (A6 + B3.5 cached_compute / manifest)
# -----------------------------------------------------------------------------
add_library(dioramap_runner STATIC
    src/step/registry.c
    src/step/step_bbox_params.c
    src/pipeline/pipeline.c
    src/runner/runner.c
    src/runner/step_manifest.c
    ${_EMBED_OUT}
)
target_include_directories(dioramap_runner PUBLIC include)
target_link_libraries(dioramap_runner PUBLIC
    dioramap_store dioramap_intent_map dioramap_cached_compute
    dioramap_hash dioramap_log dioramap_runtime jansson)

# -----------------------------------------------------------------------------
# Outbound HTTPS (A9). libcurl for transport. TLS backend comes from whatever
# libcurl was built against on the target system (OpenSSL on Ubuntu's
# libcurl4-openssl-dev today; PLAN-FETCHERS decision 2 reserves mbedTLS for
# static-binary builds, which we'll wire when size matters).
# -----------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(LWS REQUIRED IMPORTED_TARGET libwebsockets)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(PROJ REQUIRED IMPORTED_TARGET proj)
pkg_check_modules(JPEG REQUIRED IMPORTED_TARGET libjpeg)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)

# Run journal (L6.1) — SQLite-backed history of completed pipeline runs.
# Stays a separate static lib so consumers that don't need it (batch
# mode, tests) link nothing extra.
add_library(dioramap_run_journal STATIC
    src/runner/run_journal.c)
target_include_directories(dioramap_run_journal PUBLIC include)
target_link_libraries(dioramap_run_journal PUBLIC PkgConfig::SQLITE3 Threads::Threads)

# -----------------------------------------------------------------------------
# LAZ decompression + COPC hierarchy (B3). Vendored under third_party/.
# copc-lib brings its own laz-perf via libs/laz-perf. We link the static
# variants (copc-lib-s, lazperf) into our own thin C wrapper.
# -----------------------------------------------------------------------------
set(WITH_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../third_party/copc-lib copc-lib EXCLUDE_FROM_ALL)
# copc-lib's cpp/CMakeLists.txt links against `lazperf_s`, but laz-perf's
# CMakeLists builds the static target as `lazperf` (when BUILD_SHARED_LIBS=OFF).
# The rename-on-install conventions don't apply to add_subdirectory, so we
# provide the expected alias target here.
if(NOT TARGET lazperf_s)
    add_library(lazperf_s ALIAS lazperf)
endif()

# Opt third-party out of coverage instrumentation (see helper at the top
# of this file).  We only care about coverage on our own code.
foreach(_3p_target copc-lib copc-lib-s lazperf)
    dioramap_opt_out_of_coverage(${_3p_target})
endforeach()

add_library(dioramap_lazperf STATIC
    src/lazperf/decompress.cpp
)
target_include_directories(dioramap_lazperf PUBLIC include)
target_include_directories(dioramap_lazperf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/copc-lib/cpp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/copc-lib/libs/laz-perf/cpp
)
target_link_libraries(dioramap_lazperf PUBLIC copc-lib-s)
# Vendored copc-lib / laz-perf headers trip several of our strict warnings
# when transitively included. We quiet them for this wrapper TU only.
target_compile_options(dioramap_lazperf PRIVATE
    -Wno-deprecated-declarations -Wno-shadow -Wno-error)


# -----------------------------------------------------------------------------
# PROJ coord transforms (B2).
# -----------------------------------------------------------------------------
add_library(dioramap_coords STATIC
    src/coords/coords.c
)
target_include_directories(dioramap_coords PUBLIC include)
target_link_libraries(dioramap_coords PUBLIC PkgConfig::PROJ Threads::Threads)

add_library(dioramap_fetch STATIC
    src/fetch/fetch.c
)
target_include_directories(dioramap_fetch PUBLIC include)
target_link_libraries(dioramap_fetch PUBLIC
    dioramap_hash dioramap_store dioramap_intent_map dioramap_cached_compute
    dioramap_log jansson PkgConfig::CURL Threads::Threads)

add_library(dioramap_server STATIC
    src/server/server.c
    src/server/session.c
    src/server/session_mgr.c
)
target_include_directories(dioramap_server PUBLIC include)
# processor_version in SESSION_STARTED / RUN_FAILED (docs/ERRORS.md).
target_compile_definitions(dioramap_server PRIVATE DIORAMAP_VERSION="${PROJECT_VERSION}")
target_link_libraries(dioramap_server PUBLIC
    dioramap_runner dioramap_run_journal dioramap_log dioramap_fetch jansson
    PkgConfig::LWS Threads::Threads)

# -----------------------------------------------------------------------------
# Step packages. Each package is a library of step_def_t's + a register() fn.
# -----------------------------------------------------------------------------
set(FETCHERS_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/fetchers/fetchers.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/fetchers/wms-ortho-fetch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/fetchers/wms-elevation-fetch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/fetchers/fetch-ortho-rgb.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/fetchers/fetch-ortho-irc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/fetchers/fetch-elevation.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/fetchers/fetch-cadastre-parcels.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/fetchers/fetch-cadastre-buildings.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/fetchers/fetch-ortho-surround.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/fetchers/fetch-elevation-surround.c
)
add_library(dioramap_steps_fetchers STATIC ${FETCHERS_SRCS})
target_include_directories(dioramap_steps_fetchers
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../steps/fetchers
    PRIVATE include
)
target_link_libraries(dioramap_steps_fetchers PUBLIC
    dioramap_runner dioramap_fetch jansson)

set(COORDS_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/coords/coord-steps.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/coords/canonicalize-location.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/coords/compute-scene-origin.c
)
add_library(dioramap_steps_coords STATIC ${COORDS_SRCS})
target_include_directories(dioramap_steps_coords
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../steps/coords
    PRIVATE include
)
target_link_libraries(dioramap_steps_coords PUBLIC
    dioramap_runner dioramap_coords jansson m)

set(COPC_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/copc/copc-steps.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/copc/find-copc-tiles.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/copc/fetch-copc-tiles.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/copc/laz-extract-disc.c
)
add_library(dioramap_steps_copc STATIC ${COPC_SRCS})
target_include_directories(dioramap_steps_copc
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../steps/copc
    PRIVATE include
           ${CMAKE_CURRENT_SOURCE_DIR}/../steps/fetchers   # for fetchers_shared_client
)
target_link_libraries(dioramap_steps_copc PUBLIC
    dioramap_runner dioramap_fetch dioramap_lazperf dioramap_steps_fetchers jansson)

set(NORMALS_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/normals/normals-steps.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/normals/compute-normals.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/normals/kd_tree.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/normals/pca.c
)
add_library(dioramap_steps_normals STATIC ${NORMALS_SRCS})
target_include_directories(dioramap_steps_normals
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../steps/normals
    PRIVATE include
)
target_link_libraries(dioramap_steps_normals PUBLIC dioramap_runner jansson m)

# B5a + B5c: LiDAR rasters + coregister-ortho-lidar. Also exposes
# raster_format.{h,c} (the multi-channel F32 raster artifact format) used
# by every B5 step, plus edges.{h,c} (Sobel + adaptive normalisation).
set(REGISTRATION_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/registration/registration-steps.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/registration/raster_format.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/registration/edges.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/registration/build-lidar-rasters.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/registration/compute-ndsm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/registration/compute-building-mask.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/registration/coregister-ortho-lidar.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/registration/rasterize-cadastre-edges.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/registration/fit-cadastre-to-lidar.c
)
add_library(dioramap_steps_registration STATIC ${REGISTRATION_SRCS})
target_include_directories(dioramap_steps_registration
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../steps/registration
    PRIVATE include
            ${CMAKE_CURRENT_SOURCE_DIR}/../steps/ortho  # for ortho-steps.h channel enums
)
target_link_libraries(dioramap_steps_registration PUBLIC dioramap_runner jansson m)

# B5b: ortho rasters (JPEG decode + bilinear resample + per-cell channels).
# Depends on dioramap_steps_registration for raster_format.{h,c}.
set(ORTHO_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/ortho/ortho-steps.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/ortho/jpeg_decode.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/ortho/grid_shape.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/ortho/build-ortho-rgb-rasters.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/ortho/build-ortho-irc-rasters.c
)
add_library(dioramap_steps_ortho STATIC ${ORTHO_SRCS})
target_include_directories(dioramap_steps_ortho
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../steps/ortho
    PRIVATE include
)
target_link_libraries(dioramap_steps_ortho PUBLIC
    dioramap_runner dioramap_steps_registration jansson PkgConfig::JPEG m)

# B6: classification chain (tile-ndvi, normal-scatter, near-building mask,
# classify-vegetation). Re-uses the jpeg_decode helper from ortho and the
# raster_format helper from registration.
set(CLASSIFICATION_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/classification/classification-steps.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/classification/build-tile-ndvi.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/classification/compute-normal-scatter.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/classification/compute-near-building-mask.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/classification/classify-vegetation.c
)
add_library(dioramap_steps_classification STATIC ${CLASSIFICATION_SRCS})
target_include_directories(dioramap_steps_classification
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../steps/classification
    PRIVATE include
)
target_link_libraries(dioramap_steps_classification PUBLIC
    dioramap_runner dioramap_steps_registration dioramap_steps_ortho
    jansson PkgConfig::JPEG m)

# B7: ground + parcels (build-lidar-ground, build-parcel-lines).
set(GROUND_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/ground/ground-steps.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/ground/build-lidar-ground.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/ground/build-parcel-lines.c
)
add_library(dioramap_steps_ground STATIC ${GROUND_SRCS})
target_include_directories(dioramap_steps_ground
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../steps/ground
    PRIVATE include
)
target_link_libraries(dioramap_steps_ground PUBLIC
    dioramap_runner dioramap_steps_registration jansson m)

# Phase 1 of the roof-recon evaluation effort: extract-buildings step that
# slices the disc point cloud + cadastre footprints into per-building
# records. See v3/docs/research/roof-recon-survey.md.
set(BUILDINGS_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/buildings/buildings-steps.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/buildings/extract-buildings.c
)
add_library(dioramap_steps_buildings STATIC ${BUILDINGS_SRCS})
target_include_directories(dioramap_steps_buildings
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../steps/buildings
    PRIVATE include
)
target_link_libraries(dioramap_steps_buildings PUBLIC
    dioramap_runner jansson m)


# -----------------------------------------------------------------------------
# Phase 5 of the roof-recon evaluation effort: V7 pipeline port — bench-v2's
# evolved roof-reconstruction pipeline ported to v3 step by step. See
# v3/docs/plan/PHASE-5-V7-PORT.md.
#
# Links only dioramap_runner / jansson / m / dioramap_steps_normals. Gated on
# DIORAMAP_V7 (default ON).
# -----------------------------------------------------------------------------
if(DIORAMAP_V7)
add_library(dioramap_steps_v7 STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-steps.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-canary.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-derive-thresholds.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-tile-signals-impl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-tile-signals.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-load-real-test-case.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/build-building-kd-trees.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-lambert.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-histogram.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-hemisphere-segment-impl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-hemisphere-segment.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-plane-fit.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-normal-cascade-seed-impl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-normal-cascade-seed.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-ransac-plane.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-density-voids-2-impl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-density-voids-2.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-grow-refit-merge-impl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-grow-refit-merge.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-beam-incidence-impl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-beam-incidence.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-roof-graph-format.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-boundary-topology-impl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-boundary-topology.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-boundary-signals-impl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-boundary-signals.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-boundary-corners-impl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-boundary-corners.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-polygon-primitives.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-grid-contour.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-perbldg-gather.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-plane-grids-parse.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-corners-parse.c
    # v7-boundary-polygons (bp1) wrapper retired in #71, impl retired in
    # #76. v7-boundary-merge step retired in #76. Header
    # v7-boundary-merge-impl.h survives as a types-only stub for the
    # v7_bm_input_* records that classify / wall-detect / regularize
    # still consume.
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-boundary-polygons-2-impl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-boundary-polygons-2.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-corner-pool.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-boundary-classify-impl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-boundary-classify.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-wall-detect-impl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-wall-detect.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-boundary-regularize-impl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-boundary-regularize.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-score-intrinsic-impl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-score-intrinsic.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-score-intrinsic-postwall.c
)
target_include_directories(dioramap_steps_v7
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7
    PRIVATE include
            ${CMAKE_CURRENT_SOURCE_DIR}/../steps/normals  # kd_tree.h + pca.h
)
# Link against dioramap_steps_normals to reuse its v2-faithful kd-tree + PCA
# implementations (same algorithms, no duplication). PUBLIC because v7's
# public headers (v7-tile-signals-impl.h) expose `kd_tree_t` in the
# `v7_ts_compute` signature — dependents need the kd_tree.h header.
target_link_libraries(dioramap_steps_v7
    PUBLIC dioramap_runner jansson m dioramap_steps_normals)
if(DIORAMAP_V7_TRACE)
    target_compile_definitions(dioramap_steps_v7 PRIVATE V7_TRACE=1)
endif()
if(DIORAMAP_PLANE_FIT_AREA_WEIGHTED)
    target_compile_definitions(dioramap_steps_v7 PRIVATE V7_PLANE_FIT_AREA_WEIGHTED=1)
endif()

# v7-*-bench / v7-*-only-bench / v7-bench-v2-loaders all retired in
# task #76 (v2-divergence tooling cleanup). The byte-identical-with-v2
# parity tests were the only consumers; deleting v2-divergence let us
# drop them. v7_*_compute kernels keep their unit tests in core/tests/
# (driven by hand-crafted inputs, no v2 fixture coupling).

# Phase 3 — TLV machinery unit tests for v7-roof-graph-format.
add_executable(v7-roof-graph-format-test
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-roof-graph-format-test.c)
target_link_libraries(v7-roof-graph-format-test PRIVATE dioramap_steps_v7)
add_test(NAME v7_roof_graph_format COMMAND v7-roof-graph-format-test)

# Phase 5.7-rewrite — v7-boundary-polygons-2 tests.
add_executable(v7-bp2-test
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-bp2-test.c)
target_link_libraries(v7-bp2-test PRIVATE dioramap_steps_v7 jansson m)
add_test(NAME v7_bp2_test
         COMMAND v7-bp2-test ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fixtures/v7-boundary-polygons-2)

# Phase 5 — CLI inspector for roof-graph TLV files.
add_executable(v7-roof-graph-inspect
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/v7/v7-roof-graph-inspect.c)
target_link_libraries(v7-roof-graph-inspect PRIVATE dioramap_steps_v7)

# Stream B (task #73) — model-validity scorer. Compares a v3 v7
# pipeline output against a bench-v2 analytic ground truth. Reuses
# the roof-graph reader + `rg_plane_is_wall` classifier so the metric
# stays in lockstep with the pipeline; no parallel-language drift.
add_executable(v7mv-score-vs-gt
    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/v7-model-validity/tools/score-vs-gt.c)
target_link_libraries(v7mv-score-vs-gt PRIVATE dioramap_steps_v7 jansson)
endif() # DIORAMAP_V7 (dioramap_steps_v7)

# B8a + B8b: shadow primitives + sun-fit + raster-domain deshadow. The
# cast_shadow_kernel (used by both cast-shadows-from-dsm and the inner
# loop of find-best-sun-time) lives in shadow-cast-kernel.h as a self-
# contained inline function so a future .cu wrapper can dispatch it
# 1-to-1 onto a CUDA grid (per GPU-canary pattern in PLAN-VIEWER-PORT
# §11 / UNIFIED §7.6). sun-position.c is a pure SunCalc.js port (MIT,
# Vladimir Agafonkin) — only the getPosition path is vendored.
set(SHADOW_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/shadow/shadow-steps.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/shadow/detect-photo-shadows.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/shadow/cast-shadows-from-dsm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/shadow/find-best-sun-time.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/shadow/deshadow-ortho-rgb.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/shadow/sun-position.c
)
add_library(dioramap_steps_shadow STATIC ${SHADOW_SRCS})
target_include_directories(dioramap_steps_shadow
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../steps/shadow
    PRIVATE include
)
target_link_libraries(dioramap_steps_shadow PUBLIC
    dioramap_runner dioramap_steps_registration dioramap_steps_ortho jansson m)

set(DEMO_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/demo/demo.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/demo/scale-by-factor.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/demo/add-bias.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/demo/multiply.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/demo/report.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/demo/split-range.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/demo/fan-process.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/demo/reduce-sum.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/demo/cube-memoized.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/demo/stream-demo.c
)
add_library(dioramap_steps_demo STATIC ${DEMO_SRCS})
target_include_directories(dioramap_steps_demo
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../steps/demo
    PRIVATE include
)
target_link_libraries(dioramap_steps_demo PUBLIC
    dioramap_runner dioramap_cached_compute jansson)

# -----------------------------------------------------------------------------
# gpu-smoke canary (§7.6). Always compiles a CPU path; the .cu file is added
# only when DIORAMAP_CUDA=ON. Existence of this library guarantees the GPU
# wiring is exercised at build/test time regardless of where CUDA actually runs.
# -----------------------------------------------------------------------------
set(GPU_SMOKE_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/../steps/gpu-smoke/gpu-smoke.c)
if(DIORAMAP_CUDA)
    # The .cu file uses the same t() framework via shared headers; pass it
    # through the rewriter as well so device-side identifier strings get
    # tokenized in the binary alongside the host code.
    set(GPU_SMOKE_CU_SRCS
        ${CMAKE_CURRENT_SOURCE_DIR}/../steps/gpu-smoke/gpu-smoke.cu)
    list(APPEND GPU_SMOKE_SRCS ${GPU_SMOKE_CU_SRCS})
endif()
add_library(dioramap_steps_gpu_smoke STATIC ${GPU_SMOKE_SRCS})
target_include_directories(dioramap_steps_gpu_smoke
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../steps/gpu-smoke
    PRIVATE include
)
target_link_libraries(dioramap_steps_gpu_smoke PUBLIC dioramap_runner)
if(DIORAMAP_CUDA)
    target_compile_definitions(dioramap_steps_gpu_smoke PUBLIC DIORAMAP_CUDA=1)
    target_link_libraries(dioramap_steps_gpu_smoke PUBLIC CUDA::cudart)
endif()

# -----------------------------------------------------------------------------
# Main binary
# -----------------------------------------------------------------------------
add_library(dioramap_batch STATIC src/batch/batch_ctx.c)
target_include_directories(dioramap_batch PUBLIC include)
target_link_libraries(dioramap_batch PUBLIC
    dioramap_runner dioramap_store dioramap_hash Threads::Threads)

add_executable(dioramap_processor src/main.c)

target_include_directories(dioramap_processor PRIVATE include)
target_link_libraries(dioramap_processor PRIVATE
    dioramap_log dioramap_runner dioramap_store dioramap_server dioramap_fetch
    dioramap_batch dioramap_coords dioramap_lazperf
    dioramap_steps_demo dioramap_steps_gpu_smoke dioramap_steps_fetchers
    dioramap_steps_coords dioramap_steps_copc dioramap_steps_normals
    dioramap_steps_registration dioramap_steps_ortho
    dioramap_steps_classification dioramap_steps_ground
    dioramap_steps_shadow dioramap_steps_buildings)

# v7 roof-recon step lib — product port, linked whenever DIORAMAP_V7=ON
# (default). main.c gates v7's registration on the DIORAMAP_V7=1 compile def.
if(DIORAMAP_V7)
    target_link_libraries(dioramap_processor PRIVATE dioramap_steps_v7)
    target_compile_definitions(dioramap_processor PRIVATE DIORAMAP_V7=1)
endif()


target_compile_definitions(dioramap_processor PRIVATE
    DIORAMAP_VERSION="${PROJECT_VERSION}")

# Produce a hyphenated binary name matching the docs / CLI idiom
set_target_properties(dioramap_processor PROPERTIES
    OUTPUT_NAME "dioramap-processor")

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------
if(DIORAMAP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Fuzz targets are gated on DIORAMAP_FUZZ (clang only); tests/fuzz/CMakeLists
# returns early when the option is OFF.  Lives outside DIORAMAP_BUILD_TESTS
# so it can be enabled independently for fuzz-only builds.
add_subdirectory(tests/fuzz)

# -----------------------------------------------------------------------------
# Configuration summary
# -----------------------------------------------------------------------------
message(STATUS "")
message(STATUS "== Dioramap configuration ==")
message(STATUS "  Version:       ${PROJECT_VERSION}")
message(STATUS "  Build type:    ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler:      ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "  CUDA:          ${DIORAMAP_CUDA}")
message(STATUS "  ASan:          ${DIORAMAP_ASAN}")
message(STATUS "  TSan:          ${DIORAMAP_TSAN}")
message(STATUS "  Coverage:      ${DIORAMAP_COVERAGE}")
message(STATUS "  Fuzz:          ${DIORAMAP_FUZZ}")
message(STATUS "  Tests:         ${DIORAMAP_BUILD_TESTS}")
message(STATUS "  v7 roof-recon step lib (DIORAMAP_V7): ${DIORAMAP_V7}")
message(STATUS "")
