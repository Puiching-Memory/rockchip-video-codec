cmake_minimum_required(VERSION 3.21)
project(rkvc-core VERSION 0.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)

add_library(rkvc-core-static STATIC
    src/status.cpp
    src/diag.cpp
    src/spec.cpp
    src/frame.cpp
    src/sha256.cpp
    src/rkmodel.cpp
    src/request.cpp
    src/node.cpp
    src/registry.cpp
    src/graph.cpp
    src/executor.cpp
    src/session.cpp
    src/context.cpp
    src/plugin.cpp
    src/fileio.cpp
    src/c_api.cpp
)
add_library(rkvc::core ALIAS rkvc-core-static)

# Header-only doctest for downstream test targets.
add_library(rkvc-doctest INTERFACE)
target_include_directories(rkvc-doctest INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party>)
add_library(rkvc::doctest ALIAS rkvc-doctest)

target_include_directories(rkvc-core-static
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(rkvc-core-static
    PUBLIC Threads::Threads
    PRIVATE ${CMAKE_DL_LIBS})

foreach(t rkvc-core-static)
    target_compile_options(${t} PRIVATE -fno-exceptions -fno-rtti
        -Wall -Wextra -Werror=return-type)
    target_compile_definitions(${t} PUBLIC RKVC_NO_EXCEPTIONS=1)
endforeach()

if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    # Final links (tests, plugins, CLIs) carry no dynamic C++ runtime.
    add_link_options(-static-libstdc++ -static-libgcc)
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

    add_executable(test_rkmodel tests/test_rkmodel.cpp)
    target_link_libraries(test_rkmodel PRIVATE rkvc::core)
    target_include_directories(test_rkmodel PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
    target_compile_definitions(test_rkmodel PRIVATE
        DOCTEST_CONFIG_NO_EXCEPTIONS)
    target_compile_options(test_rkmodel PRIVATE -fno-exceptions -fno-rtti)
    add_test(NAME test_rkmodel COMMAND test_rkmodel)

    foreach(t test_queue test_pipeline test_c_abi)
        add_executable(${t} tests/${t}.cpp)
        target_link_libraries(${t} PRIVATE rkvc::core)
        target_include_directories(${t} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
        target_compile_definitions(${t} PRIVATE
            DOCTEST_CONFIG_NO_EXCEPTIONS)
        target_compile_options(${t} PRIVATE -fno-exceptions -fno-rtti)
        add_test(NAME ${t} COMMAND ${t})
    endforeach()

    # Plugin handshake fixtures: built as DSOs, loaded by test_plugin.
    foreach(p identity badabi empty)
        add_library(test_plugin_${p} MODULE tests/plugins/${p}.cpp)
        target_include_directories(test_plugin_${p} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_compile_options(test_plugin_${p} PRIVATE
            -fno-exceptions -fno-rtti -Wall -Wextra)
    endforeach()

    add_executable(test_plugin tests/test_plugin.cpp)
    target_link_libraries(test_plugin PRIVATE rkvc::core)
    target_include_directories(test_plugin PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
    target_compile_definitions(test_plugin PRIVATE
        DOCTEST_CONFIG_NO_EXCEPTIONS
        RKVC_TEST_PLUGIN_IDENTITY="$<TARGET_FILE:test_plugin_identity>"
        RKVC_TEST_PLUGIN_BADABI="$<TARGET_FILE:test_plugin_badabi>"
        RKVC_TEST_PLUGIN_EMPTY="$<TARGET_FILE:test_plugin_empty>"
        RKVC_TEST_TMPDIR="${CMAKE_CURRENT_BINARY_DIR}")
    target_compile_options(test_plugin PRIVATE -fno-exceptions -fno-rtti)
    # The host executable must export core symbols for dlopened plugins.
    set_target_properties(test_plugin PROPERTIES ENABLE_EXPORTS ON)
    add_dependencies(test_plugin
        test_plugin_identity test_plugin_badabi test_plugin_empty)
    add_test(NAME test_plugin COMMAND test_plugin)
endif()
