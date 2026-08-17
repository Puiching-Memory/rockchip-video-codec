# 打包与分发

## 可移植包 (推荐)

从源码构建，核心运行库随包携带，解压即用：

```bash
git submodule update --init --depth 1

./scripts/package-portable.sh
# 编译树: .build/portable/（preset portable）
# 成品包: .build/dist/rkvc-<version>-linux-<arch>-portable/

# 包名由 CMake project(VERSION) 生成
source scripts/build-common.sh
./scripts/test-portable.sh ".build/dist/$(rkvc_portable_pkg_dir)"
```

产物：`rkvc-*-linux-aarch64-portable.tar.gz`（约 7–8 MB，含 `librga` + `librknnrt` + `models/`）

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
# 成品包: .build/dist/rkvc-<version>-linux-<arch>-portable-licensed/
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
rkvc-*-linux-aarch64-portable/
├── bin/
│   ├── rkvc_encode
│   ├── rkvc_decode
│   ├── rkvc_transcode
│   ├── rkvc_session_upscale   # 硬解 + 后处理上采样
│   ├── rkvc_yuv_upscale
│   ├── rkvc_info
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
│   └── rkvc_sr_x3.crypt.rknn  # 约定 3× AI 超分模型（随 librknnrt 一并打包）
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
tar xzf rkvc-*-linux-aarch64-portable.tar.gz
cd rkvc-*-linux-aarch64-portable

./test.sh
./network-e2e-test.sh

./bin/rkvc_info -j
./bin/rkvc_transcode -i in.mp4 -o out.mp4 -p balanced

# AI 超分（需目标机 NPU 驱动）
./bin/rkvc_session_upscale -i in.mp4 -o out.nv12 \
  --width 1920 --height 1080 --enc-scale-denom 3 \
  --post-upscale rkvc_sr --rkvc-sr-model models/rkvc_sr_x3.crypt.rknn

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
- **rkvc**: 项目源码

包内自测确认关键库解析到包内 `lib/`，避免串入系统旧版本。

**系统依赖（不进包）**：NPU 驱动/固件不随包分发；`librga`、`librknnrt` 与约定模型 `models/rkvc_sr_x3.crypt.rknn` 在启用对应功能时随包携带。目标机仍需 `/dev/rga` 等设备节点。

### 模型与 NPU 运行时来源声明

- **`models/rkvc_sr_x3.crypt.rknn`**：项目自训练的 3× RGB 域超分模型（训练说明见 [sr-model-yuv-spec.md](sr-model-yuv-spec.md)），非第三方开源模型衍生，版权归本项目作者。文件以 `.crypt` 加密存储；明文模型与训练细节仅在商业授权范围内提供（见下「双许可」）。
- **`librknnrt.so`**：Rockchip RKNN NPU 运行时，为 Rockchip 官方 SDK 提供的**专有二进制**，仅授权在 Rockchip 硬件（RK3588 等）上使用；再分发须遵守 Rockchip SDK 许可条款。项目不修改该库，仅动态链接。商业分发前请向 Rockchip 确认条款。

## DEB 包

```bash
cmake -B .build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C .build/release -j4 package
sudo dpkg -i .build/release/packages/rkvc_*_arm64.deb
```

> **说明**：DEB 包依赖系统上的 ffmpeg-rockchip 和 librockchip-mpp（与可移植包的自包含策略不同）。

## CPack TGZ

开发者 SDK 包（不含 ffmpeg 依赖）：

```bash
ninja -C .build/release -j4 package
# 产物: .build/release/packages/rkvc-*-Linux.tar.gz
```

## 打包脚本

| 脚本                                    | 用途                                        |
| --------------------------------------- | ------------------------------------------- |
| `scripts/package-portable.sh`           | 从源码构建可移植包                          |
| `scripts/package-portable.sh --clean`   | 清理重建                                    |
| `scripts/package-portable.sh --license` | 强制授权版（运行时校验 license + rkvc_lic） |
| `scripts/test-portable.sh <dir>`        | 测试可移植包（99 项）                       |
| `scripts/build-svt.sh`                  | 构建 SVT-AV1                                |
| `scripts/rebuild-ffmpeg-rkmpp.sh`       | 重建 ffmpeg-rockchip                        |
| `<package>/test.sh`                     | 包内一键自测                                |
| `<package>/network-e2e-test.sh`         | 冒烟（码流生成 + stream_device_pair）       |

## 发布文档模板

可移植包附带的用户文档源文件位于 `docs/release/`，打包时复制到包根目录。完整 API 参考在源码树 [docs/api.md](api.md)（不随可移植包分发，二次开发可查阅头文件 `include/rkvc/*.h`）。
