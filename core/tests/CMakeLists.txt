# Test scaffolding.
#
# Phase A1: one smoke test — the binary runs and prints a version line.
# The real unit/golden/integration harness arrives in the testing subsystem (§18).

# Note: ASan defaults for dioramap-processor and friends are baked into
# the binary via __asan_default_options() in src/main.c
# (verify_asan_link_order=0, detect_leaks=0). store_test sets ASAN_OPTIONS
# via ENVIRONMENT below because it exercises a deliberate-leak scenario.

add_test(
    NAME main_prints_version
    COMMAND $<TARGET_FILE:dioramap_processor> --version
)
set_tests_properties(main_prints_version PROPERTIES
    PASS_REGULAR_EXPRESSION "^dioramap-processor v"
)

add_test(
    NAME main_help
    COMMAND $<TARGET_FILE:dioramap_processor> --help
)
set_tests_properties(main_help PROPERTIES
    PASS_REGULAR_EXPRESSION "dioramap-processor"
)

add_test(
    NAME main_rejects_unknown_arg
    COMMAND $<TARGET_FILE:dioramap_processor> --frobnicate
)
set_tests_properties(main_rejects_unknown_arg PROPERTIES
    WILL_FAIL TRUE
)

# Main with no args prints a usage summary, exits 0.
add_test(
    NAME main_no_args_prints_usage
    COMMAND $<TARGET_FILE:dioramap_processor>
)
set_tests_properties(main_no_args_prints_usage PROPERTIES
    PASS_REGULAR_EXPRESSION "Subcommands"
)

# -----------------------------------------------------------------------------
# Batch mode end-to-end: run demo pipeline and verify expected report output.
# -----------------------------------------------------------------------------
add_test(
    NAME batch_demo_chain
    COMMAND ${CMAKE_COMMAND} -E env
        V3_PIPELINES_DIR=${CMAKE_SOURCE_DIR}/../pipelines
        sh -c "set -e; \
               OUT=$(mktemp -d /tmp/batch_demo_XXXXXX); \
               CACHE=$(mktemp -d /tmp/batch_cache_XXXXXX); \
               $<TARGET_FILE:dioramap_processor> batch \
                   --pipeline $V3_PIPELINES_DIR/smoke/demo-chain.json \
                   --input-json '{\"base\":5}' \
                   --produce report \
                   --output-dir $OUT \
                   --cache-dir $CACHE > /dev/null 2>&1; \
               cat $OUT/report.bin; \
               rm -rf $OUT $CACHE"
)
# Expect the report JSON with the correct math (scaled=15, biased=12, product=180).
set_tests_properties(batch_demo_chain PROPERTIES
    PASS_REGULAR_EXPRESSION "\"biased\":12.*\"product\":180.*\"scaled\":15"
)

# gpu-smoke canary: input bytes must equal output bytes (CPU path; CUDA path
# only runs if the build was configured with DIORAMAP_CUDA=ON).
add_test(
    NAME batch_gpu_smoke
    COMMAND ${CMAKE_COMMAND} -E env
        V3_PIPELINES_DIR=${CMAKE_SOURCE_DIR}/../pipelines
        sh -c "set -e; \
               OUT=$(mktemp -d /tmp/batch_smoke_XXXXXX); \
               CACHE=$(mktemp -d /tmp/batch_smoke_cache_XXXXXX); \
               $<TARGET_FILE:dioramap_processor> batch \
                   --pipeline $V3_PIPELINES_DIR/smoke/gpu-smoke.json \
                   --input-json '\"gpu-smoke-canary-payload\"' \
                   --produce smoke-output \
                   --output-dir $OUT \
                   --cache-dir $CACHE > /dev/null 2>&1; \
               cat $OUT/smoke-output.bin; \
               rm -rf $OUT $CACHE"
)
set_tests_properties(batch_gpu_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "gpu-smoke-canary-payload"
)

# T13 array artifacts: run array-demo with count=5.
# Expected: sum of i² for i in [0..4] = 0+1+4+9+16 = 30.
# Second invocation with same cache reuses everything → still prints 30.
add_test(
    NAME batch_array_demo
    COMMAND ${CMAKE_COMMAND} -E env
        V3_PIPELINES_DIR=${CMAKE_SOURCE_DIR}/../pipelines
        sh -c "set -e; \
               OUT=$(mktemp -d /tmp/batch_array_XXXXXX); \
               CACHE=$(mktemp -d /tmp/batch_array_cache_XXXXXX); \
               RUN() { $<TARGET_FILE:dioramap_processor> batch \
                           --pipeline $V3_PIPELINES_DIR/smoke/array-demo.json \
                           --input-json '{\"count\":5}' \
                           --produce square-sum \
                           --output-dir $OUT \
                           --cache-dir $CACHE > /dev/null 2>&1; }; \
               RUN; \
               FIRST=$(od -An -td8 -N8 < $OUT/square-sum.bin | tr -d ' '); \
               rm -rf $OUT; OUT=$(mktemp -d /tmp/batch_array_XXXXXX); \
               RUN; \
               SECOND=$(od -An -td8 -N8 < $OUT/square-sum.bin | tr -d ' '); \
               echo FIRST=$FIRST SECOND=$SECOND; \
               rm -rf $OUT $CACHE"
)
set_tests_properties(batch_array_demo PROPERTIES
    PASS_REGULAR_EXPRESSION "FIRST=30 SECOND=30"
)

