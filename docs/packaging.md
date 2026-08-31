# 打包与分发

## 0.4 核心包（rkvc-build，新流水线）

0.4 起发布产物由单一编排器产出，阶段链
`sysroot -> build-install -> release-meta -> seal -> verify -> archive -> qemu-smoke`
全部可重放、确定性归档、产物验证失败即失败：

```bash
python3 tools/rkvc-build package --jobs $(nproc)
# 成品: .build/portable/dist/rkvc-<version>-linux-aarch64-glibc231-portable.tar.gz
# 内容: bin/rkvc, lib/librkvc_core.so.0*, lib/rkvc/backends/,
#       share/rkvc/models/, bom.cyclonedx.json, legal/, SHA256SUMS, provenance.json

python3 tools/rkvc-build verify <包目录>     # 单独重跑产物验证
python3 tools/rkvc-build package --refresh-sysroot   # 重建锁定 sysroot
```

sysroot 为 Ubuntu 20.04 (glibc 2.31) arm64 固定包集
（`tools/rkvc_build/sysroot.focal-arm64.lock.json`，SHA-256 锁定）。
当前核心包不含媒体后端（P3 迁移中）；媒体功能请暂用下方 0.3 路径。

## 可移植包（0.3 旧媒体栈，迁移期保留）

> **弃用预告**：`scripts/package-portable.sh` 将在媒体后端完成 P3 迁移、
> 统一 CLI 功能平价后退役（见 docs/0.4.0-refactor-plan.md P6）。

从源码构建，核心运行库随包携带，解压即用：

```bash
git submodule update --init --depth 1

./scripts/package-portable.sh                          # 默认：探测本机 SoC，单平台包
./scripts/package-portable.sh --platforms rk3576,rk3588,rv1126b   # 多目标板，每平台一个包
# 编译树: .build/portable/（preset portable，只编译一次）
# 成品包: .build/dist/rkvc-<version>-linux-<platform>-portable/（每平台一个）

# 包名由 CMake project(VERSION) + 平台生成
source scripts/build-common.sh
./scripts/test-portable.sh ".build/dist/$(rkvc_portable_pkg_dir rk3576)"
```

产物：`rkvc-*-linux-<platform>-portable.tar.gz`（每平台一个，含 `librga` + `librknnrt` + 该平台 `models/`）

### 在 x86_64 服务器交叉打包与测试 AArch64 包

无需在 Rockchip 板卡上完成编译。打包脚本可以把本项目、MPP、SVT-AV1、
ffmpeg-rockchip 和 libsodium 交叉编译为 AArch64；模型转换工具仍作为 x86_64
宿主程序运行，librga/librknnrt 则选取 AArch64 发行文件。QEMU user-mode 用于
验证目标 ELF 能否加载及 CLI 基本行为。

Ubuntu 24.04 构建机需要 AArch64 交叉工具链、QEMU，以及目标架构的开发库：

```bash
sudo dpkg --add-architecture arm64
# amd64 仓库须限制为 amd64，并为 arm64 配置 ports.ubuntu.com；CI workflow
# 中的 cross-package job 给出了完整的 deb822 源配置。
sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
  libc6-dev-arm64-cross libdrm-dev:arm64 zlib1g-dev:arm64 \
  qemu-user cmake ninja-build make pkg-config patchelf binutils curl git \
  python3 python3-venv autoconf automake libtool

git submodule update --init --depth 1 --recursive
BUILD_JOBS=8 ./scripts/package-portable.sh \
  --target-arch aarch64 \
  --platforms rk3588 \
  --qemu-test
```

上述命令生成含 NPU runtime、MLVC-S 与 Phase-RLFN SR 模型的完整包：
`.build/cross/aarch64/dist/rkvc-*-linux-rk3588-portable.tar.gz`。打包脚本会自动调用
`prepare-model-env.sh`，按 `uv.lock` 创建/同步 x86_64 宿主上的 rknn-toolkit2
模型生产环境；如果机器没有 `uv`，会先把固定版本安装到
`.build/host/uv-bootstrap/`。模型加密所用的 host/target libsodium 也会分别
构建，不能混用。

