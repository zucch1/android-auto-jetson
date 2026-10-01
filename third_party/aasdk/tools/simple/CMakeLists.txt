cmake_minimum_required(VERSION 3.10)
project(aasdk_simple_headunit)

set(CMAKE_CXX_STANDARD 17)

add_executable(aasdk_simple_headunit
    main.cpp
    usb_init.cpp
)

target_include_directories(aasdk_simple_headunit PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/protobuf
    ${CMAKE_SOURCE_DIR}/aasdk_proto
    /usr/include/libusb-1.0
)

target_link_libraries(aasdk_simple_headunit
    aasdk
    pthread
    ssl
    crypto
    usb-1.0
)
