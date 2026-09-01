# SPDX-License-Identifier: AGPL-3.0-or-later
# The install tree is the only input accepted by the package builder.

include(GNUInstallDirs)

install(FILES
    include/rkvc/api.h
    include/rkvc/backend.h
    include/rkvc/context.h
    include/rkvc/request.h
    include/rkvc/job.h
    include/rkvc/frame.h
    include/rkvc/diagnostic.h
    include/rkvc/model.h
    include/rkvc/rkvc.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/rkvc
    COMPONENT devel)

if(RKVC_BUILD_SHARED)
    install(TARGETS rkvc_shared
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        COMPONENT runtime
        NAMELINK_COMPONENT devel)
endif()

if(RKVC_BUILD_STATIC)
    install(TARGETS rkvc_static
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        COMPONENT devel)
endif()

if(RKVC_BUILD_CLI)
    install(TARGETS rkvc_cli
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT runtime)
endif()

install(DIRECTORY DESTINATION ${CMAKE_INSTALL_LIBDIR}/rkvc/backends
    COMPONENT runtime)
install(DIRECTORY DESTINATION ${CMAKE_INSTALL_DATADIR}/rkvc/models
    COMPONENT runtime)

if(TARGET rkvc_backend_mpp)
    install(TARGETS rkvc_backend_mpp
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/rkvc/backends
        COMPONENT backends)
endif()

set(RKVC_PC_DESCRIPTION "Rockchip media graph library and backend ABI")
set(RKVC_PC_REQUIRES_PRIVATE "")
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/rkvc.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/rkvc.pc
    @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/rkvc.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
    COMPONENT devel)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/rkvc
    COMPONENT licenses)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/rkvc-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/rkvc-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rkvc)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/rkvc-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/rkvc-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/rkvc-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rkvc
    COMPONENT devel)
