# SPDX-License-Identifier: GPL-3.0-or-later
# Decode probe target development metadata (task 9).
# Native builds: host GStreamer development metadata is mandatory; discovery fails closed (REQUIRED).
# Cross builds (jetson-aarch64): target GStreamer development metadata must come from the
# hash-validated sysroot payload or the explicitly configured AA_DECODE_GST_OVERLAY capture root
# (dev files captured read-only from the target, same layout as the target filesystem). Exactly one
# of these roots is the metadata provider; pkg-config is bound to it (F3 isolation). When no root
# provides any target GStreamer module metadata, the probe is EXPLICITLY UNAVAILABLE: configure
# succeeds (AA_DECODE_PROBE_CROSS=UNAVAILABLE, aa-decode-probe: UNAVAILABLE status line) and the
# probe CMakeLists registers no probe targets or tests. Incomplete metadata (some but not all of
# the three modules, from any root combination) fails closed and never marks the probe available.
#
# FindPkgConfig has its own search policy; CMAKE_FIND_ROOT_PATH does not isolate it.

# Declare the cache knob only when nothing (preset -D or caller) has defined it; a plain
# set(... CACHE ...) here would clear a caller-set binding under the 3.20 policy defaults.
if(NOT DEFINED AA_DECODE_GST_OVERLAY)
    set(AA_DECODE_GST_OVERLAY "" CACHE PATH "Cross-only rootfs fragment with target GStreamer dev metadata (.pc/headers/libs) captured read-only from the target")
endif()
set(AA_DECODE_PROBE_READY FALSE)
if(NOT CMAKE_CROSSCOMPILING AND AA_DECODE_GST_OVERLAY)
    message(FATAL_ERROR "AA_DECODE_GST_OVERLAY: cross-only extension; native builds discover mandatory host GStreamer development metadata directly")
endif()

