# SPDX-License-Identifier: GPL-3.0-or-later
# Harden only project-owned targets. Call aa_harden_project_targets() from each
# project directory after its targets are created; staged dependencies live in
# separate directories and deliberately do not inherit these target options.
function(aa_harden_project_targets)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
            NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
        message(FATAL_ERROR "AA_HARDENING_UNSUPPORTED: project binaries require GNU/Clang on Linux")
    endif()

    get_property(_aa_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_aa_target IN LISTS _aa_targets)
        get_target_property(_aa_type ${_aa_target} TYPE)
        if(NOT _aa_type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$")
            continue()
        endif()
        get_target_property(_aa_already_hardened ${_aa_target} AA_HARDENED)
        if(_aa_already_hardened)
            continue()
        endif()

        target_compile_options(${_aa_target} PRIVATE -fstack-protector-strong)
        if(NOT AA_ENABLE_ASAN)
            target_compile_options(${_aa_target} PRIVATE
                "$<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>:-D_FORTIFY_SOURCE=2>")
        endif()

        if(_aa_type STREQUAL "EXECUTABLE")
            target_compile_options(${_aa_target} PRIVATE -fPIE)
            target_link_options(${_aa_target} PRIVATE -pie)
            set_property(GLOBAL APPEND PROPERTY AA_HARDENED_EXECUTABLES "$<TARGET_FILE:${_aa_target}>")
        elseif(_aa_type MATCHES "^(STATIC_LIBRARY|OBJECT_LIBRARY)$")
            # PIC is also valid input to a PIE and must be preserved for archives
            # (notably aa_tls) that can be linked into a shared library.
            get_target_property(_aa_pic ${_aa_target} POSITION_INDEPENDENT_CODE)
            if(_aa_pic)
                target_compile_options(${_aa_target} PRIVATE -fPIC)
            else()
                target_compile_options(${_aa_target} PRIVATE -fPIE)
            endif()
        endif()
        if(NOT _aa_type MATCHES "^(STATIC_LIBRARY|OBJECT_LIBRARY)$")
            target_link_options(${_aa_target} PRIVATE
                -Wl,-z,relro,-z,now -Wl,-z,noexecstack)
        endif()
        set_target_properties(${_aa_target} PROPERTIES AA_HARDENED TRUE)
    endforeach()
endfunction()
