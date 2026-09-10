cmake_minimum_required(VERSION 3.21)
project(rkvc-av1 VERSION 0.5.0 LANGUAGES CXX)

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

option(RKVC_AV1_WITH_SVT "Build the SVT-AV1 software encoder" ON)
set(SVT_AV1_INSTALL_PREFIX "" CACHE PATH "SVT-AV1 install prefix")
option(RKVC_AV1_BUILD_TESTS "Build av1 tests" ON)

if(RKVC_AV1_WITH_SVT)
    if(SVT_AV1_INSTALL_PREFIX)
        find_path(SVT_INCLUDE_DIR NAMES EbSvtAv1Enc.h
            PATHS ${SVT_AV1_INSTALL_PREFIX}/include NO_DEFAULT_PATH)
        find_library(SVT_ENC_LIB NAMES SvtAv1Enc
            PATHS ${SVT_AV1_INSTALL_PREFIX}/lib
                  ${SVT_AV1_INSTALL_PREFIX}/lib64 NO_DEFAULT_PATH)
    else()
        find_path(SVT_INCLUDE_DIR NAMES EbSvtAv1Enc.h
            PATH_SUFFIXES svt-av1)
        find_library(SVT_ENC_LIB NAMES SvtAv1Enc)
    endif()
    if(NOT SVT_INCLUDE_DIR OR NOT SVT_ENC_LIB)
        message(STATUS "rkvc-av1: SVT-AV1 not found, encoder skipped")
        set(RKVC_AV1_WITH_SVT OFF)
    endif()
endif()

if(RKVC_AV1_WITH_SVT)
    add_library(av1-codec STATIC src/encoder.cpp)
    add_library(av1::codec ALIAS av1-codec)
    target_include_directories(av1-codec
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${SVT_INCLUDE_DIR})
    target_link_libraries(av1-codec PUBLIC rkvc::core ${SVT_ENC_LIB})
    target_compile_options(av1-codec PRIVATE -fno-exceptions -fno-rtti
        -Wall -Wextra -Werror=return-type)

    add_library(rkvc_av1 MODULE src/plugin.cpp)
    target_link_libraries(rkvc_av1 PRIVATE av1::codec)
    target_compile_options(rkvc_av1 PRIVATE -fno-exceptions -fno-rtti
        -Wall -Wextra)
    set_target_properties(rkvc_av1 PROPERTIES PREFIX "")

    if(RKVC_AV1_BUILD_TESTS)
        enable_testing()
        add_executable(test_av1 tests/test_av1.cpp)
        target_link_libraries(test_av1 PRIVATE av1::codec rkvc::doctest)
        target_compile_definitions(test_av1 PRIVATE
            DOCTEST_CONFIG_NO_EXCEPTIONS
            RKVC_TEST_AV1_PLUGIN="$<TARGET_FILE:rkvc_av1>")
        target_compile_options(test_av1 PRIVATE -fno-exceptions -fno-rtti)
        set_target_properties(test_av1 PROPERTIES ENABLE_EXPORTS ON)
        add_dependencies(test_av1 rkvc_av1)
        add_test(NAME test_av1 COMMAND test_av1)
    endif()
endif()