# B3.5 sub-scope cached_compute demo. cube-memoized uses cached_compute
# internally with a namespaced ("cube", i) identity per element. Running the
# pipeline back-to-back against the same input should produce:
#   - First run: N body executions (N cube computations).
#   - Second run against the SAME cache: zero body executions — step-level
#     intent map hits.
# This test just verifies correctness + stable output across runs; the
# unit-level zero-exec assertion lives in runner_test.c.
add_test(
    NAME batch_cube_memoized_demo
    COMMAND ${CMAKE_COMMAND} -E env
        V3_PIPELINES_DIR=${CMAKE_SOURCE_DIR}/../pipelines
        sh -c "set -e; \
               OUT=$(mktemp -d /tmp/batch_cube_XXXXXX); \
               CACHE=$(mktemp -d /tmp/batch_cube_cache_XXXXXX); \
               RUN() { $<TARGET_FILE:dioramap_processor> batch \
                           --pipeline $V3_PIPELINES_DIR/smoke/cube-memoized-demo.json \
                           --input-json '{\"count\":4}' \
                           --produce cube-array \
                           --output-dir $OUT \
                           --cache-dir $CACHE > /dev/null 2>&1; }; \
               RUN; \
               N=$(od -An -td4 -N4 < $OUT/cube-array.bin | tr -d ' '); \
               test \"$N\" = \"4\" || { echo 'ERR: n != 4'; exit 1; }; \
               rm -rf $OUT; OUT=$(mktemp -d /tmp/batch_cube_XXXXXX); \
               RUN; \
               N2=$(od -An -td4 -N4 < $OUT/cube-array.bin | tr -d ' '); \
               test \"$N2\" = \"4\" || { echo 'ERR: n2 != 4'; exit 1; }; \
               echo OK cube-memoized; \
               rm -rf $OUT $CACHE"
)
set_tests_properties(batch_cube_memoized_demo PROPERTIES
    PASS_REGULAR_EXPRESSION "OK cube-memoized"
)

# -----------------------------------------------------------------------------
# Logging unit tests (A2)
# -----------------------------------------------------------------------------
add_executable(log_test log_test.c)
target_link_libraries(log_test PRIVATE dioramap_log)
add_test(NAME log_test COMMAND log_test)

# -----------------------------------------------------------------------------
# Runtime unit tests (A3)
# -----------------------------------------------------------------------------
add_executable(thread_pool_test thread_pool_test.c)
target_link_libraries(thread_pool_test PRIVATE dioramap_runtime)
add_test(NAME thread_pool_test COMMAND thread_pool_test)

add_executable(refcount_test refcount_test.c)
target_link_libraries(refcount_test PRIVATE dioramap_runtime)
add_test(NAME refcount_test COMMAND refcount_test)

# -----------------------------------------------------------------------------
# Artifact layout + hash + mmap tests (A4)
# -----------------------------------------------------------------------------
add_executable(hash_test hash_test.c)
target_link_libraries(hash_test PRIVATE dioramap_hash)
add_test(NAME hash_test COMMAND hash_test)

add_executable(artifact_test artifact_test.c)
target_link_libraries(artifact_test PRIVATE dioramap_artifact)
add_test(NAME artifact_test COMMAND artifact_test)

add_executable(mmap_file_test mmap_file_test.c)
target_link_libraries(mmap_file_test PRIVATE dioramap_artifact)
add_test(NAME mmap_file_test COMMAND mmap_file_test)

# -----------------------------------------------------------------------------
# Store test (A5)
# -----------------------------------------------------------------------------
add_executable(store_test store_test.c)
target_link_libraries(store_test PRIVATE dioramap_store dioramap_hash Threads::Threads)
add_test(NAME store_test COMMAND store_test)
# store_test exercises the "survive unclosed close" path which leaks by
# design (the test scenario simulates a crash mid-write). Disable ASan
# leak detection for this test specifically; other tests still detect leaks.
set_tests_properties(store_test PROPERTIES
    ENVIRONMENT "ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=0"
)

# -----------------------------------------------------------------------------
# Intent map test (B3.5 / HASH-MODEL.md §6)
# -----------------------------------------------------------------------------
add_executable(intent_map_test intent_map_test.c)
target_link_libraries(intent_map_test PRIVATE dioramap_intent_map Threads::Threads)
add_test(NAME intent_map_test COMMAND intent_map_test)

# -----------------------------------------------------------------------------
# cached_compute test (B3.5 / HASH-MODEL.md §3)
# -----------------------------------------------------------------------------
add_executable(cached_compute_test cached_compute_test.c)
target_link_libraries(cached_compute_test PRIVATE dioramap_cached_compute Threads::Threads)
add_test(NAME cached_compute_test COMMAND cached_compute_test)

