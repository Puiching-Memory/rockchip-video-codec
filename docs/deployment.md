# 部署

本仓**不设 install 规则、不产安装树**：发布产物是 `tools/portable/`
交叉构建出的可移植包，源树自身保持"构建树即产物"。旧 C 树的
`tools/rkvc-build`、SBOM/provenance 流水线与 glibc 2.31 基线已随旧树
删除，不再维护。

## 可移植包

~~~bash
tools/portable/build.sh                 # 全量：MPP + SVT-AV1 + rknnrt
tools/portable/build.sh --no-rknn       # 不收 NPU 运行库（mlvc/sr 落 stub）
tools/portable/build.sh --models DIR    # 另收 DIR 下模型 bundle 目录进包
~~~

输出 `.build/dist/rkvc-<版本>-linux-aarch64-portable.tar.gz`（附 `.sha256`）。
构建全程跑在 `tools/portable/Dockerfile` 的 jammy 镜像里（宿主机有
docker 时脚本自动重入）：镜像固定交叉工具链，目标 glibc 2.35 与 RK3576
板（Ubuntu 22.04）对齐，产物 GLIBC 引用 ≤ 2.34、可跑在更新的 glibc 上。
依赖按子模块 commit 缓存于 `.build/portable/deps/`，重复构建只重编 rkvc
自身。

包布局（RUNPATH 全部 `$ORIGIN` 相对，整包可搬迁）：

~~~text
bin/rkvc                       # CLI（C++ 运行时静态链接）
lib/rkvc/backends/rkvc_*.so    # h264h265 / av1 / mlvc / sr 插件
lib/librkvc.so.0.5.0           # SDK 主体（预编译 core，供上游链接；§SDK 面）
lib/librkvc-<codec>.so.0.5.0   # codec SDK（五个 codec，pspack 无插件入口）
include/rkvc/                  # 16 个公开头文件（rkvc.h + C++ 头）
include/<codec>/               # codec 自己的公开头文件
lib/cmake/rkvc/                # find_package(rkvc CONFIG) 的包文件
lib/pkgconfig/rkvc.pc          # pkg-config（前缀由 pcfiledir 反推，可搬迁）
lib/cmake/rkvc-<codec>/        # find_package(rkvc-<codec> CONFIG) 的包文件
lib/pkgconfig/rkvc-<codec>.pc  # codec 版 pkg-config（Requires: rkvc）
lib/librockchip_mpp.so.1       # MPP（h264h265 插件用）
lib/librknnrt.so               # RKNN 运行库 2.3.2（mlvc/sr 用，SHA-256 固定）
lib/libSvtAv1Enc.so.4          # SVT-AV1（av1 插件用）
docs/                          # 全套文档（原样收录，入口 docs/index.md）
examples/                      # 三个 C ABI 样板（integration-c/decode-file/upscale-file）
models/<bundle>/               # 可选，--models 收录（目录即装载单元）
licenses/                      # AGPLv3 + 第三方文本 + PROVENANCE.txt
CHANGELOG.md                   # 版本历史（docs 里有链接引到这里）
test.sh                        # 包内自测（含 SDK 面校验与 C 消费者冒烟）
MANIFEST.sha256                # 全包校验和
~~~

包是复合产品：`bin/rkvc` 供直接使用，`include/ + lib/librkvc.so + lib/cmake +
lib/pkgconfig` 供上游链接（三种用法与 SDK 细节见
[可移植包 × 宿主集成 §1–§2](portable-package.md)）。`librkvc.so` 与包内插件
同源同编译器，插件指纹天然一致；它自己只依赖 glibc（C++ 运行时静态链入）。

`docs/`、`examples/` 都是仓库里那两份原样收录，不为包单独维护副本；不含
doxide 生成的 `docs/api/`（按头文件现生成、不入库，入包会让包内容随构建机
变化）。示例两种构建方式都支持：`-Drkvc_DIR=<pkg>/lib/cmake/rkvc`（用包内
SDK，不需要源码树）或 `-DRKVC_CORE_DIR=<core 目录>`（自带源码）。
`docs/` 里提到的 `tools/` 等路径指的是仓库，不在包内。
文档图片走 git-lfs，构建机未拉全时 `build.sh` 会直接报错而不是交付坏图。

## 板端部署

整包 scp 到板子解压即可；插件与运行库按包内相对路径自动发现
（`bin/../lib/rkvc/backends`，发现顺序见
[SDK 集成 §4.1](semantic-codec-sdk-integration.md)），CLI 不带
`--backend-dir` 也能装载。要指向包外的插件/模型仍可显式传
`--backend-dir` / `--model-dir`（或逐个 `--model`）。

宿主（如 `semantic-codec-sdk`）与这个包怎么拼装——插件发现位、工具链指纹
约束、验收与排障——见[可移植包 × 宿主集成](portable-package.md)。

先跑 `./test.sh`：覆盖布局、校验和、依赖解析、插件握手、SDK 面（头文件/
`librkvc.so`/CMake config/pkg-config + 纯 C 消费者零配置建会话）、av1 软编码与
MPP 硬编解码冒烟，无硬件项自动跳过（缺编译器时 SDK 段跳过）。`librknnrt.so` 自身动态依赖目标机的
`libstdc++.so.6` / `libgcc_s.so.1`，系统自带即可。

## 直接部署构建树

不用包时也可手工复制构建树（预设与目录见
[快速开始](getting-started.md)）：

~~~text
.build/release/rkvc            # CLI
.build/release/rkvc_*.so       # 所需 codec 插件
<模型目录>/                     # 原生模型文件（tools/mlvc、tools/sr 导出）
~~~

此时插件与 CLI 必须来自同一次构建（工具链指纹握手会拒载混版 `.so`），
并用 `--backend-dir` / `--model-dir` 指过去；第三方运行时由目标机系统
路径或自备前缀目录提供。

## 符号审计

aarch64 产物静态链接 libstdc++/libgcc；`tools/portable/build.sh` 打包时
自动跑 `tools/check-symbols.sh`（GLIBC ≤ 2.34、NEEDED 禁动态 C++
运行时）。该审计只对 aarch64 产物有意义：x86 构建天然动态链接系统
libstdc++，不跑这一步。
