cmake_minimum_required(VERSION 3.21)
project(rkvc-pspack VERSION 0.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/RkvcCodec.cmake")
rkvc_resolve_core()

add_library(pspack STATIC src/ps.cpp)
add_library(pspack::pspack ALIAS pspack)
target_include_directories(pspack
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_link_libraries(pspack PUBLIC rkvc::core)
target_compile_options(pspack PRIVATE -fno-exceptions -fno-rtti
    -Wall -Wextra -Werror=return-type)

# SDK 面。本工程没有插件入口（核心直接链接），只出库与头文件。
rkvc_codec_sdk(
    INCLUDE_DIR include
    SOURCES src/ps.cpp)

option(RKVC_PSPACK_BUILD_TESTS "Build pspack logic tests" ON)

# 单测的 doctest 来自 core 源码树；对着已安装的 SDK 配置本工程时没有它。
if(RKVC_PSPACK_BUILD_TESTS AND NOT TARGET rkvc::doctest)
    message(STATUS "rkvc-pspack: doctest 不可用，跳过单测")
    set(RKVC_PSPACK_BUILD_TESTS OFF)
endif()

if(RKVC_PSPACK_BUILD_TESTS)
    enable_testing()
    add_executable(test_ps tests/test_ps.cpp)
    target_link_libraries(test_ps PRIVATE pspack::pspack rkvc::doctest)
    target_compile_definitions(test_ps PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS)
    target_compile_options(test_ps PRIVATE -fno-exceptions -fno-rtti)
    add_test(NAME test_ps COMMAND test_ps)
endif()