# -----------------------------------------------------------------------------
# Step-output manifest packer/walker test (B3.5 / HASH-MODEL.md §3)
# -----------------------------------------------------------------------------
add_executable(step_manifest_test
    step_manifest_test.c
    ${CMAKE_SOURCE_DIR}/src/runner/step_manifest.c
)
target_include_directories(step_manifest_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/runner)
target_link_libraries(step_manifest_test PRIVATE dioramap_hash)
add_test(NAME step_manifest_test COMMAND step_manifest_test)

# -----------------------------------------------------------------------------
# ARRAY_INLINE round-trip test (Phase 5.1d). Covers writer→reader byte
# round-trip + 8-byte alignment of variable-length record payloads. The
# alignment assertion is the test that would have surfaced the original
# kd_node_t double-misalignment bug at first run.
# -----------------------------------------------------------------------------
add_executable(array_inline_test array_inline_test.c)
target_link_libraries(array_inline_test PRIVATE dioramap_runner)
add_test(NAME array_inline_test COMMAND array_inline_test)

# -----------------------------------------------------------------------------
# B4 compute-normals unit + integration tests (kd-tree, PCA, end-to-end).
# -----------------------------------------------------------------------------
add_executable(normals_test normals_test.c)
target_link_libraries(normals_test PRIVATE
    dioramap_steps_normals dioramap_runner m)
add_test(NAME normals_test COMMAND normals_test)

# B5a registration step tests (build-lidar-rasters, compute-ndsm,
# compute-building-mask) — synthetic geometry + chain rerun caching.
add_executable(registration_test registration_test.c)
target_link_libraries(registration_test PRIVATE
    dioramap_steps_registration dioramap_runner m)
add_test(NAME registration_test COMMAND registration_test)

# B5b ortho raster step tests (JPEG decode + resample + ortho-{rgb,irc}-rasters).
add_executable(ortho_test ortho_test.c)
target_link_libraries(ortho_test PRIVATE
    dioramap_steps_ortho dioramap_steps_registration dioramap_runner
    PkgConfig::JPEG m)
add_test(NAME ortho_test COMMAND ortho_test)

# B5c coregister-ortho-lidar tests (edges primitives + synthetic offset recovery).
add_executable(coregister_test coregister_test.c)
target_include_directories(coregister_test PRIVATE
    ${CMAKE_SOURCE_DIR}/../steps/registration
    ${CMAKE_SOURCE_DIR}/../steps/ortho)
target_link_libraries(coregister_test PRIVATE
    dioramap_steps_registration dioramap_steps_ortho dioramap_runner jansson m)
add_test(NAME coregister_test COMMAND coregister_test)

# B5d cadastre tests (rasterize-cadastre-edges + fit-cadastre-to-lidar).
add_executable(cadastre_test cadastre_test.c)
target_include_directories(cadastre_test PRIVATE
    ${CMAKE_SOURCE_DIR}/../steps/registration)
target_link_libraries(cadastre_test PRIVATE
    dioramap_steps_registration dioramap_runner jansson m)
add_test(NAME cadastre_test COMMAND cadastre_test)

# B6 classification tests (tile-ndvi, normal-scatter, near-building, classify-veg).
add_executable(classification_test classification_test.c)
target_include_directories(classification_test PRIVATE
    ${CMAKE_SOURCE_DIR}/../steps/classification
    ${CMAKE_SOURCE_DIR}/../steps/registration)
target_link_libraries(classification_test PRIVATE
    dioramap_steps_classification dioramap_steps_registration dioramap_steps_ortho
    dioramap_runner PkgConfig::JPEG jansson m)
add_test(NAME classification_test COMMAND classification_test)

# B7 ground tests (build-lidar-ground, build-parcel-lines).
add_executable(ground_test ground_test.c)
target_include_directories(ground_test PRIVATE
    ${CMAKE_SOURCE_DIR}/../steps/ground
    ${CMAKE_SOURCE_DIR}/../steps/registration)
target_link_libraries(ground_test PRIVATE
    dioramap_steps_ground dioramap_steps_registration dioramap_runner jansson m)
add_test(NAME ground_test COMMAND ground_test)

# B8a shadow primitive tests (detect-photo-shadows, cast-shadows-from-dsm).
add_executable(shadow_test shadow_test.c)
target_include_directories(shadow_test PRIVATE
    ${CMAKE_SOURCE_DIR}/../steps/shadow
    ${CMAKE_SOURCE_DIR}/../steps/registration
    ${CMAKE_SOURCE_DIR}/../steps/ortho)
target_link_libraries(shadow_test PRIVATE
    dioramap_steps_shadow dioramap_steps_ortho dioramap_steps_registration
    dioramap_runner jansson m)
add_test(NAME shadow_test COMMAND shadow_test)

