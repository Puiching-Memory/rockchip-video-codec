# chsrc v0.2.7 官方二进制（换源工具）；版本与 SHA-256 成对钉死，升级需同改。
FROM alpine:3.20 AS chsrc-fetch
ARG TARGETARCH
RUN set -eu; \
    case "$TARGETARCH" in \
        amd64) asset=chsrc-x64-linux; \
               sha=70d236dbd346e969a3d2abe507343eb07419be494ff40fd117cad44c619465d5 ;; \
        arm64) asset=chsrc-aarch64-linux; \
               sha=64e176a6cdcfdaa7f25a604ed2be62587d81dfe1e048608302384da1c4063703 ;; \
        *) echo "不支持的构建架构: $TARGETARCH" >&2; exit 1 ;; \
    esac; \
    wget -qO /chsrc \
        "https://github.com/RubyMetric/chsrc/releases/download/v0.2.7/$asset"; \
    echo "$sha  /chsrc" | sha256sum -c -; \
    chmod 755 /chsrc

# 可移植包的交叉构建环境。
#
# jammy 是刻意选的基线：它的 aarch64 交叉工具链目标 glibc 2.35，与板卡机群
# （RK3576 = Ubuntu 22.04）一致，产物 GLIBC 引用上限 2.34，正好落在
# tools/check-symbols.sh 的门限内；noble 的交叉库是 2.39，会把运行下限白白
# 抬高一档。把工具链固定进镜像，替代「宿主交叉 gcc + 自建旧 sysroot +
# -nostdinc 接管头搜索」那套 flag 手术——版本差异交给镜像 tag 管理。
FROM ubuntu:22.04

# 装包前先换源：chsrc 默认切到其维护团队实测最快的镜像，官方
# archive.ubuntu.com 在部分网络下慢到不可用。换源后 apt 不走代理——镜像站
# 本就该直连，绕过代理反而更快（部分代理还会拒绝境内目标）。
# CHSRC_MIRROR 可传具体镜像名（chsrc list ubuntu 查看）或 default 保持官方源。
COPY --from=chsrc-fetch /chsrc /usr/local/bin/chsrc
ARG CHSRC_MIRROR=first
RUN set -eu; \
    if [ "$CHSRC_MIRROR" != default ]; then \
        http_proxy= https_proxy= all_proxy= \
            chsrc set ubuntu "$CHSRC_MIRROR" >/tmp/chsrc.log 2>&1 ||
            { cat /tmp/chsrc.log >&2; exit 1; }; \
        rm -f /tmp/chsrc.log; \
        unset http_proxy https_proxy all_proxy; \
    fi; \
    apt-get update; \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git pkg-config ca-certificates curl \
        gcc-aarch64-linux-gnu g++-aarch64-linux-gnu binutils-aarch64-linux-gnu \
        qemu-user-static; \
    rm -rf /var/lib/apt/lists/*
