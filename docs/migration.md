# v1 → v2 迁移指南

rkvc **0.2.0** 为破坏性升级：v1 的 `encoder` / `decoder` / `stream` / `frame` / `scale` API 已移除，统一为 Session + Pipeline 架构。

## 概念映射

| v1 概念 | v2 概念 |
|---------|---------|
| `rkvc_encoder` / `rkvc_decoder` | `rkvc_session` + 管线模板 |
| `rkvc_frame` | `rkvc_buffer`（`RKVC_BUF_VIDEO`） |
| `rkvc_packet` | `rkvc_buffer`（`RKVC_BUF_BITSTREAM`） |
| `rkvc_stream` | Session 端口 `capture` / `output` |
| 手动选 HEVC 编码器 | `rkvc_policy` + Codec Router |
| `rkvc_scale_*` | `enc_scale_denom` + RGA 节点 + `post_upscale_algo` |

## 头文件变更

**已删除：**

- `rkvc/encoder.h`、`decoder.h`、`stream.h`、`frame.h`、`scale.h`

**新增：**

- `rkvc/buffer.h`、`pipeline.h`、`policy.h`、`port.h`、`session.h`

## 代码迁移

### 文件编码（v0.1.x）

```c
// v0.1.x
rkvc_encoder_config cfg = rkvc_encoder_config_defaults();
cfg.width = 1920; cfg.height = 1080;
rkvc_encoder *enc;
rkvc_encoder_open_file(&enc, &cfg, "out.h265");
rkvc_encoder_send_frame(enc, frame);
rkvc_encoder_close(enc);
```

```c
// v0.2.0
rkvc_pipeline_desc d;
rkvc_pipeline_from_template(RKVC_TEMPLATE_FILE_ENCODE, &d);
d.input_path  = "raw.nv12";
d.output_path   = "out.mp4";
d.policy        = RKVC_POLICY_REALTIME;
d.width = 1920; d.height = 1080;

rkvc_session *s;
rkvc_session_create(&d, &s);
rkvc_session_run_file(s);
rkvc_session_destroy(s);
```

### 文件转码

```c
rkvc_pipeline_desc d;
rkvc_pipeline_from_template(RKVC_TEMPLATE_FILE_TRANSCODE, &d);
d.input_path  = "in.mp4";
d.output_path = "out.mp4";
d.policy      = RKVC_POLICY_BALANCED;

rkvc_session *s;
rkvc_session_create(&d, &s);
rkvc_session_run_file(s);
rkvc_session_destroy(s);
```

### 流式 push/pull（v0.1.x stream API）

```c
// v0.2.0 — 端口模式
rkvc_session_start(s);
rkvc_port *cap = rkvc_session_port(s, "capture");
rkvc_port *out = rkvc_session_port(s, "output");

rkvc_buffer *frame = NULL;
rkvc_buffer_alloc_video_host(&frame, w, h, RKVC_PIX_FMT_NV12);
rkvc_port_push(cap, frame);
rkvc_buffer_unref(frame);

rkvc_buffer *pkt = NULL;
if (rkvc_port_pull(out, &pkt, 100) == RKVC_OK) {
    rkvc_buffer_bitstream_view view;
    rkvc_buffer_get_bitstream(pkt, &view);
    // 使用 view.data / view.size
    rkvc_buffer_unref(pkt);
}
```

### 帧分配

```c
// v0.1.x
rkvc_frame *f;
rkvc_frame_alloc(&f, 1920, 1080, RKVC_PIX_FMT_NV12);
rkvc_frame_get_data(f, planes, strides);

// v0.2.0
rkvc_buffer *buf;
rkvc_buffer_alloc_video_host(&buf, 1920, 1080, RKVC_PIX_FMT_NV12);
rkvc_buffer_get_video_planes(buf, planes, strides);
```

## CLI 迁移

| v0.1.x | v0.2.0 |
|--------|--------|
| `rkvc_encode --testsrc -o test.h265` | `./example_encode_file -o test.mp4` |
| `rkvc_encode -i raw.nv12 -o out.h265` | `rkvc_encode -i raw.nv12 -o out.mp4 -p realtime` |
| `rkvc_encode --stdin/--stdout` | 已移除；使用 Session API 或示例程序 |
| `rkvc_decode --stdin/--stdout` | 已移除 |
| （无） | `rkvc_transcode -i in.mp4 -o out.mp4 -p balanced` |

## 能力查询 JSON 字段

| v0.1.x | v0.2.0 |
|--------|--------|
| `rkmpp_enc` / `rkmpp_dec` | `h264_enc`、`hevc_enc`、`av1_enc`、`h264_dec`、`hevc_dec`、`av1_dec` |

## 构建依赖新增

v2 需要额外构建：

```bash
git submodule update --init --depth 1 third_party/SVT-AV1
./scripts/build-svt.sh
./scripts/rebuild-ffmpeg-rkmpp.sh   # 含 H.264/HEVC/AV1 RKMPP
```

## 已知行为差异

- 默认输出容器为 **MP4**，不再默认裸 `.h265` 流
- `rkvc_encode` **仅接受原始 NV12 文件**，不接受压缩码流
- `QUALITY` 策略走 SVT-AV1 软件编码，需 `libSvtAv1Enc.so`
- `LIVE_CAPTURE` 模板与 V4L2 采集尚未接入，`stream_device_pair` 为占位
- 网络 UDP/RTP 回环测试已改为 v2 冒烟模式，完整回环待 LiveCapture 接入

## 可移植包

包名由 `rkvc-0.1.x-linux-aarch64-portable` 更新为 **`rkvc-0.2.1-linux-aarch64-portable`**（0.2.0 起 v2 API）。

相较 v1 包新增：

- `rkvc_transcode`、`rkvc_session_upscale`、`rkvc_yuv_upscale`
- `libSvtAv1Enc.so.4`（QUALITY / AV1 路线）
- 自测由 92 项增至 **99 项**（三策略 bench 严格匹配 + 后处理上采样）