# B10 surround-fetcher subscription-gating: assert that fetch-ortho-surround
# and fetch-elevation-surround are pruned from DAG plans whose subscribers
# don't depend on them. No network I/O — runner's on_run_started fires with
# the plan BEFORE execution; bad-input makes canonicalize-location fail
# so fetchers never dispatch.
add_executable(surround_gating_test surround_gating_test.c)
target_link_libraries(surround_gating_test PRIVATE
    dioramap_steps_coords dioramap_steps_fetchers dioramap_runner)
add_test(NAME surround_gating_test COMMAND surround_gating_test)

# -----------------------------------------------------------------------------
# Fetch test (A9 / B1): spin up a Python http.server serving tests/fixtures
# on a random port, run fetch_test against it, tear down.
# -----------------------------------------------------------------------------
add_executable(fetch_test fetch_test.c)
target_link_libraries(fetch_test PRIVATE dioramap_fetch dioramap_store dioramap_intent_map dioramap_hash)

add_executable(coords_test coords_test.c)
target_link_libraries(coords_test PRIVATE dioramap_coords)
add_test(NAME coords_test COMMAND coords_test)

# Online smoke against real IGN endpoints. Gated on DIORAMAP_TEST_IGN=1 so
# regular builds don't depend on network reachability. To run locally:
#   DIORAMAP_TEST_IGN=1 ctest -R batch_viewer_fetchers --output-on-failure
add_test(
    NAME batch_viewer_fetchers
    COMMAND ${CMAKE_COMMAND} -E env
        V3_PIPELINES_DIR=${CMAKE_SOURCE_DIR}/../pipelines
        sh -c "if [ \"$DIORAMAP_TEST_IGN\" != 1 ]; then \
                   echo 'SKIP: set DIORAMAP_TEST_IGN=1 to run (needs data.geopf.fr)'; \
                   exit 0; \
               fi; \
               set -e; \
               OUT=$(mktemp -d /tmp/viewer_fetch_XXXXXX); \
               CACHE=$(mktemp -d /tmp/viewer_fetch_cache_XXXXXX); \
               trap 'rm -rf $OUT $CACHE' EXIT; \
               INPUT='{\"lat\":48.8566,\"lon\":2.3522,\"radius_m\":75}'; \
               $<TARGET_FILE:dioramap_processor> batch \
                   --pipeline $V3_PIPELINES_DIR/smoke/viewer-fetchers.json \
                   --input-json \"$INPUT\" \
                   --produce canonical-location,ortho-rgb-jpeg,elevation-f32,scene-origin \
                   --output-dir $OUT \
                   --cache-dir $CACHE; \
               grep -q '\"center_l93_x\"' $OUT/canonical-location.bin; \
               # ortho-rgb-jpeg is raw JPEG — check the 3-byte magic FFD8FF.
               head -c 3 $OUT/ortho-rgb-jpeg.bin | od -An -tx1 | tr -d ' ' | grep -q '^ffd8ff'; \
               # elevation-f32 is a raw Float32 grid — verify byte count matches
               # grid_size^2 * 4 (default grid_size=128 here).
               test $(stat -c%s $OUT/elevation-f32.bin) -eq $((128*128*4)); \
               grep -q '\"min_z\"' $OUT/scene-origin.bin; \
               # Second run must cache-hit.
               $<TARGET_FILE:dioramap_processor> batch \
                   --pipeline $V3_PIPELINES_DIR/smoke/viewer-fetchers.json \
                   --input-json \"$INPUT\" \
                   --produce scene-origin \
                   --output-dir $OUT \
                   --cache-dir $CACHE 2>&1 | tee /dev/stderr | grep -q 'cache hit'"
)
set_tests_properties(batch_viewer_fetchers PROPERTIES
    PASS_REGULAR_EXPRESSION "(SKIP|cache hit)"
)

# B3 end-to-end: coords → disc-filtered LiDAR point cloud.
# Cold run downloads multi-hundred-MB COPC tiles — gated on DIORAMAP_TEST_IGN
# AND a separate DIORAMAP_TEST_COPC flag so the faster ortho/elevation live
# test doesn't drag the point cloud into every live run. To invoke:
#   DIORAMAP_TEST_IGN=1 DIORAMAP_TEST_COPC=1 ctest -R batch_viewer_pointcloud --output-on-failure
add_test(
    NAME batch_viewer_pointcloud
    COMMAND ${CMAKE_COMMAND} -E env
        V3_PIPELINES_DIR=${CMAKE_SOURCE_DIR}/../pipelines
        sh -c "if [ \"$DIORAMAP_TEST_IGN\" != 1 ] || [ \"$DIORAMAP_TEST_COPC\" != 1 ]; then \
                   echo 'SKIP: set DIORAMAP_TEST_IGN=1 DIORAMAP_TEST_COPC=1 to run (downloads 100+ MB)'; \
                   exit 0; \
               fi; \
               set -e; \
               OUT=$(mktemp -d /tmp/viewer_pc_XXXXXX); \
               CACHE=$(mktemp -d /tmp/viewer_pc_cache_XXXXXX); \
               trap 'rm -rf $OUT $CACHE' EXIT; \
               INPUT='{\"lat\":48.8566,\"lon\":2.3522,\"radius_m\":50}'; \
               $<TARGET_FILE:dioramap_processor> batch \
                   --pipeline $V3_PIPELINES_DIR/smoke/viewer-pointcloud.json \
                   --input-json \"$INPUT\" \
                   --produce lidar-positions,lidar-classifications \
                   --output-dir $OUT \
                   --cache-dir $CACHE; \
               # Positions are F64 interleaved (x,y,z). Must be divisible by 24 bytes.
               POS=$(stat -c%s $OUT/lidar-positions.bin); \
               CLS=$(stat -c%s $OUT/lidar-classifications.bin); \
               echo \"points = $((POS/24)) positions_bytes=$POS classifications=$CLS\"; \
               test $((POS/24)) -eq $CLS || { echo 'ERR: point count mismatch'; exit 1; }; \
               test $CLS -gt 10000 || { echo 'ERR: suspiciously few points'; exit 1; }"
)
set_tests_properties(batch_viewer_pointcloud PROPERTIES
    PASS_REGULAR_EXPRESSION "(SKIP|points = [0-9]+ positions_bytes=)"
)

