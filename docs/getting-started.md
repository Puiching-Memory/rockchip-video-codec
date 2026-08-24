# 快速开始

## 环境要求

| 组件   | 要求                                                                                          |
| ------ | --------------------------------------------------------------------------------------------- |
| SoC    | Rockchip RK3588 / RK3588S                                                                     |
| 内核   | Rockchip BSP 5.10 或 6.1                                                                      |
| 构建   | CMake >= 3.16（裸 `cmake -B`）；**CMake Presets 需 >= 3.21**、Ninja（推荐）、GCC/Clang（C11） |
| 系统包 | libdrm-dev、patchelf（可移植包打包）                                                          |
| 测试   | CMocka（可选，`RKVC_BUILD_TESTS=ON`）                                                         |

> **说明**：ffmpeg-rockchip、rockchip-mpp、SVT-AV1、librga 均从 `third_party/` 子模块获取（浅克隆）。`librga` 经 `./scripts/install-librga.sh` 安装到 `.build/deps/librga-install/`。`librknnrt` 经 `./scripts/install-rknnrt.sh` 从 [rknn-toolkit2](https://github.com/airockchip/rknn-toolkit2) 下载到 `.build/deps/rknn-install/`。两者都**不再依赖**开发板系统预装。可移植包携带 `librga` 与 `librknnrt`；NPU 驱动/固件仍需目标机提供。

## 设备权限

运行时会检测 MPP 服务、DMA heap、RGA、DRM 设备权限；不足时返回 `RKVC_ERR_PERMISSION`。

```bash
sudo chmod 666 /dev/mpp_service /dev/dma_heap/* /dev/rga
sudo chmod 666 /dev/dri/*
```

生产环境建议通过 udev 规则永久配置（见 [交付文档](delivery.md#设备权限)）。

## 获取源码与初始化子模块

```bash
git clone --recursive --shallow-submodules <repo-url> rockchip-video-codec
cd rockchip-video-codec

# 若未 --recursive（.gitmodules 已设 shallow = true）
git submodule update --init --depth 1
```

## 构建依赖

```bash
# SVT-AV1（QUALITY 策略需要）
./scripts/build-svt.sh

# librga（airockchip 预编译库 → .build/deps/librga-install）
./scripts/install-librga.sh

# librknnrt（从 airockchip/rknn-toolkit2 下载 → .build/deps/rknn-install；
# 不依赖板卡系统预装，可移植包会打进这份 .so）
./scripts/install-rknnrt.sh

# ffmpeg-rockchip：H.264/HEVC/AV1 RKMPP 编解码（自动使用上述 librga；
# 并应用 patches/ffmpeg-rockchip/*.patch）
./scripts/rebuild-ffmpeg-rkmpp.sh
```

## Python 工具环境

绘图、bench 脚本、MLVC ONNX/RKNN 导出共用仓库根目录 **一个** `.venv`。版本钉在 `pyproject.toml`，由 `uv.lock` 锁定传递依赖。

```bash
uv sync
.venv/bin/python tools/mlvc/export_rknn.py --help
```

不要在 `tools/` 或 `.build/deps/mlvc/` 再创建虚拟环境。

## 编译 rkvc

构建目录约定见 [build-layout.md](build-layout.md)（全部在 `.build/`，日常用 `.build/release/`）。

```bash
# CMake Presets（推荐，固定 -j6）→ 产物在 .build/release/
cmake --preset default
cmake --build --preset default

# Debug → .build/debug/
cmake --preset debug && cmake --build --preset debug

# 或手动（仍应使用约定目录名；CMake < 3.21 无 preset）
source scripts/build-common.sh && rkvc_limit_build_jobs  # 默认 round(nproc×80%)
cmake -B .build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C .build/release -j"$BUILD_JOBS"
```

默认 preset 构建目标：`rkvc_shared`、`rkvc_static`、`rkvc_encode`、`rkvc_decode`、`rkvc_transcode`、`rkvc_info`、`rkvc_bench`、`rkvc_session_upscale`、`rkvc_yuv_upscale`。

## 验证安装

```bash
./.build/release/rkvc_info -j
./.build/release/example_encode_file -o /tmp/bench_in.mp4 -s 640x480 -n 30
./.build/release/rkvc_bench -i /tmp/bench_in.mp4
```

## 快速使用

### 硬件能力查询

```bash
./.build/release/rkvc_info          # 文本
./.build/release/rkvc_info -j       # JSON
```

### 文件编码（需原始 NV12 输入）

```bash
# 使用示例程序生成测试图案并编码
./.build/release/example_encode_file -o test.mp4 -s 1920x1080 -n 100

# CLI：自备 NV12 文件
./.build/release/rkvc_encode -i raw.nv12 -o out.mp4 -s 1920x1080 -p realtime
```

### 文件解码

```bash
./.build/release/rkvc_decode -i out.mp4 -o decoded.nv12
```

### 转码

```bash
./.build/release/rkvc_transcode -i in.mp4 -o out.mp4 -p balanced -b 4000000
```

## 运行测试

```bash
cmake --preset tests
cmake --build --preset tests
ctest --preset tests -j1 --output-on-failure

# RK3588 硬件集成（每用例独立进程，串行）
export RKVC_RUN_HARDWARE_TESTS=1
ctest --test-dir .build/tests -j1 -R 'test_session_' --output-on-failure
```

详见 [测试](testing.md)。

## 系统安装

```bash
cmake --install .build/release --prefix /usr/local
gcc -o myapp myapp.c $(pkg-config --cflags --libs rkvc)
```

## RD 基准测试

```bash
./tools/bench/run_rd_benchmark.sh /path/to/1080p.mp4
```

详见 [基准测试](benchmark.md) 与 [tools/bench/README.md](../tools/bench/README.md)。

## MLVC 模型导出

现网推理按变体使用自包含 bundle：标准版在 `models/mlvc/`，轻量版在 `models/mlvc-s/`。每个目录都独立包含 `MLVCEncoder_*.rknn`、`MLVCDecoder_*.rknn`、`gaussian.bin`、`bitest.bin`、`qp_patches/` 与导出 manifest，不能跨目录混用。从 Microsoft MLVC 官方 ONNX 生成这些文件见 [MLVC RKNN 导出](mlvc-rknn-export.md)。

## 二次开发

- 完整 API：[api.md](api.md)（与 `include/rkvc/*.h` Doxygen 注释一致）
- 集成示例：[release/DEVELOPMENT.md](release/DEVELOPMENT.md)
- 调试日志：`rkvc_set_log_level(AV_LOG_DEBUG)` 或 `export RKVC_LOG_LEVEL=debug`