若只需快速验证 C/C++ 交叉工具链，可追加 `--no-rknn`，生成包名带
`-no-npu` 的精简包并跳过 Python 环境和模型导出。

环境同步显式使用锁文件记录的 PyPI 源，避免构建机全局 `~/.config/uv/uv.toml`
把仓库锁文件改写成私有镜像地址；确有内部镜像锁文件时可通过
`RKVC_UV_DEFAULT_INDEX` 覆盖。

可单独复测已有的解包目录：

```bash
./scripts/test-portable-cross.sh \
  .build/cross/aarch64/dist/rkvc-*-linux-rk3588-portable \
  --target aarch64 --qemu qemu-aarch64
```

测试分为两层：`check-elf-deps.py` 不执行目标程序，检查全部 ELF 架构、
`DT_NEEDED` 闭包和关键库是否确实随包携带；随后 QEMU 执行 `rkvc_info`
和 CLI 负向冒烟测试。QEMU user-mode 不模拟 Rockchip MPP/RGA/RKNN 设备和
ioctl，因此硬件编解码、缩放和 NPU 推理仍必须在对应 SoC 的目标板上完成最终门禁。

高级环境可用 `--cross-prefix PREFIX` 覆盖默认的 `aarch64-linux-gnu-`，或用
`--sysroot DIR` 指定自备目标 sysroot。宿主工具和目标库分别放入
`.build/host/` 与 `.build/cross/aarch64/`，避免 x86_64/AArch64 产物串用。

### 模型自动生产（打包时）

启用 RKNN 时，打包脚本会先对每个目标平台调用 `scripts/build-models.sh` 自动生产
模型（产物暂存 `.build/models/<platform>/`，不污染仓库 `models/`，幂等缓存）：

| 模型 | 权重来源（自动下载，SHA-256 钉住） | 说明 |
| --- | --- | --- |
| MLVC | `mlvideopub.blob.core.windows.net/mlvc/models/mlvc-psnr-v1.ckpt` | Microsoft 官方公开容器 |
| MLVC-S | `mlvideopub.blob.core.windows.net/mlvc/models/mlvc-s-psnr-v1.ckpt` | 同上；属上游非承诺资源，失效时可 `--weights-path` 手动提供 |
| Phase-RLFN SR | `huggingface.co/Sail2Dream/phase-rlfn-codec-v1`（`best_ema.pth`） | QAT checkpoint，免校准（`--no-quantize`）；可用 `--sr-weight` 覆盖、`RKVC_SR_WEIGHT_URL`/`RKVC_SR_WEIGHT_SHA256` 换源 |

打包选项：
- `--platforms rk3588,rk3576,rk3568,rk3566,rv1126b`：逗号分隔目标板列表，每平台产一个包；缺省探测本机 SoC（`/proc/device-tree/compatible`）。RK3568/RK3566 只生产 MLVC bundle，因 SR exporter 不支持这两个 target，会明确跳过 SR。
- `--mlvc-variants mlvc|mlvc-s|all`：随包的 MLVC 变体，默认 `mlvc-s`（只带轻量版，包体减 ~85MB）；`all` 同时携带两个变体。
- `--sr-weight PATH`：本地 SR 权重，缺省自动从 HuggingFace 下载。
- `--allow-skip-sr`：SR 权重不可得/导出失败时警告跳过（默认报错）。
- `--no-encrypt-models`：关闭模型自研加密（默认开启，见下节）。
- `--no-rknn`：不下载 librknnrt、不启用 NPU、不生产模型（CI 用）；包名追加 `-no-npu`，避免与完整能力包混淆。
- `--no-model-env-sync`：不自动运行锁文件环境同步；仅用于离线镜像或已经预置 `RKVC_MODEL_PYTHON` 的构建机。
- `--no-test`：打包后不自动运行包内自测。默认每个平台包产出后自动跑 `test.sh`（仅对平台与本机 SoC 匹配的包；日志落盘 `<pkg>.test.log`），失败则打包以错误退出。

### 模型自研加密（默认开启）

