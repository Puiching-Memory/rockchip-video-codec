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

if(RKVC_MLVC_BUILD_TESTS)
    enable_testing()
    foreach(t test_mlvc_tables test_ratectl test_rans)
        add_executable(${t} tests/${t}.cpp)
        target_link_libraries(${t} PRIVATE mlvc::algo rkvc::doctest)
        target_compile_definitions(${t} PRIVATE
            DOCTEST_CONFIG_NO_EXCEPTIONS)
        target_compile_options(${t} PRIVATE
            -fno-exceptions -fno-rtti)
        add_test(NAME ${t} COMMAND ${t})
    endforeach()
endif()
