# SPDX-License-Identifier: AGPL-3.0-or-later
# Generic Linux/AArch64 cross toolchain for rkvc and bundled CMake projects.
#
# Optional environment variables:
#   RKVC_CROSS_PREFIX=aarch64-linux-gnu-
#   RKVC_SYSROOT=/path/to/aarch64/sysroot

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(_rkvc_cross_prefix "$ENV{RKVC_CROSS_PREFIX}")
if(_rkvc_cross_prefix STREQUAL "")
    set(_rkvc_cross_prefix "aarch64-linux-gnu-")
endif()

set(CMAKE_C_COMPILER "${_rkvc_cross_prefix}gcc")
set(CMAKE_CXX_COMPILER "${_rkvc_cross_prefix}g++")
set(CMAKE_ASM_COMPILER "${_rkvc_cross_prefix}gcc")
set(CMAKE_AR "${_rkvc_cross_prefix}ar" CACHE FILEPATH "target archiver")
set(CMAKE_RANLIB "${_rkvc_cross_prefix}ranlib" CACHE FILEPATH "target ranlib")
set(CMAKE_STRIP "${_rkvc_cross_prefix}strip" CACHE FILEPATH "target strip")

set(_rkvc_sysroot "$ENV{RKVC_SYSROOT}")
if(NOT _rkvc_sysroot STREQUAL "")
    set(CMAKE_SYSROOT "${_rkvc_sysroot}")
    set(CMAKE_FIND_ROOT_PATH "${_rkvc_sysroot}")
endif()

# Build helpers always come from the x86 host; headers and libraries must come
# from the target sysroot/install prefixes.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