if(CMAKE_CROSSCOMPILING)
    if(NOT TARGET aa_sysroot_hash_gate OR NOT AA_SYSROOT_PAYLOAD OR NOT CMAKE_SYSROOT)
        message(FATAL_ERROR "AA_DECODE_SYSROOT: require a hash-gated payload bound to CMAKE_SYSROOT")
    endif()
    get_filename_component(_aa_decode_root "${CMAKE_SYSROOT}" REALPATH)
    get_filename_component(_aa_decode_payload "${AA_SYSROOT_PAYLOAD}" REALPATH)
    if(NOT _aa_decode_root STREQUAL _aa_decode_payload)
        message(FATAL_ERROR "AA_DECODE_SYSROOT: validated payload must equal link sysroot")
    endif()
    if(NOT "$ENV{PKG_CONFIG_PATH}" STREQUAL "")
        message(FATAL_ERROR "AA_DECODE_PKG_ENV: clear PKG_CONFIG_PATH; host metadata is forbidden")
    endif()
    # Snapshot the complete overlay tree, then reverify before compilation and linking.
    # The digest records content, not an observed-target origin or runtime qualification.
    set(_aa_decode_overlay "")
    set(AA_DECODE_OVERLAY_PROVENANCE_ARGS)
    if(AA_DECODE_GST_OVERLAY)
        if(NOT IS_DIRECTORY "${AA_DECODE_GST_OVERLAY}")
            message(FATAL_ERROR "AA_DECODE_PKG_ESCAPE: AA_DECODE_GST_OVERLAY is not a directory: ${AA_DECODE_GST_OVERLAY}")
        endif()
        get_filename_component(_aa_decode_overlay "${AA_DECODE_GST_OVERLAY}" REALPATH)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        set(_aa_overlay_tool "${CMAKE_CURRENT_LIST_DIR}/../tools/build/decode_overlay.py")
        set(AA_DECODE_GST_OVERLAY_MANIFEST "${CMAKE_BINARY_DIR}/aa-decode-overlay.sha256")
        execute_process(COMMAND "${Python3_EXECUTABLE}" -B "${_aa_overlay_tool}"
            --root "${_aa_decode_overlay}" --manifest "${AA_DECODE_GST_OVERLAY_MANIFEST}"
            RESULT_VARIABLE _aa_overlay_result OUTPUT_VARIABLE _aa_overlay_digest
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_VARIABLE _aa_overlay_error)
        if(NOT _aa_overlay_result EQUAL 0)
            message(FATAL_ERROR "AA_DECODE_PKG_ESCAPE: ${_aa_overlay_error}")
        endif()
        set(AA_DECODE_GST_OVERLAY_DIGEST "${_aa_overlay_digest}" CACHE STRING "sha256 of complete overlay input manifest" FORCE)
        set(AA_DECODE_OVERLAY_VERIFY_COMMAND "${Python3_EXECUTABLE}" -B "${_aa_overlay_tool}"
            --root "${_aa_decode_overlay}" --manifest "${AA_DECODE_GST_OVERLAY_MANIFEST}"
            --digest "${_aa_overlay_digest}")
        set(AA_DECODE_OVERLAY_PROVENANCE_ARGS --overlay "${_aa_decode_overlay}"
            --overlay-manifest "${AA_DECODE_GST_OVERLAY_MANIFEST}" --overlay-digest "${_aa_overlay_digest}")
        if(NOT TARGET aa_decode_overlay_hash_gate)
            add_custom_target(aa_decode_overlay_hash_gate
                COMMAND ${AA_DECODE_OVERLAY_VERIFY_COMMAND}
                COMMENT "AA_DECODE overlay hash validation before compile" VERBATIM)
        endif()
        set(AA_DECODE_OVERLAY_RECORD_SOURCE "${CMAKE_BINARY_DIR}/aa-decode-overlay-record.cpp")
        file(WRITE "${AA_DECODE_OVERLAY_RECORD_SOURCE}"
            "[[gnu::used, gnu::section(\".aa_decode_overlay\")]] static const char aa_decode_overlay_digest[] = \"${_aa_overlay_digest}\";\n")
        message(STATUS "aa-decode-probe: overlay inputs digest-recorded ${_aa_overlay_digest}")
    endif()
    # Sanctioned metadata roots: validated sysroot payload first, then the overlay extension.
    # Each root's pkg-config inputs are escape-checked against that root alone.
    set(_aa_decode_candidates "${_aa_decode_root}")
    if(_aa_decode_overlay)
        list(APPEND _aa_decode_candidates "${_aa_decode_overlay}")
    endif()
    set(_aa_decode_found_0 0)
    set(_aa_decode_found_1 0)
    set(_aa_decode_idx 0)
    foreach(_aa_decode_candidate IN LISTS _aa_decode_candidates)
        set(_aa_pc_dirs)
        foreach(_aa_pc_suffix usr/lib/aarch64-linux-gnu/pkgconfig usr/lib/pkgconfig usr/share/pkgconfig)
            set(_aa_pc_dir "${_aa_decode_candidate}/${_aa_pc_suffix}")
            if(EXISTS "${_aa_pc_dir}")
                file(GLOB _aa_pc_files "${_aa_pc_dir}/*.pc")
                foreach(_aa_pc_path "${_aa_pc_dir}" ${_aa_pc_files})
                    get_filename_component(_aa_pc_real "${_aa_pc_path}" REALPATH)
                    string(FIND "${_aa_pc_real}" "${_aa_decode_candidate}/" _aa_pc_inside)
                    if(NOT _aa_pc_inside EQUAL 0)
                        message(FATAL_ERROR "AA_DECODE_PKG_ESCAPE: ${_aa_pc_path}")
                    endif()
                endforeach()
            endif()
            list(APPEND _aa_pc_dirs "${_aa_pc_dir}")
        endforeach()
        set(_aa_decode_pc_dirs_${_aa_decode_idx} ${_aa_pc_dirs})
        foreach(_aa_decode_module gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)
            set(_aa_decode_module_found FALSE)
            foreach(_aa_pc_dir IN LISTS _aa_pc_dirs)
                if(EXISTS "${_aa_pc_dir}/${_aa_decode_module}.pc")
                    set(_aa_decode_module_found TRUE)
                endif()
            endforeach()
            if(_aa_decode_module_found)
                math(EXPR _aa_decode_found_${_aa_decode_idx} "${_aa_decode_found_${_aa_decode_idx}}+1")
            endif()
        endforeach()
        math(EXPR _aa_decode_idx "${_aa_decode_idx}+1")
    endforeach()
    # Availability gate on target metadata, never on host fallbacks (F3 sanctioned outcome:
    # explicitly mark the probe unavailable when target dev metadata is absent). Incomplete
    # metadata is a fail-closed error, not an unavailable probe.
    set(_aa_sysroot_modules ${_aa_decode_found_0})
    set(_aa_overlay_modules ${_aa_decode_found_1})
    if(_aa_sysroot_modules EQUAL 3)
        set(_aa_decode_provider "${_aa_decode_root}")
        set(_aa_decode_provider_pc_dirs ${_aa_decode_pc_dirs_0})
        if(_aa_overlay_modules GREATER 0)
            message(STATUS "aa-decode-probe: overlay metadata present but incomplete; validated sysroot payload takes precedence")
        endif()
    elseif(_aa_overlay_modules EQUAL 3)
        set(_aa_decode_provider "${_aa_decode_overlay}")
        set(_aa_decode_provider_pc_dirs ${_aa_decode_pc_dirs_1})
        message(STATUS "aa-decode-probe: cross metadata source AA_DECODE_GST_OVERLAY")
    elseif(_aa_sysroot_modules GREATER 0 OR _aa_overlay_modules GREATER 0)
        message(FATAL_ERROR "AA_DECODE_PKG_METADATA: incomplete target GStreamer dev metadata (sysroot ${_aa_sysroot_modules}/3, overlay ${_aa_overlay_modules}/3)")
    else()
        if(_aa_decode_overlay)
            message(STATUS "aa-decode-probe: UNAVAILABLE (target GStreamer dev metadata absent from frozen sysroot and overlay)")
        else()
            message(STATUS "aa-decode-probe: UNAVAILABLE (target GStreamer dev metadata absent from frozen sysroot)")
        endif()
        set(AA_DECODE_PROBE_CROSS "UNAVAILABLE" CACHE STRING "Cross decode probe target metadata state" FORCE)
        return()
    endif()
    list(JOIN _aa_decode_provider_pc_dirs ":" _aa_decode_pc_libdir)
    set(ENV{PKG_CONFIG_SYSROOT_DIR} "${_aa_decode_provider}")
    set(ENV{PKG_CONFIG_LIBDIR} "${_aa_decode_pc_libdir}")
    # Ignore ambient wrappers/arguments and CMake prefixes as additional search roots.
    find_program(AA_DECODE_HOST_PKG_CONFIG NAMES pkg-config REQUIRED)
    set(PKG_CONFIG_EXECUTABLE "${AA_DECODE_HOST_PKG_CONFIG}" CACHE FILEPATH "Host metadata reader" FORCE)
    set(PKG_CONFIG_ARGN "" CACHE STRING "Isolated metadata reader arguments" FORCE)
    set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH FALSE)
    # Do not reuse FindPkgConfig's cached host observations after a mode/input change.
    unset(__pkg_config_checked_GST CACHE)
    get_cmake_property(_aa_decode_cache CACHE_VARIABLES)
    foreach(_aa_decode_cached IN LISTS _aa_decode_cache)
        if(_aa_decode_cached MATCHES "^pkgcfg_lib_GST_")
            unset(${_aa_decode_cached} CACHE)
        endif()
    endforeach()
