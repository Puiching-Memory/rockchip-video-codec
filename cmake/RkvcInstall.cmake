# SPDX-License-Identifier: AGPL-3.0-or-later
# RkvcInstall.cmake — 安装规则：安装树是可移植包内容的唯一来源。
#
# 打包器（tools/rkvc-build）只消费 `cmake --install` 的结果，绝不手工
# 复制"应该存在"的二进制。组件划分：
#   runtime   库与 CLI（随包分发）
#   devel     头文件/pkg-config/CMake config（SDK 开发面）
#   backends  后端 DSO 目录（lib/rkvc/backends）
#   models    模型容器目录（share/rkvc/models）

include(GNUInstallDirs)

install(DIRECTORY include/rkvc
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    COMPONENT devel
)

if(RKVC_BUILD_SHARED)
    install(TARGETS rkvc_shared
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        COMPONENT runtime
        NAMELINK_COMPONENT devel
    )
endif()

if(RKVC_BUILD_STATIC)
    install(TARGETS rkvc_static
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        COMPONENT devel
    )
endif()

if(RKVC_BUILD_NEW_ENGINE)
    install(TARGETS rkvc_core
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        COMPONENT runtime
        NAMELINK_COMPONENT devel
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        COMPONENT devel
    )
    install(TARGETS rkvc_cli
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT runtime
    )
    # 后端 DSO 与模型容器的包内固定位置（内容可为空，由打包阶段填充）。
    install(DIRECTORY DESTINATION ${CMAKE_INSTALL_LIBDIR}/rkvc/backends
        COMPONENT runtime
    )
    install(DIRECTORY DESTINATION ${CMAKE_INSTALL_DATADIR}/rkvc/models
        COMPONENT runtime
    )
endif()

# 安装旧版 CLI 到 bin/（迁移期保留，P6 移除）
if(RKVC_CLI_TARGETS)
    install(TARGETS ${RKVC_CLI_TARGETS}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT runtime
    )
endif()

# pkg-config 文件
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/rkvc.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/rkvc.pc
    @ONLY
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/rkvc.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
    COMPONENT devel
)

# 许可证文本（随包审计材料）
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/rkvc
    COMPONENT licenses
)

# CMake config
include(CMakePackageConfigHelpers)
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/rkvc-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/rkvc-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rkvc
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/rkvc-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/rkvc-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/rkvc-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rkvc
    COMPONENT devel
)
