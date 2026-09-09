cmake_minimum_required(VERSION 3.21)
project(rkvc-cli VERSION 0.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT TARGET rkvc::core)
    add_subdirectory(../core core-build)
endif()

if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    add_link_options(-static-libstdc++ -static-libgcc)
endif()

add_executable(rkvc-cli src/main.cpp src/args.cpp)
set_target_properties(rkvc-cli PROPERTIES
    OUTPUT_NAME rkvc
    ENABLE_EXPORTS ON)
target_include_directories(rkvc-cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(rkvc-cli PRIVATE rkvc::core)
target_compile_options(rkvc-cli PRIVATE -fno-exceptions -fno-rtti
    -Wall -Wextra)

option(RKVC_CLI_BUILD_TESTS "Build CLI tests" ON)
if(RKVC_CLI_BUILD_TESTS)
    enable_testing()
    if(NOT TARGET rkvc::doctest)
        add_library(rkvc-cli-doctest INTERFACE)
        target_include_directories(rkvc-cli-doctest INTERFACE
            ${CMAKE_CURRENT_SOURCE_DIR}/../core/third_party)
        add_library(rkvc::doctest ALIAS rkvc-cli-doctest)
    endif()
    add_executable(test_cli tests/test_cli.cpp src/args.cpp)
    target_include_directories(test_cli PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_cli PRIVATE rkvc::doctest)
    target_compile_definitions(test_cli PRIVATE
        DOCTEST_CONFIG_NO_EXCEPTIONS)
    target_compile_options(test_cli PRIVATE -fno-exceptions -fno-rtti)
    add_test(NAME test_cli COMMAND test_cli)
endif()
