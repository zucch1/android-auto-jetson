# SPDX-License-Identifier: GPL-3.0-or-later
# Task 17 Android Auto session lifecycle: a session-thread-owned policy
# controller (include/aa/session) over the ten-state StateMachine with an
# explicit transition policy, fail-closed trust admission, injected monotonic
# deadlines, and a CancellationSource shared with an attached Transport. The raw
# StateMachine table stays in aa_core (task 14); aa_session layers the bounded
# lifecycle on top without changing it. Tests register as session_state_* for
# `ctest -R session_state`.
#
# Integration contract: include AFTER cmake/CoreArchitecture.cmake (aa_core) and
# the googletest declaration (GTest::gtest_main). The parent adds
# `include(cmake/Session.cmake)` in its own root edit.

if(CMAKE_CROSSCOMPILING)
    message(STATUS "Session lifecycle qualification unavailable in cross-smoke mode")
    return()
endif()
if(NOT TARGET aa_core)
    message(FATAL_ERROR
        "AA_SESSION_REQUIRES_AA_CORE: include cmake/CoreArchitecture.cmake before cmake/Session.cmake")
endif()

set(AA_SESSION_SOURCES
    src/session/Lifecycle.cpp
    src/session/LifecycleDeadlines.cpp
    src/session/LifecyclePolicy.cpp)
set(AA_SESSION_TEST_SOURCES
    tests/session/state_pairs_test.cpp
    tests/session/lifecycle_test.cpp
    tests/session/lifecycle_property_test.cpp
    tests/session/deadline_enforcement_test.cpp
    tests/session/pairing_identity_test.cpp
    tests/session/transport_lifecycle_test.cpp)
foreach(source IN LISTS AA_SESSION_SOURCES AA_SESSION_TEST_SOURCES)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${source}")
        message(FATAL_ERROR "AA_SESSION_SOURCE_MISSING: ${source} (session source inventory)")
    endif()
endforeach()

add_library(aa_session STATIC ${AA_SESSION_SOURCES})
set_target_properties(aa_session PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(aa_session PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(aa_session PUBLIC aa_core)
if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(aa_session PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
endif()

add_executable(aa_session_state_test ${AA_SESSION_TEST_SOURCES})
target_link_libraries(aa_session_state_test PRIVATE aa_session aa_transport GTest::gtest_main)
set_target_properties(aa_session_state_test PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(aa_session_state_test PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
endif()

if(AA_ENABLE_ASAN)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        message(FATAL_ERROR "AA_SESSION_ASAN_UNSUPPORTED: requires GNU or Clang")
    endif()
    foreach(_aa_session_asan_target aa_session aa_session_state_test)
        target_compile_options(${_aa_session_asan_target} PRIVATE
            -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(${_aa_session_asan_target} PRIVATE -fsanitize=address)
    endforeach()
    unset(_aa_session_asan_target)
endif()

add_test(NAME session_state_machine COMMAND aa_session_state_test --gtest_filter=StateMachineTable.*)
add_test(NAME session_state_lifecycle COMMAND aa_session_state_test --gtest_filter=Lifecycle.*)
add_test(NAME session_state_property COMMAND aa_session_state_test --gtest_filter=PolicyProperty.*)
add_test(NAME session_state_deadline COMMAND aa_session_state_test --gtest_filter=DeadlineEnforcement.*)
add_test(NAME session_state_pairing COMMAND aa_session_state_test --gtest_filter=PairingIdentity.*)
add_test(NAME session_state_transport COMMAND aa_session_state_test --gtest_filter=TransportLifecycle.*)
set_tests_properties(session_state_machine session_state_lifecycle session_state_property
    session_state_deadline session_state_pairing session_state_transport
    PROPERTIES TIMEOUT 60)

unset(AA_SESSION_SOURCES)
unset(AA_SESSION_TEST_SOURCES)