# B4 end-to-end: extract disc + compute per-point normals.
# Same gating as batch_viewer_pointcloud (downloads >100 MB COPC).
# Validates: normals byte-count = positions byte-count × 1/2 (F32 vs F64),
# and a sample of normals sit on the unit sphere within 1e-3.
add_test(
    NAME batch_viewer_pointcloud_normals
    COMMAND ${CMAKE_COMMAND} -E env
        V3_PIPELINES_DIR=${CMAKE_SOURCE_DIR}/../pipelines
        sh -c "if [ \"$DIORAMAP_TEST_IGN\" != 1 ] || [ \"$DIORAMAP_TEST_COPC\" != 1 ]; then \
                   echo 'SKIP: set DIORAMAP_TEST_IGN=1 DIORAMAP_TEST_COPC=1 to run'; \
                   exit 0; \
               fi; \
               set -e; \
               OUT=$(mktemp -d /tmp/normals_pc_XXXXXX); \
               CACHE=$(mktemp -d /tmp/normals_pc_cache_XXXXXX); \
               trap 'rm -rf $OUT $CACHE' EXIT; \
               INPUT='{\"lat\":48.8566,\"lon\":2.3522,\"radius_m\":50}'; \
               $<TARGET_FILE:dioramap_processor> batch \
                   --pipeline $V3_PIPELINES_DIR/smoke/viewer-pointcloud-with-normals.json \
                   --input-json \"$INPUT\" \
                   --produce lidar-positions,lidar-normals \
                   --output-dir $OUT \
                   --cache-dir $CACHE; \
               POS=$(stat -c%s $OUT/lidar-positions.bin); \
               NRM=$(stat -c%s $OUT/lidar-normals.bin); \
               N_POS=$((POS/24)); \
               N_NRM=$((NRM/12)); \
               echo \"points=$N_POS normals=$N_NRM\"; \
               test \"$N_POS\" = \"$N_NRM\" || { echo 'count mismatch'; exit 1; }; \
               test $N_POS -gt 1000 || { echo 'too few points'; exit 1; }; \
               python3 -c 'import struct, sys; \
b=open(sys.argv[1],\"rb\").read(); n=len(b)//12; bad=0; \
for i in range(min(n,500)): \
 nx,ny,nz=struct.unpack_from(\"<fff\",b,i*12); \
 m=(nx*nx+ny*ny+nz*nz)**0.5; \
 if abs(m-1.0)>1e-3: bad+=1; \
print(f\"unit-norm checks: {500-bad}/500 within 1e-3\"); \
sys.exit(0 if bad<=5 else 1)' $OUT/lidar-normals.bin"
)
set_tests_properties(batch_viewer_pointcloud_normals PROPERTIES
    PASS_REGULAR_EXPRESSION "(SKIP|points=[0-9]+ normals=)"
)