包内 `.rknn` 模型默认经自研加密层保护（`RKVC_ENABLE_MODEL_CRYPT`，libsodium `crypto_secretbox` / XSalsa20-Poly1305），
不依赖 Rockchip `rknn_crypt_tool`（aarch64 wheel 不附带该工具）：

- 模型体用随机数据密钥 `data.key` 加密，打包时由 `rkvc_model_crypt encrypt` 在包内原地完成；
- `data.key` 不随包分发，而是用内嵌进 `librkvc`（XOR 混淆）的主密钥 `master.key`
  密封进每机一份的 `model.key`（内含目标机机器码，与 1机1码同一指纹）；
- 运行时加载模型自动解密：先解 `model.key`、校验本机机器码，通过才解密模型体。
  包拷到未签发 `model.key` 的机器上模型不可加载。
- 密钥位于 `tools/keys/{master.key,data.key}`（首次打包自动生成，已 gitignore）；
  必须成对离线备份。任一文件丢失时打包会终止，不会自动轮换并作废历史模型和 `model.key`；`--no-encrypt-models` 可关闭。

该机制用于阻止未修改客户端上的直接跨机复制，不是硬件级 DRM：对称主密钥和模型明文最终必须在客户进程中可用，有能力逆向或转储进程的攻击者仍可提取。符号隐藏和 XOR 混淆只降低误用与低成本提取，不应作为密钥不可恢复的安全承诺。需要更强保证时应把设备根密钥和解密操作放入 TEE/安全世界或远程密钥服务。

交付包只包含运行所需的 `.rknn`，完整明文权重的 `.onnx` 仅保留在打包机 `.build/models/` 生产缓存。加密后脚本会刷新 `sr_export_manifest.json` 中的密文大小与 SHA-256，并由包内测试重新验证。

客户签发流程（打包结束后脚本会打印同样提示）：

1. 客户机运行标准包内的 `bin/rkvc_model_id` 采集机器码；该工具只有采集能力，不含密钥生成、签发或解密命令；
2. 打包方：`rkvc_model_crypt issue -d tools/keys/data.key -m tools/keys/master.key -M <客户机器码> -o model.key`；
3. 客户放置 `~/.config/rkvc/model.key` 或设置 `RKVC_MODEL_KEY_FILE`。

错误语义：无 `model.key` → `RKVC_ERR_UNLICENSED`；机器码不符/文件被篡改 → `RKVC_ERR_LICENSE`。

容器内默认拒绝使用容易漂移的 MAC 地址兜底。部署时应把宿主的
`/proc/device-tree/serial-number` 只读挂载进容器，或确保容器能读取 Rockchip OTP；
诊断时可运行 `rkvc_model_id` 查看每一路指纹失败原因。只有明确接受容器 MAC
漂移风险时才设置 `RKVC_LICENSE_ALLOW_CONTAINER_MAC=1`。

### 授权构建（可选）

打包时可选开启 1机1码强制授权。`--license` 会自动完成全部密钥管理：

1. 构建 `libsodium` 子模块
2. 编译临时 `rkvc_lic` 工具（仅依赖 libsodium）
3. 检查 `tools/keys/secret.key` + `public.key`，**不存在则自动生成**密钥对（首次）
4. 用公钥自动注入 CMake 编译（`RKVC_LICENSE_PUBKEY_FILE`），不再使用演示密钥
5. 用私钥签发**本机自测 license**（`.build/dist/<pkg>.lic`，不随包分发）
6. 打包**客户版** `rkvc_lic` 到 `bin/`，仅支持 `machine-id` 与 `verify`，
   用于客户机采集机器码和自验证；**不含 `genkey`/`issue`/`inspect` 等签发能力**。

```bash
# 强制授权版（首次自动生成密钥对）
./scripts/package-portable.sh --license
# 成品包: .build/dist/rkvc-<version>-linux-<platform>-portable-licensed/
# 自测license: .build/dist/rkvc-<version>-linux-<arch>-portable-licensed.lic

# 本机自测（打包机自身）
RKVC_LICENSE_FILE=".build/dist/rkvc-<version>-linux-<arch>-portable-licensed.lic" \
  .build/dist/rkvc-<version>-linux-<arch>-portable-licensed/bin/rkvc_info --version
```

