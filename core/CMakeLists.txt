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
    target_compile_options(test_status_result PRIVATE -fno-exceptions -fno-rtti)
    add_test(NAME test_status_result COMMAND test_status_result)
endif()
