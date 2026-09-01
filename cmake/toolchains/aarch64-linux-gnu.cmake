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
    # 发行版交叉 gcc 自带的启动文件/libgcc 面向其构建时 glibc（可能远高于
    # 发布基线）。强制从固定 sysroot 取 crt1/crti/crtn，并静态链接 libgcc，
    # 避免把宿主工具链的 GLIBC 版本需求引入目标产物；验证器是最终判据。
    # 链接基线: 必须让 -lc/-lm 命中固定 sysroot (glibc 2.31) 而不是宿主
    # 交叉工具链的 glibc (Ubuntu 24.04 的 gcc-13 自带 2.39)。
    # 关键约束: -B<sysroot>/usr/lib/aarch64-linux-gnu 不能放进 CMAKE_C_FLAGS,
    # 因为 third_party/mpp/CMakeLists.txt 会用 "-g[^ ]*" 正则清 -g 调试选项,
    # 误把路径里的 "-gnu" 也删掉 (-B.../aarch64-linux-gnu -> -B.../aarch64-linux),
    # 目录失效后 gcc 静默回落宿主 libc。链接 flags 不受该正则影响。
    # -B 用于把 crt 文件与 libc.so 链接脚本指向 sysroot; --sysroot 用于让
    # gcc 驱动把脚本内绝对路径(/lib/...、/usr/lib/...)改写进 sysroot。
    set(CMAKE_C_FLAGS_INIT "--sysroot=${_rkvc_sysroot} -static-libgcc")
    set(CMAKE_CXX_FLAGS_INIT
        "--sysroot=${_rkvc_sysroot} -static-libgcc -static-libstdc++")
    set(CMAKE_EXE_LINKER_FLAGS_INIT
        "-B${_rkvc_sysroot}/usr/lib/aarch64-linux-gnu --sysroot=${_rkvc_sysroot} -static-libgcc")
    set(CMAKE_SHARED_LINKER_FLAGS_INIT
        "-B${_rkvc_sysroot}/usr/lib/aarch64-linux-gnu --sysroot=${_rkvc_sysroot} -static-libgcc")
endif()

# Build helpers always come from the x86 host; headers and libraries must come
# from the target sysroot/install prefixes.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