# B5a end-to-end: extract disc + LiDAR rasters + NDSM + building mask.
# Validates: header parses correctly; building-mask cells are 0 or 1;
# non-trivial fraction of cells classified as building.
add_test(
    NAME batch_viewer_rasters
    COMMAND ${CMAKE_COMMAND} -E env
        V3_PIPELINES_DIR=${CMAKE_SOURCE_DIR}/../pipelines
        sh -c "if [ \"$DIORAMAP_TEST_IGN\" != 1 ] || [ \"$DIORAMAP_TEST_COPC\" != 1 ]; then \
                   echo 'SKIP: set DIORAMAP_TEST_IGN=1 DIORAMAP_TEST_COPC=1 to run'; \
                   exit 0; \
               fi; \
               set -e; \
               OUT=$(mktemp -d /tmp/rasters_XXXXXX); \
               CACHE=$(mktemp -d /tmp/rasters_cache_XXXXXX); \
               trap 'rm -rf $OUT $CACHE' EXIT; \
               INPUT='{\"lat\":48.8566,\"lon\":2.3522,\"radius_m\":50}'; \
               $<TARGET_FILE:dioramap_processor> batch \
                   --pipeline $V3_PIPELINES_DIR/smoke/viewer-pointcloud-with-rasters.json \
                   --input-json \"$INPUT\" \
                   --produce lidar-rasters,lidar-ndsm,lidar-building-mask \
                   --output-dir $OUT \
                   --cache-dir $CACHE; \
               python3 -c 'import struct, sys, math; \
def hdr(p): \
 b=open(p,\"rb\").read(); \
 cols,rows,nch=struct.unpack(\"<III\",b[:12]); \
 return b,cols,rows,nch; \
b,cols,rows,nch=hdr(sys.argv[1]); \
print(f\"lidar-rasters: {cols}x{rows} channels={nch}\"); \
assert nch==7; \
assert len(b)==12+cols*rows*nch*4; \
b2,c2,r2,n2=hdr(sys.argv[2]); \
print(f\"lidar-ndsm: {c2}x{r2} channels={n2}\"); \
assert (c2,r2,n2)==(cols,rows,1); \
b3,c3,r3,n3=hdr(sys.argv[3]); \
print(f\"lidar-building-mask: {c3}x{r3} channels={n3}\"); \
assert (c3,r3,n3)==(cols,rows,1); \
mask=struct.unpack(f\"<{cols*rows}f\", b3[12:]); \
n_b=sum(1 for v in mask if v==1.0); \
n_g=sum(1 for v in mask if v==0.0); \
print(f\"building cells: {n_b}/{cols*rows} ({100*n_b/(cols*rows):.1f}%)\"); \
assert n_b+n_g==cols*rows; \
assert 0.05 < n_b/(cols*rows) < 0.95, \"building fraction sanity\"; \
print(\"OK rasters\")' $OUT/lidar-rasters.bin $OUT/lidar-ndsm.bin $OUT/lidar-building-mask.bin"
)
set_tests_properties(batch_viewer_rasters PROPERTIES
    PASS_REGULAR_EXPRESSION "(SKIP|OK rasters)"
)

# B5b end-to-end: ortho RGB + IRC rasters at the LiDAR grid resolution.
# Gated on DIORAMAP_TEST_IGN only (no COPC dependency, just WMS fetches).
add_test(
    NAME batch_viewer_ortho
    COMMAND ${CMAKE_COMMAND} -E env
        V3_PIPELINES_DIR=${CMAKE_SOURCE_DIR}/../pipelines
        sh -c "if [ \"$DIORAMAP_TEST_IGN\" != 1 ]; then \
                   echo 'SKIP: set DIORAMAP_TEST_IGN=1 to run'; \
                   exit 0; \
               fi; \
               set -e; \
               OUT=$(mktemp -d /tmp/ortho_e2e_XXXXXX); \
               CACHE=$(mktemp -d /tmp/ortho_e2e_cache_XXXXXX); \
               trap 'rm -rf $OUT $CACHE' EXIT; \
               INPUT='{\"lat\":48.8566,\"lon\":2.3522,\"radius_m\":50}'; \
               $<TARGET_FILE:dioramap_processor> batch \
                   --pipeline $V3_PIPELINES_DIR/smoke/viewer-ortho-rasters.json \
                   --input-json \"$INPUT\" \
                   --produce ortho-rgb-rasters,ortho-irc-rasters \
                   --output-dir $OUT \
                   --cache-dir $CACHE; \
               python3 -c 'import struct,sys; \
def hd(p):  b=open(p,\"rb\").read(); cols,rows,nch=struct.unpack(\"<III\",b[:12]); return cols,rows,nch,len(b); \
c1,r1,n1,L1=hd(sys.argv[1]); print(f\"ortho-rgb-rasters: {c1}x{r1} channels={n1}\"); assert n1==7 and L1==12+c1*r1*7*4; \
c2,r2,n2,L2=hd(sys.argv[2]); print(f\"ortho-irc-rasters: {c2}x{r2} channels={n2}\"); assert n2==2 and L2==12+c2*r2*2*4 and (c2,r2)==(c1,r1); \
print(\"OK ortho\")' $OUT/ortho-rgb-rasters.bin $OUT/ortho-irc-rasters.bin"
)
set_tests_properties(batch_viewer_ortho PROPERTIES
    PASS_REGULAR_EXPRESSION "(SKIP|OK ortho)"
)

add_test(
    NAME fetch_test
    COMMAND sh -c "set -e; \
        cd ${CMAKE_CURRENT_SOURCE_DIR}/fixtures; \
        PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind((\"127.0.0.1\",0)); print(s.getsockname()[1]); s.close()'); \
        CACHE=$(mktemp -d /tmp/fetch_test_cache_XXXXXX); \
        python3 -u -m http.server --bind 127.0.0.1 $PORT > /dev/null 2>&1 & \
        PID=$!; \
        trap 'kill $PID 2>/dev/null; rm -rf $CACHE' EXIT; \
        for i in $(seq 1 50); do \
            curl -sf http://127.0.0.1:$PORT/hello.txt > /dev/null && break; \
            sleep 0.05; \
        done; \
        TEST_URL=http://127.0.0.1:$PORT TEST_CACHE_DIR=$CACHE \
            $<TARGET_FILE:fetch_test>"
)

