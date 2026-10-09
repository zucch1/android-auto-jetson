# SPDX-License-Identifier: GPL-3.0-or-later
# Task 16 transport: a common bounded AAP framed API (include/aa/transport) with
# real TCP and TLS transports and a USB byte-endpoint seam. The frame codec and
# message fragmentation live in aa_transport (pure project code); the TLS record
# and stream adapters live in aa_transport_tls under src/**/*_adapter/ and are the
# only place OpenSSL appears. Tests register as transport_* for `ctest -R transport`.

if(CMAKE_CROSSCOMPILING)
    message(STATUS "Transport qualification unavailable in cross-smoke mode")
    return()
endif()

set(AA_TRANSPORT_SOURCES
    src/transport/FrameCodec.cpp
    src/transport/MessageFramer.cpp
    src/transport/ByteEndpoint.cpp
    src/transport/TcpTransport.cpp
    src/transport/UsbByteTransport.cpp)
set(AA_TRANSPORT_TLS_SOURCES
    src/transport/tls_adapter/TlsRecordCodec.cpp
    src/transport/tls_adapter/TlsTransport.cpp
    src/transport/tls_adapter/TlsStream.cpp
    src/transport/tls_adapter/TlsListener.cpp)
set(AA_TRANSPORT_TEST_SOURCES
    tests/transport/framing_test.cpp
    tests/transport/malformed_test.cpp
    tests/transport/io_test.cpp
    tests/transport/tls_test.cpp)
foreach(source IN LISTS AA_TRANSPORT_SOURCES AA_TRANSPORT_TLS_SOURCES AA_TRANSPORT_TEST_SOURCES)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${source}")
        message(FATAL_ERROR "AA_TRANSPORT_SOURCE_MISSING: ${source}")
    endif()
endforeach()

add_library(aa_transport STATIC ${AA_TRANSPORT_SOURCES})
set_target_properties(aa_transport PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(aa_transport PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(aa_transport PUBLIC aa_core)
if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(aa_transport PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
endif()

add_library(aa_transport_tls STATIC ${AA_TRANSPORT_TLS_SOURCES})
set_target_properties(aa_transport_tls PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(aa_transport_tls PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(aa_transport_tls PUBLIC aa_transport aa_tls)
if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(aa_transport_tls PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
endif()

add_executable(aa_transport_frame_test
    tests/transport/framing_test.cpp tests/transport/malformed_test.cpp)
target_link_libraries(aa_transport_frame_test PRIVATE aa_transport GTest::gtest_main)
set_target_properties(aa_transport_frame_test PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
target_compile_options(aa_transport_frame_test PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)

add_executable(aa_transport_io_test
    tests/transport/io_test.cpp tests/transport/cancellation_test.cpp
    tests/transport/lifetime_test.cpp tests/transport/pending_test.cpp)
target_link_libraries(aa_transport_io_test PRIVATE aa_transport GTest::gtest_main)
set_target_properties(aa_transport_io_test PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
target_compile_options(aa_transport_io_test PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)

add_executable(aa_transport_tls_test
    tests/transport/tls_test.cpp tests/transport/tls_lifetime_test.cpp
    tests/transport/tls_pending_test.cpp)
target_link_libraries(aa_transport_tls_test PRIVATE aa_transport_tls GTest::gtest_main)
set_target_properties(aa_transport_tls_test PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
target_compile_options(aa_transport_tls_test PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)

if(AA_ENABLE_ASAN)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        message(FATAL_ERROR "AA_TRANSPORT_ASAN_UNSUPPORTED: requires GNU or Clang")
    endif()
    foreach(_aa_asan_target aa_transport aa_transport_tls
            aa_transport_frame_test aa_transport_io_test aa_transport_tls_test)
        target_compile_options(${_aa_asan_target} PRIVATE
            -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(${_aa_asan_target} PRIVATE -fsanitize=address)
    endforeach()
    unset(_aa_asan_target)
endif()

add_test(NAME transport_framing COMMAND aa_transport_frame_test --gtest_filter=Framing.*)
add_test(NAME transport_malformed COMMAND aa_transport_frame_test --gtest_filter=Malformed.*)
add_test(NAME transport_fuzz_smoke COMMAND aa_transport_frame_test --gtest_filter=FuzzSmoke.*)
add_test(NAME transport_tcp COMMAND aa_transport_io_test --gtest_filter=TcpTransport.*)
add_test(NAME transport_usb COMMAND aa_transport_io_test --gtest_filter=UsbByteTransport.*)
add_test(NAME transport_cancellation COMMAND aa_transport_io_test --gtest_filter=InFlightCancellation.*)
add_test(NAME transport_tls COMMAND "${Python3_EXECUTABLE}" -B
    "${CMAKE_SOURCE_DIR}/tests/transport/tls_run.py"
    "${CMAKE_SOURCE_DIR}" "$<TARGET_FILE:aa_transport_tls_test>")
set_tests_properties(transport_framing transport_malformed transport_fuzz_smoke
    transport_tcp transport_usb transport_tls transport_cancellation PROPERTIES TIMEOUT 60)

unset(AA_TRANSPORT_SOURCES)
unset(AA_TRANSPORT_TLS_SOURCES)
unset(AA_TRANSPORT_TEST_SOURCES)