| 选项        | CMake                     | 包名后缀    | 运行时行为                          |
| ----------- | ------------------------- | ----------- | ----------------------------------- |
| _(无)_      | `RKVC_ENABLE_LICENSE=OFF` | _(无)_      | 无授权校验                          |
| `--license` | `RKVC_ENABLE_LICENSE=ON`  | `-licensed` | `rkvc_init()` 须有效 license 方通过 |

**密钥管理**（完整版 `rkvc_lic` 仅用于打包机内部；分发包内为裁剪版，无签发能力）：

| 文件           | 路径                       | gitignore | 用途                                       |
| -------------- | -------------------------- | --------- | ------------------------------------------ |
| 私钥           | `tools/keys/secret.key`    | ✅ 已忽略  | 签发 license，保管在打包机                 |
| 公钥           | `tools/keys/public.key`    | ❌ 可提交  | 嵌入 librkvc 编译                          |
| 打包方完整工具 | `.build/portable/rkvc_lic` | —         | 打包脚本内部生成密钥、签发本机测试 license |
| 客户机裁剪工具 | `bin/rkvc_lic`             | —         | 分发版，仅 `machine-id` / `verify`         |

**客户签发流程**（客户机运行的是裁剪版，打包方使用完整版 `rkvc_lic`）：

```bash
# 1. 客户机采集机器码（包内 rkvc_lic）
rkvc_lic machine-id

# 2. 打包方签发 license（用打包机私钥）
rkvc_lic issue -m <客户机器码> -k tools/keys/secret.key -o customer.lic

# 3. 客户放置 license
cp customer.lic ~/.config/rkvc/license.lic
# 或设置环境变量
export RKVC_LICENSE_FILE=/path/to/customer.lic
```

> **前置依赖**：`--license` 需 `third_party/libsodium` 子模块及
> autotools（`autoconf` / `automake` / `libtool`）。`libsodium` 静态链接，不随包分发 `.so`。

```
rkvc-*-linux-<platform>-portable/
├── bin/
│   ├── rkvc_encode
│   ├── rkvc_decode
│   ├── rkvc_transcode
│   ├── rkvc_session_upscale   # 硬解 + 后处理上采样
│   ├── rkvc_yuv_upscale
│   ├── rkvc_info
│   ├── rkvc_model_id       # model.key 客户机器码采集器
│   └── rkvc_bench
├── lib/
│   ├── librkvc.so*
│   ├── libavcodec.so.62     # ffmpeg-rockchip 8.1 (H.264/HEVC/AV1 RKMPP + libsvtav1)
│   ├── libavformat.so.62
│   ├── libavutil.so.60
│   ├── libswscale.so.9
│   ├── libSvtAv1Enc.so.4    # QUALITY / AV1 编码（FFmpeg libsvtav1 依赖）
│   ├── librockchip_mpp.so.1
│   ├── librga.so            # RGA 用户态库（submodule）
│   ├── librknnrt.so         # RKNN NPU runtime（rkvc_sr）
│   └── ...
├── models/
│   ├── rkvc-sr/                 # Phase-RLFN runtime bundle（仅支持的 SoC；.rknn 默认已加密）
│   │   ├── phase_rlfn_sr_x3.rknn（默认经自研加密层加密，需 model.key 解密）
│   │   ├── sr_export_manifest.json
│   │   └── LICENSE.rknn-super-resolution-MIT / SOURCE.md
│   └── mlvc-s/                # MLVC bundle（默认仅轻量版；--mlvc-variants all 时另有 mlvc/）
│       ├── MLVCEncoder_<soc>.rknn / MLVCDecoder_<soc>.rknn（默认已加密）
│       ├── gaussian.bin / bitest.bin
│       ├── qp_patches/<soc>/
│       └── mlvc_rknn_export_manifest.json
├── include/rkvc/            # 头文件
├── share/pkgconfig/rkvc.pc
├── licenses/                # AGPLv3 与各第三方组件许可证（再分发义务）
│   ├── AGPL-3.0.txt         # 本项目（AGPLv3，见源码树 LICENSE）
│   ├── COPYING.LGPLv3       # ffmpeg-rockchip（LGPLv3 构建）
│   ├── LICENSE.md           # SVT-AV1（BSD-3-Clause Clear）
│   ├── PATENTS.md           # SVT-AV1（AOM 专利许可 1.0，随 BSD-3 一并适用）
│   ├── COPYING              # librga（Apache-2.0）
│   ├── LICENSE              # libsodium（ISC）
│   ├── mpp-LICENSES/        # rockchip-mpp（Apache-2.0 / MIT）
│   └── ffmpeg-modifications/ # 对 ffmpeg-rockchip 的补丁 + 对应源码说明（LGPLv3 §4）
├── examples/                # 示例源码与二进制
├── test.sh                  # 一键自测
├── network-e2e-test.sh      # 冒烟测试
├── portable-test-helpers.sh
├── README.md / USAGE.md / DEVELOPMENT.md / EXAMPLES.md
```

