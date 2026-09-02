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
    #
    # 头文件隔离（2026-09 板卡回归教训）: Ubuntu 交叉 gcc 的内建搜索链把
    # 宿主 /usr/<triplet>/include（glibc 2.39 头, 含 __isoc23_* C23 重定向）
    # 排在 --sysroot 头之前, 会在 glibc 2.31 产物里悄悄引入 2.38 符号。
    # -nostdinc + 显式 -I 完全接管系统头搜索: 只允许 gcc 内建 include
    # (stddef.h 等) 与锁定 sysroot 的通用/multiarch 头。
    #
    # 上面提到的 "-g[^ ]*" 正则同样作用于所有 -I 路径: 任何含 "-gnu" 的
    # 路径（gcc 内建目录、sysroot multiarch 目录）都会被剥成 "…-linux"
    # 而失效。因此这两个目录必须经不含 "-g" 的软链中转；软链在 configure
    # 时自动创建（幂等），路径可用环境变量覆盖。
    set(_rkvc_cross_cache "$ENV{RKVC_CROSS_CACHE}")
    if(_rkvc_cross_cache STREQUAL "")
        set(_rkvc_cross_cache "/root/.rkvc-cross")
    endif()
    set(_rkvc_gcc_builtin_inc "$ENV{RKVC_GCC_BUILTIN_INC}")
    if(_rkvc_gcc_builtin_inc STREQUAL "")
        set(_rkvc_gcc_builtin_inc "/usr/lib/gcc-cross/aarch64-linux-gnu")
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -E make_directory
                    ${_rkvc_cross_cache})
    execute_process(COMMAND ln -sfn ${_rkvc_gcc_builtin_inc}
                    ${_rkvc_cross_cache}/gccinc)
    execute_process(COMMAND ln -sfn
                    ${_rkvc_sysroot}/usr/include/aarch64-linux-gnu
                    ${_rkvc_cross_cache}/sysinc)
    # glibc 2.31 头的 `!__GNUC_PREREQ (7, 0) || defined __cplusplus` 守卫
    # 在 gcc 13 C++ 下仍为真: gcc 7 起 _FloatN 在 C++ 中也是内建类型,
    # 旧守卫导致 typedef 重定义 (-fpermissive 报错)。configure 时把
    # 系统头里的守卫收紧为 `!__GNUC_PREREQ (7, 0)` (与 glibc 2.35+ 一致)。
    execute_process(
        COMMAND sh -c
        "grep -rl '__GNUC_PREREQ (7, 0) || defined __cplusplus' \
${_rkvc_sysroot}/usr/include 2>/dev/null | xargs -r sed -i \
's/__GNUC_PREREQ (7, 0) || defined __cplusplus/__GNUC_PREREQ (7, 0)/g'")
    set(CMAKE_C_FLAGS_INIT
        "--sysroot=${_rkvc_sysroot} -static-libgcc -nostdinc \
-I${_rkvc_sysroot}/usr/include \
-I${_rkvc_cross_cache}/sysinc \
-I${_rkvc_cross_cache}/gccinc/13/include")
    set(CMAKE_CXX_FLAGS_INIT
        "--sysroot=${_rkvc_sysroot} -static-libgcc -static-libstdc++ -nostdinc \
-I${_rkvc_sysroot}/usr/include/c++/13 \
-I${_rkvc_cross_cache}/sysinc/c++/13 \
-I${_rkvc_sysroot}/usr/include \
-I${_rkvc_cross_cache}/sysinc \
-I${_rkvc_cross_cache}/gccinc/13/include")
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
