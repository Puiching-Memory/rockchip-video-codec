# rkvc — RK3588 多码率视频编解码库

高性能硬件视频编解码库，专为 RK3588 平台优化。支持 H.264 / HEVC / AV1 三族编解码，通过 Codec Router 按场景自动选型。

**版本**: 运行 `./bin/rkvc_info -v` 查看

## 功能特性

- **多码率策略** — REALTIME (H.264) / BALANCED (HEVC) / QUALITY (AV1)
- **硬件加速** — RKMPP 硬编硬解，支持 8K
- **SVT-AV1** — QUALITY 策略软件编码 + av1_rkmpp 硬解
- **Session API** — 统一文件/流式编解码接口
- **实时硬件转码** — `LIVE_TRANSCODE`：Annex-B 端口输入，MPP 硬解+硬编，output 实时拉流
- **RGA / AI 上采样** — 解码路径后处理；`rkvc_sr` 使用包内单输入 Phase-RLFN bundle

性能数字见包内 `USAGE.md` 或源码树 `docs/benchmark.md`（1080p E2E：REALTIME ~36 / BALANCED ~27 / QUALITY ~24 fps）。

## 环境要求

- Rockchip RK3588 平台
- Linux 内核 5.10 或更高版本

## 设备权限

运行前需要设置设备权限。权限不足时程序返回 `RKVC_ERR_PERMISSION`。

## 模型授权（model.key）

包内 `models/` 下的 `.rknn`（AI 超分 `rkvc_sr` 与 MLVC 神经编解码）默认经加密层保护，
须放置**本机**专属的 `model.key` 才能加载；常规硬编解码（H.264 / HEVC / AV1）不受影响。

1. 向供应商提供本机机器码（由供应商提供的采集工具获取，绑定硬件指纹）；
2. 供应商签发与该机器码绑定的 `model.key`；
3. 放置到 `~/.config/rkvc/model.key`，或设置环境变量 `RKVC_MODEL_KEY_FILE=<路径>`。

错误语义：未放置 `model.key` → `RKVC_ERR_UNLICENSED`；机器码不符或文件被篡改 → `RKVC_ERR_LICENSE`。
`model.key` 不可跨机拷贝；放置后方可运行 `./test.sh`（否则 NPU 相关项会报未授权）。

## 快速使用

### 一键自测

```bash
./test.sh
```

自测 100+ 项：二进制完整性、RPATH、`rkvc_info` JSON、NV12 编码→解码→转码、`rkvc_bench` 三策略短测、`rkvc_session_upscale` 后处理上采样、pkg-config 编译、负向包检查。

### 网络冒烟测试

```bash
./network-e2e-test.sh
```

调用 `example_net_loopback` 做 UDP + RTP 本机回环冒烟（不含国标/WebRTC 信令）。

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
- `decode_file` — 文件解码（可选 NV12/YUV420P/NV16/P010 输出）
- `transcode` — 转码（Router 自动选型）
- `live_capture` — V4L2/mock 采集编码
- `adaptive_bitrate` — 带宽自适应控制环
- `roi_encode` — ROI 智能压缩
- `stream_ports` — 命名端口并发取流
- `live_transcode_ports` — H.264/H.265 Annex-B 实时 push→pull 硬件转码
- `upscale_ctx` — RGA 批量上采样
- `net_loopback` — UDP/RTP 回环

详见 `EXAMPLES.md`。

## 开发集成

```bash
gcc -o myapp myapp.c $(pkg-config --cflags --libs rkvc)
```

```cmake
find_package(rkvc REQUIRED)
target_link_libraries(myapp PRIVATE rkvc::shared)
```

详见 `DEVELOPMENT.md`（集成示例）与项目 `docs/api.md`（完整 API 参考）。

## 头文件

```
include/rkvc/
├── types.h      # 错误码、像素格式、码率控制、上采样枚举
├── rkvc.h       # 主入口（包含以下全部）
├── buffer.h     # rkvc_buffer
├── policy.h     # Codec Router
├── pipeline.h   # 管线模板与描述
├── session.h    # 会话生命周期
└── port.h       # push/pull 端口
```

## 技术支持

如有问题或需要技术支持，请联系供应商。