# -----------------------------------------------------------------------------
# Runner + pipeline test (A6)
# -----------------------------------------------------------------------------
add_executable(runner_test runner_test.c)
target_link_libraries(runner_test PRIVATE dioramap_runner)
add_test(NAME runner_test COMMAND runner_test)
# Expose the path to v3/pipelines/ so the test can load the smoke JSON.
#
# Historical note (#82 / #97): a duplicate-submission race used to make
# sum-two get enqueued twice in test_*_second_run under ctest -j load,
# producing n_cache_hits=4 instead of 3. Root cause was in runner_run's
# initial-ready loop, which re-checked rs.in_degree[i]==0 across the
# unlock window inside submit_step_task_locked — peer workers could
# decrement in_degree of plan entries we hadn't iterated to yet, so
# main would submit them a second time. Fixed in #97 by snapshotting
# the initial-ready set under a single lock acquisition; RUN_SERIAL
# is no longer needed here.
set_tests_properties(runner_test PROPERTIES
    ENVIRONMENT "V3_PIPELINES_DIR=${CMAKE_SOURCE_DIR}/../pipelines")

# Every test that drives runner_run() spins up its own internal
# thread_pool (sized to nproc by default). Running them concurrently
# under `ctest -j $(nproc)` over-subscribes cores by nproc² and
# destabilises the runner's intent-map / cached_compute timing —
# observed:
#   - runner_test.c:1189: c2.n_cache_hits == 3            (ctest-clang)
#   - classification_test.c:512: runner_run == 0          (ctest-asan)
#   - registration_test.c:315: run_chain == 0             (ctest-clang)
# all standalone-passes-5/5, fail under load.
#
# First tried RESOURCE_LOCK "dioramap_runner_thread_pool" — that
# serialises runner-tests with EACH OTHER but lets them still
# co-run with non-runner tests; on a 4-core box with `ctest -j 4`
# that still leaves the runner-test fighting for ~1 core against 3
# other concurrent tests, and the flake came back as
# registration_test in tier-full#3.
#
# Stepping up to RUN_SERIAL: each of these waits until the rest of
# the suite is quiet, then runs alone — full machine for the few
# seconds it needs. Adds ~11 × 1s of serialised time to ctest wall,
# net cost ~10 s on a tier that's dominated by the 13-minute
# Frama-C step. Worth it for deterministic green.
#
# Deep fix (task #82): teach the runner's thread_pool to scale down
# when DIORAMAP_CTEST_RUN is in env, so this becomes unnecessary.
set_tests_properties(
    classification_test
    array_inline_test
    cadastre_test
    ground_test
    surround_gating_test
    coregister_test
    normals_test
    ortho_test
    shadow_test
    registration_test
    PROPERTIES
    RUN_SERIAL TRUE
)