### 使用

```bash
tar xzf rkvc-*-linux-<platform>-portable.tar.gz
cd rkvc-*-linux-<platform>-portable

./test.sh
./network-e2e-test.sh

./bin/rkvc_info -j
./bin/rkvc_transcode -i in.mp4 -o out.mp4 -p balanced

# AI 超分（需目标机 NPU 驱动）
./bin/rkvc_session_upscale -i in.mp4 -o out.nv12 \
  --width 1920 --height 1080 --enc-scale-denom 3 \
  --post-upscale rkvc_sr \
  --rkvc-sr-model models/rkvc-sr/phase_rlfn_sr_x3.rknn

# 二次开发
gcc -o myapp myapp.c -Iinclude -Llib -lrkvc
LD_LIBRARY_PATH=lib ./myapp
```

包内工具已设置 RPATH，优先加载包内 `lib/`；二次开发程序可通过 `LD_LIBRARY_PATH=lib` 运行。

### 可复现性

所有二进制从源码 / 子模块构建：

- **rockchip-mpp**: `third_party/mpp/` 子模块
- **ffmpeg-rockchip**: `third_party/ffmpeg-rockchip/`，含 AV1 硬解
- **SVT-AV1**: `third_party/SVT-AV1/` 子模块
- **librga**: `third_party/librga/` 子模块（预编译用户态库）
- **Phase-RLFN**: manifest 固定 [rknn-super-resolution](https://github.com/Puiching-Memory/rknn-super-resolution) commit 与 checkpoint SHA-256
- **rkvc**: 项目源码

包内自测确认关键库解析到包内 `lib/`，避免串入系统旧版本。

**系统依赖（不进包）**：NPU 驱动/固件不随包分发；`librga`、`librknnrt`、Phase-RLFN bundle（`models/rkvc-sr/`）及 MLVC bundle（`models/mlvc/`、`models/mlvc-s/`）在启用对应功能时随包携带。目标机仍需 `/dev/rga` 等设备节点。

### 模型与 NPU 运行时来源声明

- **Phase-RLFN bundle（`models/rkvc-sr/`）**：来自 [Puiching-Memory/rknn-super-resolution](https://github.com/Puiching-Memory/rknn-super-resolution) 的单输入开源 core；生产缓存含 ONNX，portable runtime bundle 只含明文/可选加密 RKNN、SHA-256 manifest、MIT LICENSE 与源码 commit。旧 RGB 和 codec-aware 双输入模型不兼容。生成流程见 [sr-model-yuv-spec.md](sr-model-yuv-spec.md)。
- **MLVC bundle（`models/mlvc/` 与 `models/mlvc-s/`）**：由 Microsoft MLVC 官方 ONNX 经 `tools/mlvc/export_rknn.py` 转换生成的神经网络编解码模型，各变体独立持有 RKNN（`MLVC{Encoder,Decoder}_<soc>.rknn`）、PMF（`gaussian.bin`/`bitest.bin`）、QP 补丁与导出 manifest，不能跨变体混用。生成流程见 [mlvc-rknn-export.md](mlvc-rknn-export.md)；随 `librknnrt` 一并打进可移植包。
- **`librknnrt.so`**：Rockchip RKNN NPU 运行时，为 Rockchip 官方 SDK 提供的**专有二进制**，仅授权在 Rockchip 硬件上使用；再分发须遵守 Rockchip SDK 许可条款。构建时由 `scripts/install-rknnrt.sh` 从 [rknn-toolkit2](https://github.com/airockchip/rknn-toolkit2) 下载到 `.build/deps/rknn-install/`（默认 tag `v2.3.2`，与 `tools/` 的 rknn-toolkit2 对齐），再打进可移植包。项目不修改该库，仅动态链接。商业分发前请向 Rockchip 确认条款。

## DEB 包

```bash
source scripts/build-common.sh && rkvc_limit_build_jobs
cmake -B .build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C .build/release -j"$BUILD_JOBS" package
sudo dpkg -i .build/release/packages/rkvc_*_arm64.deb
```

> **说明**：DEB 包依赖系统上的 ffmpeg-rockchip 和 librockchip-mpp（与可移植包的自包含策略不同）。

## CPack TGZ

开发者 SDK 包（不含 ffmpeg 依赖）：

```bash
source scripts/build-common.sh && rkvc_limit_build_jobs
ninja -C .build/release -j"$BUILD_JOBS" package
# 产物: .build/release/packages/rkvc-*-Linux.tar.gz
```

## 打包脚本

| 脚本                                    | 用途                                        |
| --------------------------------------- | ------------------------------------------- |
| `scripts/package-portable.sh`           | 从源码构建可移植包（默认本机 SoC）         |
| `scripts/package-portable.sh --platforms rk3576,rk3588,rv1126b` | 多目标板，每平台一个包 |
| `scripts/package-portable.sh --mlvc-variants all` | 同时携带 mlvc 与 mlvc-s（默认仅 mlvc-s） |
| `scripts/package-portable.sh --no-test` | 跳过打包后的自动包内自测（默认自动跑） |
| `scripts/package-portable.sh --clean`   | 清理重建                                    |
| `scripts/package-portable.sh --license` | 强制授权版（运行时校验 license + rkvc_lic） |
| `scripts/build-models.sh --platform <soc>` | 单平台模型生产（打包时自动调用，也可单独运行） |
| `scripts/test-portable.sh <dir>`        | 测试可移植包（100+ 项，随平台硬件探测有增减）            |
| `scripts/build-svt.sh`                  | 构建 SVT-AV1                                |
| `scripts/rebuild-ffmpeg-rkmpp.sh`       | 重建 ffmpeg-rockchip                        |
| `<package>/test.sh`                     | 包内一键自测                                |
| `<package>/network-e2e-test.sh`         | 冒烟（`example_net_loopback` UDP/RTP 回环） |

### 构建缓存语义（重复打包免重编）

各阶段带“已构建跳过”检查，重复打包只重新组装：
- **MPP / SVT-AV1 / librga / librknnrt**：产物存在且有完成标记（`.rkvc-complete`，防磁盘满等中断的半成品）则跳过。
- **ffmpeg**：`rebuild-ffmpeg-rkmpp.sh` 在 `$FFMPEG_PREFIX/.rkvc-ffmpeg.stamp` 写入指纹（子模块 commit + 补丁哈希 + configure 选项 + 依赖前缀），命中则跳过；改选项/换补丁/切子模块自动重建。
- **rkvc**：`.build/portable`（`--license` 用平行目录 `.build/portable-licensed`，交替打包不互相触发重编）+ Ninja 增量。
- **模型**：`.build/models/<platform>/` 暂存幂等缓存。
- 从没有 `.rkvc-complete` 标记的旧版构建目录升级时，首次打包会重建 MPP / SVT-AV1；后续重复打包恢复增量跳过。
- 升级 MPP/RGA/SVT/ffmpeg 依赖后建议 `--clean` 全量重建；指纹不覆盖依赖库自身的版本变化。

## 发布文档模板

可移植包附带的用户文档源文件位于 `docs/release/`，打包时复制到包根目录。完整 API 参考在源码树 [docs/api.md](api.md)（不随可移植包分发，二次开发可查阅头文件 `include/rkvc/*.h`）。
