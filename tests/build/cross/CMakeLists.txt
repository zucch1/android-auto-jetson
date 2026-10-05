# Self-contained cross-build smoke for the jetson-aarch64 toolchain/preset (task 5).
# It is the runnable cross-build surface used to prove the toolchain + SysrootGate +
# host-contamination machinery against a local fixture sysroot. It is explicitly NOT target
# acceptance: real acceptance runs the same commands against the observed sysroot once landed.
cmake_minimum_required(VERSION 3.20)
project(aa_cross_smoke LANGUAGES NONE)

# Configure-time gate: validates every manifest input hash and binds the immutable cache to the
# manifest digest before this target compiles. Idempotent (guarded) so preset or direct include
# both work.
include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/SysrootGate.cmake)
enable_language(CXX)

find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
set(AA_BUILD_TOOLS "${CMAKE_CURRENT_LIST_DIR}/../../../tools/build")
set(AA_BUILD_TESTS "${CMAKE_CURRENT_LIST_DIR}/../../../tests/build")

add_executable(aa_cross_smoke smoke.cpp)
target_link_options(aa_cross_smoke PRIVATE "-Wl,-Map,$<TARGET_FILE:aa_cross_smoke>.map")
set_target_properties(aa_cross_smoke PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
target_compile_options(aa_cross_smoke PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
if(TARGET aa_sysroot_hash_gate)
    # Build-time hash gate: post-configure payload tamper blocks this target before it compiles.
    add_dependencies(aa_cross_smoke aa_sysroot_hash_gate)
endif()

enable_testing()

# Post-link host-contamination QA on the freshly cross-built binary (must be clean aarch64).
add_test(NAME cross_contamination
    COMMAND "${Python3_EXECUTABLE}" -B "${AA_BUILD_TOOLS}/contamination.py"
            --binary "$<TARGET_FILE:aa_cross_smoke>" --sysroot "${CMAKE_SYSROOT}"
            --build "${CMAKE_BINARY_DIR}" --source "${CMAKE_CURRENT_LIST_DIR}"
            --compiler "${CMAKE_CXX_COMPILER}" ${AA_SYSROOT_PROVENANCE_ARGS}
            --output "$<TARGET_FILE:aa_cross_smoke>.inputs.json")
set_tests_properties(cross_contamination PROPERTIES TIMEOUT 60)

# Fixture acceptance checks (each asserts it is NOT target acceptance). These exercise hash
# validation, package-version drift, same-version content drift, immutable cache binding, and the
# content-addressed cache publisher (archive digest, idempotent publish, tamper, same-version drift).
foreach(case validate-happy validate-tamper drift-none drift-package-version
             drift-same-version-content cache-bind cache-reuse cache-drift-same-version
             cache-publish cache-publish-idempotent cache-publish-deterministic
             cache-publish-tamper cache-publish-drift
             contamination-clean contamination-host)
    add_test(NAME build_${case}
        COMMAND "${Python3_EXECUTABLE}" -B "${AA_BUILD_TESTS}/scenarios.py"
                "${CMAKE_CURRENT_LIST_DIR}/../../.." "${case}")
    set_tests_properties(build_${case} PROPERTIES TIMEOUT 120)
endforeach()
