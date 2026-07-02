# rkvc — RK3588 多码率视频编解码库

高性能硬件视频编解码库，专为 RK3588 平台优化。支持 H.264 / HEVC / AV1 三族编解码，通过 Codec Router 按场景自动选型。

**版本**: 0.2.1

## 功能特性

- **多码率策略** — REALTIME (H.264) / BALANCED (HEVC) / QUALITY (AV1)
- **硬件加速** — RKMPP 硬编硬解，支持 8K
- **SVT-AV1** — QUALITY 策略软件编码 + av1_rkmpp 硬解
- **Session API** — 统一文件/流式编解码接口
- **RGA 缩放** — 硬件下采样 + 传统/AI 上采样后处理（`rkvc_sr`）

## 性能 (1080p E2E)

| policy | E2E fps |
|--------|---------|
| REALTIME (H.264) | ~36 |
| BALANCED (HEVC) | ~27 |
| QUALITY (AV1) | ~24 |

## 环境要求

- Rockchip RK3588 平台
- Linux 内核 5.10 或更高版本

## 设备权限

运行前需要设置设备权限。权限不足时程序返回 `RKVC_ERR_PERMISSION`。

## 快速使用

### 一键自测

```bash
./test.sh
```

自测 99 项：二进制完整性、RPATH、`rkvc_info` JSON、NV12 编码→解码→转码、`rkvc_bench` 三策略短测、`rkvc_session_upscale` 后处理上采样、pkg-config 编译、负向包检查。

### 网络冒烟测试

```bash
./network-e2e-test.sh
```

v2 版本生成短测试码流并验证 `stream_device_pair` 占位；完整 UDP/RTP 回环待 LiveCapture 模板接入。

### 硬件能力查询

```bash
./bin/rkvc_info -j
```

### 转码示例

```bash
./bin/rkvc_transcode -i in.mp4 -o out.mp4 -p balanced
```

### 编码示例（需原始 NV12）

```bash
./examples/bin/example_encode_file -o test.mp4 -s 1920x1080 -n 100
```

## 示例程序

- `encode_file` — 测试图案编码
- `decode_file` — 文件解码
- `transcode` — 转码
- `stream_encode` / `stream_decode` / `stream_transcode` — 流式处理
- `latency_test` — 端到端延迟
- `psnr_test` — 编解码质量
- `decode_formats` — 多像素格式验证

详见 `EXAMPLES.md`。

## 开发集成

```bash
gcc -o myapp myapp.c $(pkg-config --cflags --libs rkvc)
```

```cmake
find_package(rkvc REQUIRED)
target_link_libraries(myapp PRIVATE rkvc::shared)
```

详见 `DEVELOPMENT.md`。

## 技术支持

如有问题或需要技术支持，请联系供应商。
