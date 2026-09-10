# 可移植包交叉工具链：与 tools/portable/Dockerfile 的镜像配套使用。
#
# 编译器与目标 libc 由镜像固定（jammy → glibc 2.35），这里只做三件事：
# 打开交叉模式、把查找范围限制在目标前缀、把包内布局写进 RUNPATH。

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_AR aarch64-linux-gnu-ar CACHE FILEPATH "交叉 ar")
set(CMAKE_RANLIB aarch64-linux-gnu-ranlib CACHE FILEPATH "交叉 ranlib")
set(CMAKE_STRIP aarch64-linux-gnu-strip CACHE FILEPATH "交叉 strip")

# 工具走宿主，头文件与库只认目标前缀（deps/* 与各 *_INSTALL_PREFIX），
# 避免把宿主 x86 库悄悄链进目标产物。
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# 包布局：插件在 <root>/lib/rkvc/backends，随包运行库在 <root>/lib，因此
# 链接期就写入「安装后」的 $ORIGIN 相对 RUNPATH，产物拷进包即可重定位。
# 构建树里这些路径落空无妨，跑测试时由 LD_LIBRARY_PATH 指 deps 前缀。
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set(CMAKE_INSTALL_RPATH "\$ORIGIN/../..")
