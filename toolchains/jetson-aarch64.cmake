# CMake toolchain: cross-compile project-owned code for NVIDIA Jetson (L4T R39.2.1) aarch64.
# Target system headers/runtime are explicit sysroot inputs, not host-installed ARM64 packages.
# Compiler executables, intrinsic headers and selected GCC support remain separate toolchain inputs.
# The paired configure-time gate (cmake/SysrootGate.cmake) validates every manifest input
# hash and binds the immutable cache to the manifest digest before any compile.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(AA_CROSS_TRIPLE "aarch64-linux-gnu" CACHE STRING "GNU target triple for the Jetson cross toolchain")

# Compilers: prefer the distro aarch64-linux-gnu cross toolchain; allow explicit override.
if(NOT CMAKE_C_COMPILER)
    find_program(AA_CROSS_GCC NAMES "${AA_CROSS_TRIPLE}-gcc" REQUIRED)
    set(CMAKE_C_COMPILER "${AA_CROSS_TRIPLE}-gcc" CACHE FILEPATH "Jetson aarch64 C compiler")
endif()
if(NOT CMAKE_CXX_COMPILER)
    find_program(AA_CROSS_GXX NAMES "${AA_CROSS_TRIPLE}-g++" REQUIRED)
    set(CMAKE_CXX_COMPILER "${AA_CROSS_TRIPLE}-g++" CACHE FILEPATH "Jetson aarch64 C++ compiler")
endif()

# --sysroot alone does NOT isolate a distro cross GCC's headers or runtime.
set(AA_SYSROOT_ROOTFS "" CACHE PATH "Validated Jetson rootfs payload used as --sysroot")
if(AA_SYSROOT_ROOTFS)
    set(CMAKE_SYSROOT "${AA_SYSROOT_ROOTFS}")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/JetsonRuntime.cmake")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES AA_SYSROOT_ROOTFS AA_CROSS_TRIPLE)

# Root-find rules: never search the host for programs (use host python3/readelf), and search
# only the sysroot for libraries, includes, and packages. This is the CMake half of the
# host-contamination guard; tools/build/contamination.py is the post-link half.
set(CMAKE_FIND_ROOT_PATH "${AA_SYSROOT_ROOTFS}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# C++20 for project-owned code; no compiler-specific extensions.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