# -----------------------------------------------------------------------------
# Serve mode end-to-end (A7): spawn the server, connect via WS, run demo-chain
# and verify the full protocol envelope + binary artifact delivery.
# -----------------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(
        NAME serve_smoke
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/serve_smoke.py
    )
    set_tests_properties(serve_smoke PROPERTIES
        ENVIRONMENT
        "DIORAMAP_BIN=$<TARGET_FILE:dioramap_processor>;V3_PIPELINES_DIR=${CMAKE_SOURCE_DIR}/../pipelines"
    )

    add_test(
        NAME serve_full
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/serve_full.py
    )
    set_tests_properties(serve_full PROPERTIES
        ENVIRONMENT
        "DIORAMAP_BIN=$<TARGET_FILE:dioramap_processor>;V3_PIPELINES_DIR=${CMAKE_SOURCE_DIR}/../pipelines"
    )

    # B9: STREAM_CHUNK protocol coverage. Drives stream-demo via WS and
    # asserts chunks arrive in order (with first chunk landing well under
    # the §11 B9 "first-pixel within 500ms" budget).
    add_test(
        NAME serve_stream
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/serve_stream.py
    )
    set_tests_properties(serve_stream PROPERTIES
        ENVIRONMENT
        "DIORAMAP_BIN=$<TARGET_FILE:dioramap_processor>;V3_PIPELINES_DIR=${CMAKE_SOURCE_DIR}/../pipelines"
    )

    # Optional shared-token auth: no-token mode, rejected connects (incl. a
    # RUN fired before auth completes), and the accepted-token path.
    add_test(
        NAME serve_auth
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/serve_auth.py
    )
    set_tests_properties(serve_auth PROPERTIES
        ENVIRONMENT
        "DIORAMAP_BIN=$<TARGET_FILE:dioramap_processor>;V3_PIPELINES_DIR=${CMAKE_SOURCE_DIR}/../pipelines"
    )

    # Processor extension seam (v3/extension.h): the same serve binary linked
    # against a fake extension — auth outcomes, public-only access, run
    # reports for successful and failed runs, init refusal, shutdown.
    add_executable(dioramap_processor_fakeext ${CMAKE_SOURCE_DIR}/src/main.c fake_extension.c)
    target_include_directories(dioramap_processor_fakeext PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(dioramap_processor_fakeext PRIVATE ${DIORAMAP_PROCESSOR_LIBS} jansson)
    target_compile_definitions(dioramap_processor_fakeext PRIVATE ${DIORAMAP_PROCESSOR_DEFS})
    add_test(
        NAME serve_extension
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/serve_extension.py
    )
    set_tests_properties(serve_extension PROPERTIES
        ENVIRONMENT
        "DIORAMAP_BIN=$<TARGET_FILE:dioramap_processor_fakeext>;V3_PIPELINES_DIR=${CMAKE_SOURCE_DIR}/../pipelines"
    )
endif()

# -----------------------------------------------------------------------------
# v7 implementation tests — one per step (+ kd-tree).
#
# These are *implementation conformance* tests as defined in
# docs/quality/V7-TEST-STRATEGY.md.  Each <step>_test.c file includes the
# matching v7-*-impl.c directly to access its file-static helpers, then
# asserts spec invariants ported from bench-v2's test/<step>.test.js.
#
# New test files appear automatically when committed; re-run cmake after
# adding a new v7_<step>_test.c to register it.
# -----------------------------------------------------------------------------
foreach(_v7step IN ITEMS
        tile_signals hemisphere_segment normal_cascade_seed density_voids_2
        grow_refit_merge beam_incidence boundary_topology boundary_signals
        boundary_corners boundary_classify   # boundary_merge retired in #76
        wall_detect boundary_regularize score_intrinsic
        kd_tree polygon_primitives grid_contour
        perbldg_gather   # task #72a — extracted gather helper
        rg_parse)        # task #72b — extracted rg_parse_padded_u32_array helper
    set(_v7_src ${CMAKE_CURRENT_SOURCE_DIR}/v7_${_v7step}_test.c)
    if(EXISTS ${_v7_src})
        # Some impls #include main's shared v7 helper sources, which define
        # non-static functions the impl calls (e.g. boundary-signals →
        # grid-contour). dioramap_steps_normals doesn't carry those, so the
        # helper .c must be compiled into the test's TU set directly.
        # (boundary_polygons / bp1 was retired in #71; bp1's
        # v7-polygon-primitives extra-link sits with bp2's test now.)
        set(_v7_extra "")
        if(_v7step STREQUAL "boundary_signals")
            list(APPEND _v7_extra ${CMAKE_SOURCE_DIR}/../steps/v7/v7-grid-contour.c)
        elseif(_v7step STREQUAL "boundary_regularize")
            # T5: the impl now #includes the corner-pool solver
            # (v7p_solve_member_position) + the bp2 local self-int test
            # (v7_local_edges_cross_uv); compile those TUs into the test set.
            list(APPEND _v7_extra ${CMAKE_SOURCE_DIR}/../steps/v7/v7-corner-pool.c)
            list(APPEND _v7_extra ${CMAKE_SOURCE_DIR}/../steps/v7/v7-boundary-polygons-2-impl.c)
            # bp2-impl pulls v7_pt_in_poly/v7_pt_edge_dist from polygon-primitives.
            list(APPEND _v7_extra ${CMAKE_SOURCE_DIR}/../steps/v7/v7-polygon-primitives.c)
        elseif(_v7step STREQUAL "perbldg_gather")
            # #72a: the test calls the helper directly, no impl-included; just
            # link the helper's TU.
            list(APPEND _v7_extra ${CMAKE_SOURCE_DIR}/../steps/v7/v7-perbldg-gather.c)
        elseif(_v7step STREQUAL "rg_parse")
            # #72b: helper lives in v7-roof-graph-format.c.
            list(APPEND _v7_extra ${CMAKE_SOURCE_DIR}/../steps/v7/v7-roof-graph-format.c)
        endif()
        add_executable(v7_${_v7step}_test ${_v7_src} ${_v7_extra})
        target_include_directories(v7_${_v7step}_test PRIVATE
            ${CMAKE_SOURCE_DIR}/../steps/v7
            ${CMAKE_SOURCE_DIR}/../steps/normals
            ${CMAKE_SOURCE_DIR}/include)
        # We include the impl source directly to reach file-statics; link
        # only what the impl pulls in transitively (PCA + kd-tree from
        # steps/normals, math).  Don't link dioramap_steps_v7 — that would
        # duplicate the impl's public symbols.
        target_link_libraries(v7_${_v7step}_test PRIVATE
            dioramap_steps_normals m)
        # Quiet upstream-only warnings that fire when the impl source is
        # compiled inside the test TU (the impl is already -Werror clean
        # in its own TU; the test TU pulls in extra implicit declarations
        # from system headers).
        target_compile_options(v7_${_v7step}_test PRIVATE -Wno-error)
        add_test(NAME v7_${_v7step}_test COMMAND v7_${_v7step}_test)
    endif()
endforeach()
