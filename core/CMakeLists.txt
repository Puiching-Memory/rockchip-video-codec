cmake_minimum_required(VERSION 3.21)
project(rkvc-core VERSION 0.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(rkvc-core-static STATIC
    src/status.cpp
    src/diag.cpp
    src/spec.cpp
    src/frame.cpp
)
add_library(rkvc::core ALIAS rkvc-core-static)

target_include_directories(rkvc-core-static
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

foreach(t rkvc-core-static)
    target_compile_options(${t} PRIVATE -fno-exceptions -fno-rtti
        -Wall -Wextra -Werror=return-type)
    target_compile_definitions(${t} PUBLIC RKVC_NO_EXCEPTIONS=1)
endforeach()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    target_link_options(rkvc-core-static PRIVATE -static-libstdc++ -static-libgcc)
endif()

if(RKVC_CORE_BUILD_TESTS)
    enable_testing()
    add_executable(test_status_result tests/test_status_result.cpp)
    target_link_libraries(test_status_result PRIVATE rkvc::core)
    # doctest 单头 vendored（v2.4.11），无异常模式与 -fno-exceptions 配套。
    target_include_directories(test_status_result PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
    target_compile_definitions(test_status_result PRIVATE
        DOCTEST_CONFIG_NO_EXCEPTIONS)
    target_compile_options(test_status_result PRIVATE -fno-exceptions -fno-rtti)
    add_test(NAME test_status_result COMMAND test_status_result)

    add_executable(test_spec_frame tests/test_spec_frame.cpp)
    target_link_libraries(test_spec_frame PRIVATE rkvc::core)
    target_include_directories(test_spec_frame PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
    target_compile_definitions(test_spec_frame PRIVATE
        DOCTEST_CONFIG_NO_EXCEPTIONS)
    target_compile_options(test_spec_frame PRIVATE -fno-exceptions -fno-rtti)
    add_test(NAME test_spec_frame COMMAND test_spec_frame)
endif()
