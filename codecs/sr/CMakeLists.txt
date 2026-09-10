cmake_minimum_required(VERSION 3.21)
project(rkvc-sr VERSION 0.5.0 LANGUAGES CXX)

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

option(RKVC_SR_WITH_RKNN "Use real librknnrt backend when found" ON)
option(RKVC_SR_BUILD_TESTS "Build sr post-kernel tests" ON)
set(RKNN_INSTALL_PREFIX "" CACHE PATH "librknnrt install prefix")

if(RKVC_SR_WITH_RKNN)
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
        message(STATUS "rkvc-sr: librknnrt not found, NPU calls fail clean")
        set(RKVC_SR_WITH_RKNN OFF)
    endif()
endif()

if(RKVC_SR_WITH_RKNN)
    set(SR_RKNN_SRC src/rknn_live.cpp)
else()
    set(SR_RKNN_SRC src/rknn_stub.cpp)
endif()

add_library(sr-codec STATIC
    src/post.cpp
    src/upscale.cpp
    ${SR_RKNN_SRC}
)
add_library(sr::codec ALIAS sr-codec)
target_include_directories(sr-codec
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_link_libraries(sr-codec PUBLIC rkvc::core)
if(RKVC_SR_WITH_RKNN)
    target_include_directories(sr-codec PRIVATE ${RKNN_INCLUDE_DIR})
    target_link_libraries(sr-codec PUBLIC ${RKNN_LIB})
endif()
target_compile_options(sr-codec PRIVATE -fno-exceptions -fno-rtti
    -Wall -Wextra -Werror=return-type)

add_library(rkvc_sr MODULE src/plugin.cpp)
target_link_libraries(rkvc_sr PRIVATE sr::codec)
target_compile_options(rkvc_sr PRIVATE -fno-exceptions -fno-rtti
    -Wall -Wextra)
set_target_properties(rkvc_sr PROPERTIES PREFIX "")

if(RKVC_SR_BUILD_TESTS)
    enable_testing()
    add_executable(test_sr_post tests/test_sr_post.cpp)
    target_link_libraries(test_sr_post PRIVATE sr::codec rkvc::doctest)
    target_compile_definitions(test_sr_post PRIVATE
        DOCTEST_CONFIG_NO_EXCEPTIONS
        RKVC_TEST_SR_PLUGIN="$<TARGET_FILE:rkvc_sr>")
    target_compile_options(test_sr_post PRIVATE -fno-exceptions -fno-rtti)
    set_target_properties(test_sr_post PROPERTIES ENABLE_EXPORTS ON)
    add_dependencies(test_sr_post rkvc_sr)
    add_test(NAME test_sr_post COMMAND test_sr_post)
endif()
