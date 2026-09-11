cmake_minimum_required(VERSION 3.21)
project(rkvc-core VERSION 0.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)

# SONAME 依据：从公开头文件读 C ABI 主版本（唯一权威来源，避免两处手写漂移）。
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/rkvc/rkvc.h" _rkvc_abi_line
    REGEX "^#define RKVC_ABI_VERSION_MAJOR +[0-9]+")
if(NOT _rkvc_abi_line)
    message(FATAL_ERROR "无法从 rkvc.h 解析 RKVC_ABI_VERSION_MAJOR")
endif()
string(REGEX REPLACE "^#define RKVC_ABI_VERSION_MAJOR +" ""
    RKVC_ABI_MAJOR "${_rkvc_abi_line}")

# 官方支持的源文件清单：静态库与 SDK 共享库共用，避免两份漂移。
set(RKVC_CORE_SOURCES
    src/status.cpp
    src/diag.cpp
    src/spec.cpp
    src/frame.cpp
    src/sha256.cpp
    src/model.cpp
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

add_library(rkvc-core-static STATIC ${RKVC_CORE_SOURCES})
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
    # Final links (tests, shared core, plugins, CLIs) carry no dynamic C++ runtime.
    # 必须排在 SDK 共享库之前：add_link_options 只作用于其后创建的目标。
    add_link_options(-static-libstdc++ -static-libgcc)
endif()

# SDK 面：预编译 librkvc.so + 头文件 + CMake config + pkg-config，供下游直接
# 链接而无需自持 core 源码（tools/portable/build.sh 打开）。共享 core 与包内
# 插件同源同编译器，插件指纹天然一致——这正是它比「宿主自编 core」省事的地方。
option(RKVC_INSTALL_SDK "构建 librkvc.so 并生成可安装的 SDK（头文件/config/pc）" OFF)
if(RKVC_INSTALL_SDK)
    add_library(rkvc-shared SHARED ${RKVC_CORE_SOURCES})
    set_target_properties(rkvc-shared PROPERTIES
        OUTPUT_NAME rkvc
        VERSION ${PROJECT_VERSION}
        # SONAME 跟 C ABI 主版本，不跟项目版本：0.x 期间改 ABI 也升 SONAME。
        SOVERSION ${RKVC_ABI_MAJOR}
        # 同目录即包内 lib/，比自己多跳两级更贴合实际布局。
        INSTALL_RPATH "\$ORIGIN"
        EXPORT_NAME core)
    target_include_directories(rkvc-shared
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>)
    target_link_libraries(rkvc-shared PRIVATE Threads::Threads
        ${CMAKE_DL_LIBS})
    target_compile_options(rkvc-shared PRIVATE -fno-exceptions -fno-rtti
        -Wall -Wextra -Werror=return-type)
    target_compile_definitions(rkvc-shared PRIVATE RKVC_NO_EXCEPTIONS=1)

    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    set(RKVC_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/rkvc"
        CACHE STRING "rkvc CMake 包文件安装位置（相对前缀）")

    install(TARGETS rkvc-shared EXPORT rkvcTargets
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION bin)
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/rkvc"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
    install(EXPORT rkvcTargets
        NAMESPACE rkvc::
        DESTINATION "${RKVC_INSTALL_CMAKEDIR}")

    # 两个包文件都写成「相对自身定位前缀」，包整体搬目录/解压到别处后仍可用。
    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/rkvcConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/rkvcConfig.cmake"
        INSTALL_DESTINATION "${RKVC_INSTALL_CMAKEDIR}")
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/rkvcConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/rkvcConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/rkvcConfigVersion.cmake"
        DESTINATION "${RKVC_INSTALL_CMAKEDIR}")

    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rkvc.pc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/rkvc.pc" @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/rkvc.pc"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
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

    add_executable(test_model tests/test_model.cpp)
    target_link_libraries(test_model PRIVATE rkvc::core)
    target_include_directories(test_model PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
    target_compile_definitions(test_model PRIVATE
        DOCTEST_CONFIG_NO_EXCEPTIONS
        RKVC_TEST_MODEL_TMPDIR="${CMAKE_CURRENT_BINARY_DIR}")
    target_compile_options(test_model PRIVATE -fno-exceptions -fno-rtti)
    add_test(NAME test_model COMMAND test_model)

    foreach(t test_queue test_pipeline test_c_abi test_download)
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
