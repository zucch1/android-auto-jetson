cmake_minimum_required(VERSION 3.20)
project(aa_decode_probe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
    gstreamer-1.0
    gstreamer-app-1.0
    gstreamer-video-1.0
)

add_executable(aa-decode-probe
    src/main.cpp
    src/options.cpp
    src/discovery.cpp
    src/metrics.cpp
    src/pipeline.cpp
    src/json_report.cpp
)

target_include_directories(aa-decode-probe PRIVATE include)
target_link_libraries(aa-decode-probe PRIVATE PkgConfig::GST pthread)

if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
    target_compile_options(aa-decode-probe PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
endif()
