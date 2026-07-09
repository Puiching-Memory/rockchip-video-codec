# 使用指南

## CLI 工具

### rkvc_info

查询多 codec 硬件能力。

```bash
rkvc_info              # 文本
rkvc_info -j           # JSON
rkvc_info -v           # 版本号
```

JSON 输出字段：`h264_enc`、`hevc_enc`、`av1_enc`、`h264_dec`、`hevc_dec`、`av1_dec`、`dma_heap`、`rga`、`max_width`、`max_height`。

### rkvc_encode

原始 NV12 文件 → 编码容器（默认 MP4）。

```bash
rkvc_encode -i raw.nv12 -o out.mp4 -s 1920x1080 \
  -p realtime|balanced|quality \
  -r 30 -b 4000000 \
  --rc-mode cbr|vbr|cqp --qp 26 \
  --enc-scale-denom 2 \
  --svt-lp N --svt-rtc
```

**注意**：

- `-i` 必须为原始 NV12 文件，不接受 .mp4 / .h265 等压缩文件
- `--enc-scale-denom N`：编码前 RGA 下采样（宽高各 /N）；**解码后上采样不在本工具内**
- 后处理上采样（RGA / `rkvc_sr`）请用 `rkvc_session_upscale` 或 `FILE_DECODE` + `post_upscale_algo`
- v2 已移除 `--testsrc`、`--stdin`、`--stdout`；测试图案请用 `example_encode_file`

### rkvc_decode

容器/码流 → 原始 NV12。

```bash
rkvc_decode -i input.mp4 -o decoded.nv12
```

### rkvc_transcode

容器 → 容器，Codec Router 按 policy 选编解码路线。

```bash
rkvc_transcode -i in.mp4 -o out.mp4 -p balanced -b 4000000
rkvc_transcode -i in.mp4 -o out_av1.mp4 -p quality -b 6000000 --svt-lp 4 --svt-rtc 0
```

### rkvc_session_upscale

硬解 + 后处理上采样（与 bench post-upscale 同路径）。

```bash
rkvc_session_upscale -i stream.mp4 -o out.nv12 \
  --width 1920 --height 1080 \
  --enc-scale-denom 2 --post-upscale bilinear

# RKNN 超分（需模型与 RKVC_ENABLE_RKNN 构建）
rkvc_session_upscale -i stream.mp4 -o out.nv12 \
  --width 1920 --height 1080 --enc-scale-denom 3 \
  --post-upscale rkvc_sr --rkvc-sr-model /path/to/model.rknn
```

### rkvc_yuv_upscale

YUV420P 文件批处理 RGA 缩放（底层 `rkvc_upscale_ctx_*` API，无需 Session）。

```bash
rkvc_yuv_upscale -i input.yuv -o output.yuv \
  --src-size 1920x1080 --dst-size 3840x2160 \
  --algo bilinear
```

支持算法：`nearest` / `bilinear` / `bicubic`（不含 `rkvc_sr`）。

### rkvc_bench

对比三档 policy 的 Session E2E 转码 fps（**须** `-i` 指定输入）。

```bash
rkvc_bench -i sample.mp4 -o /tmp/bench -s 1920x1080
```

输出示例：

```
rkvc v2 session E2E bench (input=sample.mp4)
  REALTIME (H.264): 36.2 fps
  BALANCED (HEVC):  27.1 fps
  QUALITY (AV1):    24.3 fps
```

## 典型工作流

### 生成测试视频

```bash
./examples/bin/example_encode_file -o test.mp4 -s 1920x1080 -n 300
```

### 解码预览

```bash
rkvc_decode -i test.mp4 -o decoded.nv12
ffplay -f rawvideo -pixel_format nv12 -video_size 1920x1080 decoded.nv12
```

### 策略转码

```bash
# 低延迟实时监控
rkvc_transcode -i input.mp4 -o realtime.mp4 -p realtime

# 均衡画质/码率
rkvc_transcode -i input.mp4 -o balanced.mp4 -p balanced -b 4000000

# 高画质 AV1
rkvc_transcode -i input.mp4 -o quality.mp4 -p quality -b 6000000
```

### 下采样编码 + 上采样还原

编码侧只负责低分辨率入库；还原在解码/后处理工具中完成：

```bash
# 1) 全分辨率 NV12 → 1/2 分辨率编码（仅下采样）
rkvc_encode -i raw.nv12 -o half_enc.mp4 -s 1920x1080 \
  --enc-scale-denom 2 -p quality

# 2) 硬解 + RGA / AI 上采样还原到显示分辨率
rkvc_session_upscale -i half_enc.mp4 -o restored.nv12 \
  --width 1920 --height 1080 \
  --enc-scale-denom 2 --post-upscale bilinear
```

## 环境变量

| 变量 | 说明 |
|------|------|
| `LD_LIBRARY_PATH` | 二次开发时指向包内 `lib/` |
| `PKG_CONFIG_PATH` | 指向 `share/pkgconfig/` |
| `RKVC_LOG_LEVEL` | 调试日志（`debug`）；等效于代码中 `rkvc_set_log_level(AV_LOG_DEBUG)` |

程序内也可调用 `rkvc_set_log_level()` / `rkvc_get_log_level()`（级别与 FFmpeg `AV_LOG_*` 一致）。

## 错误处理

所有 API 返回 `rkvc_err` 枚举。常见错误：

| 错误码 | 含义 | 处理 |
|--------|------|------|
| `RKVC_ERR_PERMISSION` | 设备权限不足 | 检查 `/dev/mpp_service` 等 |
| `RKVC_ERR_FORMAT` | 输入格式不匹配 | 编码用 NV12；压缩文件用 decode/transcode |
| `RKVC_ERR_HW` | 硬件初始化失败 | `rkvc_info -j` 诊断 |
| `RKVC_ERR_AGAIN` | 缓冲区满/需更多输入 | 流式模式重试或 drain |

## 从 v0.1.x 升级

v0.1.x 的 `--testsrc`、`--stdin`、`--stdout` 管道模式已移除。详见项目文档 `migration.md`。
