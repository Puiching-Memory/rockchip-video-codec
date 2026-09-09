cmake_minimum_required(VERSION 3.21)
project(rkvc-h264h265 VERSION 0.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT TARGET rkvc::core)
    add_subdirectory(../../core core-build)
endif()
find_package(Threads REQUIRED)

if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    add_link_options(-static-libstdc++ -static-libgcc)
endif()

option(RKVC_H264H265_WITH_MPP "Build the MPP hard codec nodes" ON)
set(MPP_INSTALL_PREFIX "" CACHE PATH "librockchip_mpp install prefix")
option(RKVC_H264H265_BUILD_TESTS "Build h264h265 logic tests" ON)

if(RKVC_H264H265_WITH_MPP)
    if(MPP_INSTALL_PREFIX)
        set(MPP_INC_HINT ${MPP_INSTALL_PREFIX}/include)
        set(MPP_LIB_HINT ${MPP_INSTALL_PREFIX}/lib
                         ${MPP_INSTALL_PREFIX}/lib64)
    endif()
    find_path(MPP_INCLUDE_DIR NAMES rk_mpi.h
        PATHS ${MPP_INC_HINT}
              ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/mpp/inc)
    # The runtime lib is board-only; the plugin leaves MPP symbols undefined
    # when it is absent (container compile check) and links it on boards.
    find_library(MPP_LIB NAMES rockchip_mpp PATHS ${MPP_LIB_HINT})
endif()

if(RKVC_H264H265_WITH_MPP AND NOT MPP_INCLUDE_DIR)
    message(STATUS "rkvc-h264h265: MPP headers not found, nodes skipped")
    set(RKVC_H264H265_WITH_MPP OFF)
endif()

if(RKVC_H264H265_WITH_MPP)
    add_library(h264h265-codec STATIC
        src/detail.cpp
        src/decoder.cpp
        src/encoder.cpp
    )
    add_library(h264h265::codec ALIAS h264h265-codec)
    target_include_directories(h264h265-codec
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${MPP_INCLUDE_DIR})
    target_link_libraries(h264h265-codec PUBLIC rkvc::core)
    target_compile_options(h264h265-codec PRIVATE -fno-exceptions -fno-rtti
        -Wall -Wextra -Werror=return-type)

    add_library(rkvc_h264h265 MODULE src/plugin.cpp)
    target_link_libraries(rkvc_h264h265 PRIVATE h264h265::codec)
    if(MPP_LIB)
        target_link_libraries(rkvc_h264h265 PRIVATE ${MPP_LIB})
    endif()
    target_compile_options(rkvc_h264h265 PRIVATE -fno-exceptions -fno-rtti
        -Wall -Wextra)
    set_target_properties(rkvc_h264h265 PROPERTIES PREFIX "")

    if(RKVC_H264H265_BUILD_TESTS)
        enable_testing()
        add_executable(test_mpp_logic tests/test_mpp_logic.cpp)
        target_link_libraries(test_mpp_logic PRIVATE h264h265::codec
                              rkvc::doctest)
        target_compile_definitions(test_mpp_logic PRIVATE
            DOCTEST_CONFIG_NO_EXCEPTIONS)
        target_compile_options(test_mpp_logic PRIVATE
            -fno-exceptions -fno-rtti)
        add_test(NAME test_mpp_logic COMMAND test_mpp_logic)
    endif()
endif()
