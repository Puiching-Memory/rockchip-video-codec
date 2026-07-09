# 快速开始

## 环境要求

| 组件 | 要求 |
|------|------|
| SoC | Rockchip RK3588 / RK3588S |
| 内核 | Rockchip BSP 5.10 或 6.1 |
| 构建 | CMake >= 3.16（裸 `cmake -B`）；**CMake Presets 需 >= 3.21**、Ninja（推荐）、GCC/Clang（C11） |
| 系统包 | libdrm-dev、patchelf（可移植包打包） |
| 测试 | CMocka（可选，`RKVC_BUILD_TESTS=ON`） |

> **说明**：ffmpeg-rockchip、rockchip-mpp、SVT-AV1、librga 均从 `third_party/` 子模块获取（浅克隆）。`librga` 经 `./scripts/install-librga.sh` 安装到 `.build/deps/librga-install/`，**不再依赖**开发板系统预装。可移植包携带 `librga` 与 `librknnrt`；NPU 驱动/固件仍需目标机提供。

## 设备权限

运行时会检测 MPP 服务、DMA heap、RGA、DRM 设备权限；不足时返回 `RKVC_ERR_PERMISSION`。

```bash
sudo chmod 666 /dev/mpp_service /dev/dma_heap/* /dev/rga
sudo chmod 666 /dev/dri/*
```

生产环境建议通过 udev 规则永久配置（见 [交付文档](delivery.md#设备权限)）。

## 获取源码与初始化子模块

```bash
git clone --recursive --shallow-submodules <repo-url> rk3588-ai-video-codec
cd rk3588-ai-video-codec

# 若未 --recursive（.gitmodules 已设 shallow = true）
git submodule update --init --depth 1
```

## 构建依赖

```bash
# SVT-AV1（QUALITY 策略需要）
./scripts/build-svt.sh

# librga（airockchip 预编译库 → .build/deps/librga-install）
./scripts/install-librga.sh

# ffmpeg-rockchip：H.264/HEVC/AV1 RKMPP 编解码（自动使用上述 librga）
./scripts/rebuild-ffmpeg-rkmpp.sh
```

## 编译 rkvc

构建目录约定见 [build-layout.md](build-layout.md)（全部在 `.build/`，日常用 `.build/release/`）。

```bash
# CMake Presets（推荐，并行度默认 -j4）→ 产物在 .build/release/
cmake --preset default
cmake --build --preset default

# Debug → .build/debug/
cmake --preset debug && cmake --build --preset debug

# 或手动（仍应使用约定目录名；CMake < 3.21 无 preset）
cmake -B .build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C .build/release -j4
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
./scripts/run-bench.sh /path/to/1080p.mp4
```

详见 [基准测试](benchmark.md) 与 [bench/README.md](../bench/README.md)。

## 二次开发

- 完整 API：[api.md](api.md)（与 `include/rkvc/*.h` Doxygen 注释一致）
- 集成示例：[release/DEVELOPMENT.md](release/DEVELOPMENT.md)
- 调试日志：`rkvc_set_log_level(AV_LOG_DEBUG)` 或 `export RKVC_LOG_LEVEL=debug`
