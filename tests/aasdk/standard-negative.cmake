# Given: a real temporary target exercising the consumer's C++20 standard seam.
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef fixture_id)
set(fixture "${BUILD}/aasdk-standard-${fixture_id}")
file(MAKE_DIRECTORY "${fixture}")
configure_file("${ROOT}/tests/aasdk/consumer.cpp" "${fixture}/main.cpp" COPYONLY)
get_filename_component(library_dir "${SDK}" DIRECTORY)
file(WRITE "${fixture}/CMakeLists.txt" "cmake_minimum_required(VERSION 3.20)\nproject(standard_fixture LANGUAGES CXX)\nfind_package(Protobuf REQUIRED)\nadd_executable(probe main.cpp)\ntarget_include_directories(probe PRIVATE \"${INCLUDE}\" \"${GENERATED}\" \"${ROOT}/include\")\ntarget_link_libraries(probe PRIVATE \"${SDK}\" \"${PROTO}\" protobuf::libprotobuf)\nset_target_properties(probe PROPERTIES CXX_STANDARD \${STANDARD} CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)\n")
foreach(standard 20 17)
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${fixture}" -B "${fixture}/build-${standard}"
        "-DCMAKE_CXX_COMPILER=${CXX}" "-DCMAKE_BUILD_RPATH=${library_dir}" "-DSTANDARD=${standard}"
        RESULT_VARIABLE configured OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT configured EQUAL 0)
        message(FATAL_ERROR "Fixture configure failed, not a standard rejection: ${output}${error}")
    endif()
    # When: compile the same target with the good and actually incompatible settings.
    execute_process(COMMAND "${CMAKE_COMMAND}" --build "${fixture}/build-${standard}"
        RESULT_VARIABLE built OUTPUT_VARIABLE output ERROR_VARIABLE error)
    file(WRITE "${fixture}/result-${standard}.txt" "exit=${built}\n${output}${error}")
    # Then: C++20 succeeds; C++17 must fail for the C++20 API, not missing tooling.
    if(standard EQUAL 20 AND NOT built EQUAL 0)
        message(FATAL_ERROR "C++20 fixture failed: ${output}${error}")
    elseif(standard EQUAL 20)
        get_filename_component(sdk_dir "${SDK}" DIRECTORY)
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env "LD_LIBRARY_PATH=${sdk_dir}:$ENV{LD_LIBRARY_PATH}" "${fixture}/build-20/probe"
            RESULT_VARIABLE ran
        )
        if(NOT ran EQUAL 0)
            message(FATAL_ERROR "C++20 fixture consumer failed at runtime")
        endif()
    elseif(standard EQUAL 17)
        if(built EQUAL 0 OR NOT "${output}${error}" MATCHES "span|same_as")
            message(FATAL_ERROR "C++17 fixture did not reject the actual standard boundary: ${output}${error}")
        endif()
    endif()
endforeach()
message(STATUS "Actual C++17 rejection and C++20 success recorded in ${fixture}")