endif()

find_package(PkgConfig REQUIRED)
if(CMAKE_CROSSCOMPILING AND _aa_decode_overlay)
    # FindPkgConfig resolves -L inputs through find_library. Sanction only the
    # validated roots here so root-only toolchains do not re-root the overlay.
    set(_aa_decode_find_roots_defined FALSE)
    if(DEFINED CMAKE_FIND_ROOT_PATH)
        set(_aa_decode_find_roots_defined TRUE)
        set(_aa_decode_saved_find_roots "${CMAKE_FIND_ROOT_PATH}")
    endif()
    set(CMAKE_FIND_ROOT_PATH "${_aa_decode_overlay};${_aa_decode_root}")
endif()
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
    NO_CMAKE_PATH NO_CMAKE_ENVIRONMENT_PATH
    gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)
if(CMAKE_CROSSCOMPILING AND _aa_decode_overlay)
    if(_aa_decode_find_roots_defined)
        set(CMAKE_FIND_ROOT_PATH "${_aa_decode_saved_find_roots}")
    else()
        unset(CMAKE_FIND_ROOT_PATH)
    endif()
endif()

if(CMAKE_CROSSCOMPILING)
    get_target_property(_aa_decode_links PkgConfig::GST INTERFACE_LINK_LIBRARIES)
    foreach(_aa_decode_path IN LISTS GST_INCLUDE_DIRS GST_LIBRARY_DIRS _aa_decode_links)
        if(NOT IS_ABSOLUTE "${_aa_decode_path}" OR NOT EXISTS "${_aa_decode_path}")
            message(FATAL_ERROR "AA_DECODE_PKG_ESCAPE: unresolved input ${_aa_decode_path}")
        endif()
        get_filename_component(_aa_decode_real "${_aa_decode_path}" REALPATH)
        string(FIND "${_aa_decode_real}" "${_aa_decode_provider}/" _aa_decode_inside)
        if(NOT _aa_decode_inside EQUAL 0)
            message(FATAL_ERROR "AA_DECODE_PKG_ESCAPE: input outside validated metadata root ${_aa_decode_path}")
        endif()
    endforeach()
    # GStreamer needs only -pthread beyond include/library flags. Refuse opaque path flags.
    foreach(_aa_decode_flag IN LISTS GST_CFLAGS_OTHER GST_LDFLAGS_OTHER)
        if(NOT _aa_decode_flag STREQUAL "-pthread")
            message(FATAL_ERROR "AA_DECODE_PKG_ESCAPE: unsupported metadata flag ${_aa_decode_flag}")
        endif()
    endforeach()
    if(_aa_decode_provider STREQUAL _aa_decode_overlay)
        # Cross ld needs the captured DT_NEEDED closure, including /lib SONAME
        # symlinks. Containment and digest checks do not protect CMake list or -Wl
        # encoding; permit only simple path bytes before appending each option.
        foreach(_aa_decode_runtime_dir IN LISTS GST_LIBRARY_DIRS)
            if(_aa_decode_runtime_dir MATCHES "[^A-Za-z0-9_./+-]")
                message(FATAL_ERROR "AA_DECODE_PKG_ESCAPE: unsafe character in rpath-link directory ${_aa_decode_runtime_dir}")
            endif()
            set_property(TARGET PkgConfig::GST APPEND PROPERTY INTERFACE_LINK_OPTIONS
                "-Wl,-rpath-link,${_aa_decode_runtime_dir}")
        endforeach()
        foreach(_aa_decode_runtime_suffix lib/aarch64-linux-gnu lib)
            if(IS_DIRECTORY "${_aa_decode_overlay}/${_aa_decode_runtime_suffix}")
                set(_aa_decode_runtime_dir "${_aa_decode_overlay}/${_aa_decode_runtime_suffix}")
                if(_aa_decode_runtime_dir MATCHES "[^A-Za-z0-9_./+-]")
                    message(FATAL_ERROR "AA_DECODE_PKG_ESCAPE: unsafe character in rpath-link directory ${_aa_decode_runtime_dir}")
                endif()
                set_property(TARGET PkgConfig::GST APPEND PROPERTY INTERFACE_LINK_OPTIONS
                    "-Wl,-rpath-link,${_aa_decode_runtime_dir}")
            endif()
        endforeach()
    endif()
    set(AA_DECODE_PROBE_CROSS "AVAILABLE" CACHE STRING "Cross decode probe target metadata state" FORCE)
endif()
set(AA_DECODE_PROBE_READY TRUE)
