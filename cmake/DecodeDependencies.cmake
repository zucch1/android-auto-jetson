# Decode probe target development metadata (task 9).
# Native builds: host GStreamer development metadata is mandatory; discovery fails closed (REQUIRED).
# Cross builds (jetson-aarch64): target GStreamer development metadata must come from the
# hash-validated sysroot payload (or, later, an explicitly configured overlay root). When the
# validated payload provides no target GStreamer module metadata, the probe is EXPLICITLY
# UNAVAILABLE: configure succeeds (AA_DECODE_PROBE_CROSS=UNAVAILABLE, aa-decode-probe: UNAVAILABLE
# status line) and the probe CMakeLists registers no probe targets or tests. Incomplete metadata
# (some but not all of the three modules) fails closed and never marks the probe available.
#
# FindPkgConfig has its own search policy; CMAKE_FIND_ROOT_PATH does not isolate it.

set(AA_DECODE_PROBE_READY FALSE)

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
    set(_aa_decode_pc_dirs)
    foreach(_aa_pc_suffix usr/lib/aarch64-linux-gnu/pkgconfig usr/lib/pkgconfig usr/share/pkgconfig)
        set(_aa_pc_dir "${_aa_decode_root}/${_aa_pc_suffix}")
        if(EXISTS "${_aa_pc_dir}")
            file(GLOB _aa_pc_files "${_aa_pc_dir}/*.pc")
            foreach(_aa_pc_path "${_aa_pc_dir}" ${_aa_pc_files})
                get_filename_component(_aa_pc_real "${_aa_pc_path}" REALPATH)
                string(FIND "${_aa_pc_real}" "${_aa_decode_root}/" _aa_pc_inside)
                if(NOT _aa_pc_inside EQUAL 0)
                    message(FATAL_ERROR "AA_DECODE_PKG_ESCAPE: ${_aa_pc_path}")
                endif()
            endforeach()
        endif()
        list(APPEND _aa_decode_pc_dirs "${_aa_pc_dir}")
    endforeach()
    # Presence gate on target metadata, never on host fallbacks: the three probe modules must all
    # resolve from the validated payload or the probe is explicitly unavailable (F3 sanctioned).
    set(_aa_decode_found 0)
    foreach(_aa_decode_module gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)
        set(_aa_decode_module_found FALSE)
        foreach(_aa_pc_dir IN LISTS _aa_decode_pc_dirs)
            if(EXISTS "${_aa_pc_dir}/${_aa_decode_module}.pc")
                set(_aa_decode_module_found TRUE)
            endif()
        endforeach()
        if(_aa_decode_module_found)
            math(EXPR _aa_decode_found "${_aa_decode_found}+1")
        endif()
    endforeach()
    if(_aa_decode_found EQUAL 0)
        message(STATUS "aa-decode-probe: UNAVAILABLE (target GStreamer dev metadata absent from frozen sysroot)")
        set(AA_DECODE_PROBE_CROSS "UNAVAILABLE" CACHE STRING "Cross decode probe target metadata state" FORCE)
        return()
    elseif(NOT _aa_decode_found EQUAL 3)
        message(FATAL_ERROR "AA_DECODE_PKG_METADATA: incomplete target GStreamer dev metadata (${_aa_decode_found}/3 modules in validated sysroot payload)")
    endif()
    set(_aa_decode_provider "${_aa_decode_root}")
    list(JOIN _aa_decode_pc_dirs ":" _aa_decode_pc_libdir)
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
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
    NO_CMAKE_PATH NO_CMAKE_ENVIRONMENT_PATH
    gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)

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
    set(AA_DECODE_PROBE_CROSS "AVAILABLE" CACHE STRING "Cross decode probe target metadata state" FORCE)
endif()
set(AA_DECODE_PROBE_READY TRUE)
