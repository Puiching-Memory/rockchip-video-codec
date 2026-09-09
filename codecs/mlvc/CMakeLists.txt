cmake_minimum_required(VERSION 3.21)
project(rkvc-mlvc VERSION 0.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT TARGET rkvc::core)
    add_subdirectory(../../core core-build)
endif()
find_package(Threads REQUIRED)

add_library(mlvc-algo STATIC
    src/container.cpp
    src/pmf.cpp
    src/qptab.cpp
    src/qppatch.cpp
    src/ratectl.cpp
    src/rans.cpp
    src/pixel.cpp
)
add_library(mlvc::algo ALIAS mlvc-algo)

target_include_directories(mlvc-algo
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(mlvc-algo PUBLIC rkvc::core)
target_compile_options(mlvc-algo PRIVATE -fno-exceptions -fno-rtti
    -Wall -Wextra -Werror=return-type)
target_compile_definitions(mlvc-algo PUBLIC RKVC_NO_EXCEPTIONS=1)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    target_link_options(mlvc-algo PRIVATE -static-libstdc++ -static-libgcc)
endif()

option(RKVC_MLVC_BUILD_TESTS "Build mlvc algorithm tests" ON)
option(RKVC_MLVC_WITH_RKNN "Use real librknnrt backend when found" ON)
set(RKNN_INSTALL_PREFIX "" CACHE PATH "librknnrt install prefix")

if(RKVC_MLVC_WITH_RKNN)
    if(RKNN_INSTALL_PREFIX)
        find_path(RKNN_INCLUDE_DIR NAMES rknn_api.h
            PATHS ${RKNN_INSTALL_PREFIX}/include NO_DEFAULT_PATH)
        find_library(RKNN_LIB NAMES rknnrt
            PATHS ${RKNN_INSTALL_PREFIX}/lib NO_DEFAULT_PATH)
    else()
        find_path(RKNN_INCLUDE_DIR NAMES rknn_api.h)
        find_library(RKNN_LIB NAMES rknnrt)
    endif()
    if(NOT RKNN_INCLUDE_DIR OR NOT RKNN_LIB)
        message(STATUS "rkvc-mlvc: librknnrt not found, NPU calls fail clean")
        set(RKVC_MLVC_WITH_RKNN OFF)
    endif()
endif()

if(RKVC_MLVC_WITH_RKNN)
    set(MLVC_NPU_SRC src/npu_rknn.cpp)
else()
    set(MLVC_NPU_SRC src/npu_stub.cpp)
endif()

add_library(mlvc-codec STATIC
    src/codec.cpp
    src/encoder.cpp
    src/decoder.cpp
    src/npu_fake.cpp
    ${MLVC_NPU_SRC}
)
add_library(mlvc::codec ALIAS mlvc-codec)
target_include_directories(mlvc-codec
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_link_libraries(mlvc-codec PUBLIC rkvc::core mlvc::algo)
if(RKVC_MLVC_WITH_RKNN)
    target_include_directories(mlvc-codec PRIVATE ${RKNN_INCLUDE_DIR})
    target_link_libraries(mlvc-codec PUBLIC ${RKNN_LIB})
endif()
target_compile_options(mlvc-codec PRIVATE -fno-exceptions -fno-rtti
    -Wall -Wextra -Werror=return-type)

add_library(rkvc_mlvc MODULE src/plugin.cpp)
target_link_libraries(rkvc_mlvc PRIVATE mlvc::codec)
target_compile_options(rkvc_mlvc PRIVATE -fno-exceptions -fno-rtti
    -Wall -Wextra)
set_target_properties(rkvc_mlvc PROPERTIES PREFIX "")

if(RKVC_MLVC_BUILD_TESTS)
    enable_testing()
    foreach(t test_mlvc_tables test_ratectl test_rans test_pixel)
        add_executable(${t} tests/${t}.cpp)
        target_link_libraries(${t} PRIVATE mlvc::algo rkvc::doctest)
        target_compile_definitions(${t} PRIVATE
            DOCTEST_CONFIG_NO_EXCEPTIONS)
        target_compile_options(${t} PRIVATE
            -fno-exceptions -fno-rtti)
        add_test(NAME ${t} COMMAND ${t})
    endforeach()

    add_executable(test_mlvc_codec tests/test_mlvc_codec.cpp)
    target_link_libraries(test_mlvc_codec PRIVATE mlvc::codec rkvc::doctest)
    target_compile_definitions(test_mlvc_codec PRIVATE
        DOCTEST_CONFIG_NO_EXCEPTIONS
        RKVC_TEST_MLVC_PLUGIN="$<TARGET_FILE:rkvc_mlvc>")
    target_compile_options(test_mlvc_codec PRIVATE
        -fno-exceptions -fno-rtti)
    set_target_properties(test_mlvc_codec PROPERTIES ENABLE_EXPORTS ON)
    add_dependencies(test_mlvc_codec rkvc_mlvc)
    add_test(NAME test_mlvc_codec COMMAND test_mlvc_codec)
endif()
