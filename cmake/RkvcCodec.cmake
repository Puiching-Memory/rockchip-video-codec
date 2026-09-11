# rkvc codec 工程的公共 CMake 片段：rkvc::core 来源解析 + SDK 面（共享库 /
# 头文件 / CMake config / pkg-config）。只收这两段最容易在各 codec 工程之间
# 漂移的逻辑，codec 之间不因此产生任何代码或依赖耦合。

# 本文件所在目录。宏/函数体内的 CMAKE_CURRENT_LIST_DIR 指向调用者目录，
# 所以在文件作用域先取一次。
set(RKVC_CMAKE_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL
    "rkvc 公共 CMake 模块目录")

# 解析 rkvc::core。顺序：
#   1. 仓库内 core/ 源码（开发默认：插件工具链指纹与自身编译器天然一致）
#   2. -DRKVC_CORE_DIR=<core 源码目录>（把本工程拷出仓库单独构建）
#   3. find_package(rkvc CONFIG)（消费已安装的预编译 SDK）
# 用 macro 而非 function：add_subdirectory 需要在调用者目录作用域展开，
# 展开后与手写等价，不引入额外的作用域副作用。
macro(rkvc_resolve_core)
    if(NOT TARGET rkvc::core)
        set(RKVC_CORE_DIR "" CACHE PATH "rkvc core 源码目录（默认取仓库内 core/）")
        if(RKVC_CORE_DIR)
            add_subdirectory("${RKVC_CORE_DIR}" rkvc-core-build)
        elseif(EXISTS "${RKVC_CMAKE_MODULE_DIR}/../core/CMakeLists.txt")
            add_subdirectory("${RKVC_CMAKE_MODULE_DIR}/../core" rkvc-core-build)
        else()
            find_package(rkvc CONFIG REQUIRED)
        endif()
    endif()
endmacro()

# SDK 面：RKVC_INSTALL_SDK=ON 时，用同一份源码再编一个共享库并安装
# （lib<name>.so + include/<INCLUDE_DIR>/ + lib/cmake/<name> + <name>.pc）。
# 静态库继续留在构建树里供单测与内嵌形态使用，与 core 只安装 rkvc-shared
# 的做法对称。
#   rkvc_codec_sdk(NAME <名> INCLUDE_DIR <公开头文件根目录> SOURCES <源文件...>
#                  [PRIVATE_INCLUDE <目录...>] [PRIVATE_LINK <目标...>])
# NAME 省略时取 ${PROJECT_NAME}；INCLUDE_DIR 是头文件根目录（形如 include，
# 其下的 <codec>/ 子目录即头文件命名空间，与静态库的用法一致）。
# PRIVATE_INCLUDE/PRIVATE_LINK 用来补回静态库私有的外部依赖（MPP / RKNN 的
# include 与库），两者各写一份、互不影响。
function(rkvc_codec_sdk)
    if(NOT RKVC_INSTALL_SDK)
        return()
    endif()
    cmake_parse_arguments(S "" "NAME;INCLUDE_DIR" "SOURCES;PRIVATE_INCLUDE;PRIVATE_LINK"
        ${ARGN})
    if(NOT S_NAME)
        set(S_NAME "${PROJECT_NAME}")
    endif()
    if(NOT S_NAME OR NOT S_INCLUDE_DIR OR NOT S_SOURCES)
        message(FATAL_ERROR "rkvc_codec_sdk: 需要 INCLUDE_DIR 与 SOURCES")
    endif()

    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    add_library(${S_NAME} SHARED ${S_SOURCES})
    set_target_properties(${S_NAME} PROPERTIES
        EXPORT_NAME codec
        # 0.x 期间与 librkvc.so 同用主版本 0：出兼容性变更就升它。
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
        # 同目录即包内 lib/，与 librkvc.so 同处一地。
        INSTALL_RPATH "\$ORIGIN")
    target_include_directories(${S_NAME}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/${S_INCLUDE_DIR}>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    if(TARGET rkvc-shared)
        # 同源构建：core 已有共享库就别静态吸收第二份（全局状态会成两套）。
        target_link_libraries(${S_NAME} PRIVATE rkvc-shared)
    else()
        target_link_libraries(${S_NAME} PRIVATE rkvc::core)
    endif()
    if(S_PRIVATE_INCLUDE)
        target_include_directories(${S_NAME} PRIVATE ${S_PRIVATE_INCLUDE})
    endif()
    if(S_PRIVATE_LINK)
        target_link_libraries(${S_NAME} PRIVATE ${S_PRIVATE_LINK})
    endif()
    target_compile_options(${S_NAME} PRIVATE -fno-exceptions -fno-rtti
        -Wall -Wextra -Werror=return-type)
    target_compile_definitions(${S_NAME} PUBLIC RKVC_NO_EXCEPTIONS=1)

    set(_cmakedir "${CMAKE_INSTALL_LIBDIR}/cmake/${S_NAME}")
    set(CODEC_NAME "${S_NAME}")
    set(CODEC_VERSION "${PROJECT_VERSION}")

    install(TARGETS ${S_NAME} EXPORT ${S_NAME}Targets
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
    # 末尾的 / 让 install(DIRECTORY) 取内容而不是把 include 本身再套一层。
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${S_INCLUDE_DIR}/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
    install(EXPORT ${S_NAME}Targets
        NAMESPACE ${S_NAME}::
        DESTINATION "${_cmakedir}")

    configure_package_config_file(
        "${RKVC_CMAKE_MODULE_DIR}/codecConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/${S_NAME}Config.cmake"
        INSTALL_DESTINATION "${_cmakedir}")
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/${S_NAME}ConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/${S_NAME}Config.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/${S_NAME}ConfigVersion.cmake"
        DESTINATION "${_cmakedir}")

    configure_file("${RKVC_CMAKE_MODULE_DIR}/codec.pc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/${S_NAME}.pc" @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${S_NAME}.pc"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
endfunction()
