# Fuzz targets — built only when DIORAMAP_FUZZ=ON (clang required).
#
# Each target is a libFuzzer executable.  Run manually:
#   ./fuzz_<name> corpus/fuzz_<name>/  -max_total_time=3600
# Crashes saved as crash-<sha>; reproduce with:
#   ./fuzz_<name> crash-<sha>
#
# See docs/quality/README.md for the full workflow.

if(NOT DIORAMAP_FUZZ)
    return()
endif()

# libFuzzer uses its own main(); each target needs the fuzzer
# linkage flag specifically (the global -fsanitize=fuzzer-no-link
# from the top-level only enables instrumentation, not the runtime).

function(add_fuzz_target name)
    add_executable(${name} ${name}.c)
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
    target_link_options(${name}    PRIVATE -fsanitize=fuzzer)
    target_link_libraries(${name} PRIVATE dioramap_hash)
    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        "${CMAKE_BINARY_DIR}/tests/fuzz")
endfunction()

add_fuzz_target(fuzz_hash_hex)
add_fuzz_target(fuzz_run_journal_parse)
