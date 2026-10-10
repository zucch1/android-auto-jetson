# SPDX-License-Identifier: GPL-3.0-or-later
# Cross-build gate (task 5). Configure-time: validate every manifest input hash before compiling,
# bind the immutable cache to the manifest digest, and (observed mode only) enforce that the
# hash-validated payload IS the link sysroot CMAKE_SYSROOT. Build-time: define aa_sysroot_hash_gate
# so validation re-runs before compiled targets, making post-configure tamper unable to compile.
# Included explicitly before enable_language by root CMakeLists and tests/build/cross smoke
# project. No-op when no sysroot inputs are configured (host presets). Fixture manifests are accepted
# locally and always reported 'fixture-not-target'; real acceptance sets AA_SYSROOT_REQUIRE_OBSERVED=ON.

get_property(_aa_gate_ran GLOBAL PROPERTY AA_SYSROOT_GATE_RAN SET)
if(_aa_gate_ran)
    return()
endif()
set_property(GLOBAL PROPERTY AA_SYSROOT_GATE_RAN TRUE)

find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)

set(AA_SYSROOT_MANIFEST "" CACHE FILEPATH "External aa-sysroot-1 manifest.json to validate")
set(AA_SYSROOT_PAYLOAD "" CACHE PATH "rootfs payload root paired with AA_SYSROOT_MANIFEST")
set(AA_SYSROOT_ARTIFACT "" CACHE PATH "frozen stage.materialize artifact directory (alternative input)")
set(AA_SYSROOT_CACHE "" CACHE PATH "immutable private build cache root (bound to manifest digest)")
set(AA_SYSROOT_REQUIRE_OBSERVED "OFF" CACHE BOOL "reject fixture provenance for real target acceptance")
set(AA_SYSROOT_PROVENANCE_ARGS)
if(AA_SYSROOT_MANIFEST AND AA_SYSROOT_PAYLOAD)
    set(AA_SYSROOT_PROVENANCE_ARGS --manifest "${AA_SYSROOT_MANIFEST}" --payload "${AA_SYSROOT_PAYLOAD}")
endif()

# Host no-op: no sysroot inputs configured (host-dev/release/asan presets).
if(NOT AA_SYSROOT_MANIFEST AND NOT AA_SYSROOT_ARTIFACT)
    return()
endif()

set(_aa_gate_tools "${CMAKE_CURRENT_LIST_DIR}/../tools/build")
set(_aa_require)
if(AA_SYSROOT_REQUIRE_OBSERVED)
    set(_aa_require --require-observed)
endif()

# Observed-mode binding: the hash-validated payload MUST be the actual link sysroot. This makes the
# fixture-only payload/link split impossible in observed mode; the validated root is what compiles.
if(AA_SYSROOT_REQUIRE_OBSERVED)
    if(NOT AA_SYSROOT_PAYLOAD OR NOT CMAKE_SYSROOT)
        message(FATAL_ERROR "AA_SYSROOT_OBSERVED_BINDING: observed mode requires AA_SYSROOT_PAYLOAD and CMAKE_SYSROOT (toolchain --sysroot)")
    endif()
    get_filename_component(_aa_payload_abs "${AA_SYSROOT_PAYLOAD}" ABSOLUTE)
    get_filename_component(_aa_sysroot_abs "${CMAKE_SYSROOT}" ABSOLUTE)
    if(NOT _aa_payload_abs STREQUAL _aa_sysroot_abs)
        message(FATAL_ERROR "AA_SYSROOT_OBSERVED_BINDING: hash-validated payload (${_aa_payload_abs}) MUST equal CMAKE_SYSROOT (${_aa_sysroot_abs}); the fixture-only payload/link split is forbidden in observed mode")
    endif()
endif()

set(_aa_validate_args)
if(AA_SYSROOT_ARTIFACT)
    list(APPEND _aa_validate_args --artifact "${AA_SYSROOT_ARTIFACT}")
else()
    if(NOT AA_SYSROOT_MANIFEST OR NOT AA_SYSROOT_PAYLOAD)
        message(FATAL_ERROR "AA_SYSROOT_GATE_INPUT: set AA_SYSROOT_MANIFEST+AA_SYSROOT_PAYLOAD or AA_SYSROOT_ARTIFACT")
    endif()
    list(APPEND _aa_validate_args --manifest "${AA_SYSROOT_MANIFEST}" --payload "${AA_SYSROOT_PAYLOAD}")
endif()

# 1) Configure-time hash validation before compiling: every recorded content/link hash is re-verified.
execute_process(
    COMMAND "${Python3_EXECUTABLE}" -B "${_aa_gate_tools}/validate_sysroot.py"
            ${_aa_validate_args} ${_aa_require}
    RESULT_VARIABLE _aa_validate_result OUTPUT_VARIABLE _aa_validate_out ERROR_VARIABLE _aa_validate_err)
if(NOT _aa_validate_result EQUAL 0)
    message(FATAL_ERROR "AA_SYSROOT_HASH_VALIDATION_FAILED: ${_aa_validate_out}${_aa_validate_err}")
endif()
string(JSON _aa_manifest_digest GET "${_aa_validate_out}" manifest_sha256)
set(AA_SYSROOT_MANIFEST_DIGEST "${_aa_manifest_digest}" CACHE STRING "sha256 of the validated manifest" FORCE)
message(STATUS "AA_SYSROOT validated: ${_aa_validate_out}")

# 2) Immutable cache binding: namespace cache bytes by manifest digest, refuse same-version drift.
if(AA_SYSROOT_CACHE)
    if(NOT AA_SYSROOT_MANIFEST)
        message(FATAL_ERROR "AA_SYSROOT_CACHE_BINDING_INPUT: set AA_SYSROOT_MANIFEST for cache binding")
    endif()
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -B "${_aa_gate_tools}/cache_binding.py"
                --cache-root "${AA_SYSROOT_CACHE}" --manifest "${AA_SYSROOT_MANIFEST}" ${_aa_require}
        RESULT_VARIABLE _aa_cache_result OUTPUT_VARIABLE _aa_cache_out ERROR_VARIABLE _aa_cache_err)
    if(NOT _aa_cache_result EQUAL 0)
        message(FATAL_ERROR "AA_SYSROOT_CACHE_BINDING_FAILED: ${_aa_cache_out}${_aa_cache_err}")
    endif()
    message(STATUS "AA_SYSROOT cache bound: ${_aa_cache_out}")
endif()

# 3) Build-time hash gate: custom target that re-validates before dependent targets compile. A
#    post-configure payload tamper makes this command fail, so no compile happens against it.
add_custom_target(aa_sysroot_hash_gate
    COMMAND "${Python3_EXECUTABLE}" -B "${_aa_gate_tools}/validate_sysroot.py"
            ${_aa_validate_args} ${_aa_require}
    COMMENT "AA_SYSROOT build-time hash validation before compile"
    VERBATIM)

unset(_aa_gate_tools)
unset(_aa_require)
unset(_aa_validate_args)
unset(_aa_validate_result)
unset(_aa_validate_out)
unset(_aa_validate_err)
unset(_aa_manifest_digest)
