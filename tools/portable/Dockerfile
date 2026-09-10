# 可移植包的交叉构建环境。
#
# jammy 是刻意选的基线：它的 aarch64 交叉工具链目标 glibc 2.35，与板卡机群
# （RK3576 = Ubuntu 22.04）一致，产物 GLIBC 引用上限 2.34，正好落在
# tools/check-symbols.sh 的门限内；noble 的交叉库是 2.39，会把运行下限白白
# 抬高一档。把工具链固定进镜像，替代「宿主交叉 gcc + 自建旧 sysroot +
# -nostdinc 接管头搜索」那套 flag 手术——版本差异交给镜像 tag 管理。
FROM ubuntu:22.04

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y \
        --no-install-recommends \
        build-essential cmake ninja-build git pkg-config ca-certificates curl \
        gcc-aarch64-linux-gnu g++-aarch64-linux-gnu binutils-aarch64-linux-gnu \
        qemu-user-static \
    && rm -rf /var/lib/apt/lists/*
