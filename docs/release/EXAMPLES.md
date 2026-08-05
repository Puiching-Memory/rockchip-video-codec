# 示例程序指南

所有示例位于 `examples/` 目录，构建后在 `examples/bin/` 或 `.build/release/` 下运行。

## 文件编解码

### example_encode_file

内置测试图案生成，通过端口 push 编码为 MP4。

```bash
./examples/bin/example_encode_file -o output.mp4 -s 1920x1080 -n 300 -b 4000000
```

源码：`examples/encode_file.c` — 演示 `rkvc_session_port("capture")` + `rkvc_buffer_alloc_video_host`。

### example_decode_file

容器文件解码为原始 NV12。

```bash
./examples/bin/example_decode_file input.mp4 -o decoded.nv12
```

### example_decode_formats

同一码流分别以 NV12 / YUV420P / NV16 / P010 解码，从 `output` 端口校验首帧 `format`（无参数时自生成短 HEVC 夹具，需硬件）。

源码演示 `rkvc_buffer_get_video_info()` 读取解码帧格式。

```bash
./examples/bin/example_decode_formats
./examples/bin/example_decode_formats input.mp4
```

## 转码

### example_transcode

文件转码，使用 Codec Router。

```bash
./examples/bin/example_transcode input.mp4 output.mp4
```

## 流式处理

### example_stream_device_pair

双设备流式传输占位（V4L2 / LiveCapture 待接）。

```bash
./examples/bin/example_stream_device_pair
```

## 性能与质量

### example_latency_test

模拟摄像头端到端延迟（编码 → 解码）。

```bash
./examples/bin/example_latency_test -l
./examples/bin/example_latency_test -s 1280x720 -r 60 -n 600 -b 8000000 -l
```

选项：`-l` 低延迟、`-s` 分辨率、`-r` 帧率、`-n` 帧数、`-b` 码率。

### example_visual_compare

SDL2 原始/重建画面对比（可选构建，需 SDL2）。

```bash
./examples/bin/example_visual_compare -i input.mp4 -l
```

## 与 CLI 工具对比

| 场景         | 示例程序               | CLI                         |
| ------------ | ---------------------- | --------------------------- |
| 测试图案编码 | `example_encode_file`  | 需自备 NV12 + `rkvc_encode` |
| 文件解码     | `example_decode_file`  | `rkvc_decode`               |
| 文件转码     | `example_transcode`    | `rkvc_transcode`            |
| 延迟测试     | `example_latency_test` | —                           |
| E2E fps 对比 | —                      | `rkvc_bench`                |

## 二次开发参考

建议阅读顺序：

1. `encode_file.c` — Session + 端口 + Buffer 基础
2. `transcode.c` — 文件转码最简路径
3. `latency_test.c` — 性能测量
4. `decode_formats.c` — 多像素格式 + `rkvc_buffer_get_video_info`

完整 API 文档见项目 `docs/api.md` 或包内 `DEVELOPMENT.md`。
