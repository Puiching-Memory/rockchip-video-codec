# 示例程序指南

10 个示例位于 `examples/` 目录，一例一概念，构建后在 `examples/bin/` 或 `.build/release/` 下运行。

## 基础层：跑通管线

### example_encode_file

合成 NV12 测试图案 → MP4，最小编码流程（模板 + `rkvc_session_run_file`）。

```bash
./examples/bin/example_encode_file -o output.mp4 -s 1920x1080 -n 300 -b 4000000
```

### example_decode_file

容器 → 原始 YUV，可选输出像素格式（硬解不出的格式由库内经 swscale 转换）。

```bash
./examples/bin/example_decode_file input.mp4 decoded.nv12
./examples/bin/example_decode_file input.mp4 decoded.p010 p010   # nv12|yuv420p|nv16|p010
```

### example_transcode

文件转码，Codec Router 按策略自动选型并打印选型结果。

```bash
./examples/bin/example_transcode input.mp4 output.mp4 [realtime|balanced|quality|offline]
```

## 应用层：核心业务场景

### example_live_capture

V4L2 采集编码最小示例；`mock` 为合成 NV12 源，无摄像头也能跑。

```bash
./examples/bin/example_live_capture                          # mock 90 帧
./examples/bin/example_live_capture /dev/video-camera0 out.mp4 300 1280x720
```

### example_adaptive_bitrate

带宽自适应控制环：后台线程按输出字节数估算实际码率，偏差超 15% 时经
`rkvc_session_set_bitrate` 步进调整，并 `rkvc_session_request_idr` 让解码端快速重同步。

```bash
./examples/bin/example_adaptive_bitrate -d mock -n 240 -b 1500000   # 采集源（默认 mock）
./examples/bin/example_adaptive_bitrate -i in.nv12 -s 640x480       # 文件源
```

### example_roi_encode

ROI 智能压缩（仅 MPP H.264/HEVC）：中心区域 `qp_offset=-6` 保清晰、
背景区 `qp_offset=+10` 省码、角区 `force_intra=1` 限制误码扩散。

```bash
./examples/bin/example_roi_encode out.mp4 90
```

### example_net_loopback

UDP/RTP 本机回环：`rkvc_net_send` / `rkvc_net_recv` 分片重组冒烟。

```bash
./examples/bin/example_net_loopback [udp|rtp] [port]
```

## 进阶层：高级 API

### example_stream_ports

命名端口流式消费：FILE_ENCODE 在后台线程跑，主线程并发从 `output` 端口
拉取码流包（`rkvc_buffer_get_bitstream` 读 pts/key_frame）另存 Annex-B，
演示边录边取流、`AGAIN` 超时与结束后排空队列。

```bash
./examples/bin/example_stream_ports out.h264
```

### example_live_transcode_ports

无 `input_path` 的实时全硬件转码：把 H.264/H.265 Annex-B access unit 推入
`capture`，并发从 `output` 拉取 H.264/H.265 编码包。示例用 libavformat 从
裸码流文件读取 packet；Monibuca 集成时替换为视频包回调即可。

```bash
./examples/bin/example_live_transcode_ports input.h265 out.h264 h264 1280 720 2000000

# 24h 单 Session soak（循环输入，输出丢弃）
./examples/bin/example_live_transcode_ports input.h265 /dev/null h264 1280 720 2000000 86400
```

### example_upscale_ctx

RGA 上采样两种 API 对比：一次性 `rkvc_upscale_nv12` vs 复用 DMA 缓冲的
`rkvc_upscale_ctx_*`（批处理省去每帧 import/release）。

```bash
./examples/bin/example_upscale_ctx 100
```

## 与 CLI 工具对比

| 场景            | 示例程序                  | CLI              |
| --------------- | ------------------------- | ---------------- |
| 测试图案编码    | `example_encode_file`     | `rkvc_encode`    |
| 文件解码        | `example_decode_file`     | `rkvc_decode`    |
| 文件转码        | `example_transcode`       | `rkvc_transcode` |
| 采集/ROI/自适应 | `example_live_capture` 等 | —                |
| E2E fps 对比    | —                         | `rkvc_bench`     |

## 二次开发参考

建议阅读顺序：

1. `encode_file.c` — 模板 + run_file 最小流程
2. `decode_file.c` — 解码与像素格式
3. `transcode.c` — Codec Router 选型
4. `live_capture.c` — V4L2 采集
5. `stream_ports.c` — 文件模板的命名端口旁路消费
6. `live_transcode_ports.c` — Annex-B 实时 push→pull 硬件转码
7. `adaptive_bitrate.c` — 运行中热切换控制环
8. `roi_encode.c` — ROI 区域编码
9. `upscale_ctx.c` — RGA 批量上采样
10. `net_loopback.c` — UDP/RTP 收发

完整 API 文档见项目 `docs/api.md` 或包内 `DEVELOPMENT.md`。
