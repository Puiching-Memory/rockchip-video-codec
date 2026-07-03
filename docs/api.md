# rkvc v2 API 参考

v2 以 **Session + Pipeline + Codec Router** 替代 v1 的 `encoder` / `decoder` / `stream` / `frame` 四套 API。

## 头文件

| 头文件 | 说明 |
|--------|------|
| `rkvc.h` | 主入口：版本、init、caps、输入探测 |
| `buffer.h` | `rkvc_buffer` 视频/码流统一缓冲 |
| `policy.h` | `rkvc_policy`、`rkvc_codec`、Codec Router |
| `pipeline.h` | `rkvc_pipeline_desc`、管线模板 |
| `session.h` | `rkvc_session` 生命周期与统计 |
| `port.h` | `rkvc_port_push` / `rkvc_port_pull` |

```c
#include "rkvc/rkvc.h"   // 包含以上全部头文件
```

## 全局初始化

```c
rkvc_err rkvc_init(void);       // 线程安全，可多次调用
void     rkvc_deinit(void);
const char *rkvc_version(void); // "0.2.1"
uint32_t    rkvc_version_number(void); // major<<16 | minor<<8 | patch
const char *rkvc_err_str(rkvc_err err);
```

## 能力与权限

```c
typedef struct {
    int has_h264_enc, has_hevc_enc, has_av1_enc;
    int has_h264_dec, has_hevc_dec, has_av1_dec;
    int has_dma_heap, has_rga;
    int max_width, max_height;
} rkvc_caps;

rkvc_err rkvc_query_caps(rkvc_caps *caps);
rkvc_err rkvc_check_hw_permissions(void);  // 权限不足 → RKVC_ERR_PERMISSION
```

`rkvc_info --json` 输出字段与 `rkvc_caps` 对应。

## 输入格式探测

编码 CLI 在打开原始 NV12 前探测文件头，压缩视频误作 raw 输入时返回 `RKVC_ERR_FORMAT`。

```c
typedef enum {
    RKVC_INPUT_UNKNOWN = 0,
    RKVC_INPUT_RAW_VIDEO,
    RKVC_INPUT_COMPRESSED_VIDEO,
} rkvc_input_format_probe;

rkvc_input_format_probe rkvc_probe_input_format(const uint8_t *data, size_t size);
```

## 错误码

| 错误码 | 值 | 含义 |
|--------|-----|------|
| `RKVC_OK` | 0 | 成功 |
| `RKVC_ERR_NOMEM` | -1 | 内存分配失败 |
| `RKVC_ERR_INVALID` | -2 | 参数无效 |
| `RKVC_ERR_NOT_FOUND` | -3 | 编解码器或设备未找到 |
| `RKVC_ERR_IO` | -4 | I/O 错误 |
| `RKVC_ERR_HW` | -5 | 硬件加速初始化失败 |
| `RKVC_ERR_EOF` | -6 | 流结束 |
| `RKVC_ERR_AGAIN` | -7 | 需要更多输入或输出缓冲区满 |
| `RKVC_ERR_MUX` | -8 | 封装器错误 |
| `RKVC_ERR_INTERNAL` | -9 | 内部 FFmpeg 错误 |
| `RKVC_ERR_PERMISSION` | -10 | 设备节点权限不足 |
| `RKVC_ERR_FORMAT` | -11 | 输入数据格式不匹配 |

## 像素格式

| 格式 | 枚举 | 说明 |
|------|------|------|
| NV12 | `RKVC_PIX_FMT_NV12` | 默认，VPU 原生 |
| YUV420P | `RKVC_PIX_FMT_YUV420P` | Planar 4:2:0 |
| NV16 | `RKVC_PIX_FMT_NV16` | 4:2:2 semi-planar |
| P010 | `RKVC_PIX_FMT_P010` | 10-bit 4:2:0 |

## Policy 与 Codec

```c
typedef enum {
    RKVC_POLICY_REALTIME = 0,  // H.264 RKMPP
    RKVC_POLICY_BALANCED,      // HEVC RKMPP（高帧率 1080p+ 回退 H.264）
    RKVC_POLICY_QUALITY,       // SVT-AV1 + av1_rkmpp
} rkvc_policy;

typedef enum {
    RKVC_CODEC_H264, RKVC_CODEC_HEVC, RKVC_CODEC_AV1, RKVC_CODEC_AUTO,
} rkvc_codec;
```

```c
typedef struct {
    rkvc_codec       codec;
    rkvc_enc_backend enc_backend;  // MPP 或 SVT
    rkvc_dec_backend dec_backend;
    const char      *enc_name;     // "h264_rkmpp" / "svt-av1" 等
    const char      *dec_name;
    int              svt_preset;
    const char      *reason;
} rkvc_route_plan;

rkvc_err rkvc_route_resolve(const rkvc_pipeline_desc *desc, rkvc_route_plan *plan);
```

## 管线描述

```c
typedef struct rkvc_pipeline_desc {
    rkvc_pipeline_template template_id;
    rkvc_policy            policy;
    rkvc_codec             codec;

    int            width, height;
    int            fps_num, fps_den;
    int64_t        bitrate;
    rkvc_pix_fmt   pixel_format;
    int            gop_size, b_frames;
    int            low_latency;
    int            queue_depth;       // 端口队列深度，默认 3
    rkvc_rc_mode   rc_mode;           // VBR / CBR / CQP
    int            qp_init;           // -1 表示编码器默认

    const char    *input_path;
    const char    *output_path;

    int            enc_scale_denom;           // 1=全分辨率编码
    rkvc_upscale_algo post_upscale_algo;      // 解码后上采样
    const char    *post_upscale_rkvc_model_path;

    int            svt_lp;                    // SVT lp，0=自动
    int            svt_rtc;                   // SVT 实时调优，0/1

    const char    *codec_opts;                // FFmpeg key=val:key2=val2，demux/dec/enc
} rkvc_pipeline_desc;
```

### 模板

| 模板 | 默认 policy | 说明 |
|------|-------------|------|
| `RKVC_TEMPLATE_FILE_ENCODE` | `REALTIME` | 原始 NV12 → 编码文件 |
| `RKVC_TEMPLATE_FILE_DECODE` | `BALANCED` | 容器 → 原始 NV12 |
| `RKVC_TEMPLATE_FILE_TRANSCODE` | `BALANCED` | 转码（Router 选 codec） |
| `RKVC_TEMPLATE_AV1_STORAGE` | `QUALITY` | 强制 AV1 SVT 存储档 |
| `RKVC_TEMPLATE_LIVE_CAPTURE` | `REALTIME` | 低延迟 H.264（V4L2 待接） |

```c
rkvc_pipeline_desc rkvc_pipeline_desc_defaults(void);
rkvc_err rkvc_pipeline_from_template(rkvc_pipeline_template tmpl,
                                     rkvc_pipeline_desc *desc);
```

## Session

```c
typedef struct {
    rkvc_route_plan route;
    int             running;
    uint64_t        frames_in, frames_out, frames_dropped;
    double          avg_fps;
} rkvc_session_stats;

rkvc_err rkvc_session_create(const rkvc_pipeline_desc *desc, rkvc_session **out);
rkvc_err rkvc_session_start(rkvc_session *session);
rkvc_err rkvc_session_stop(rkvc_session *session);
rkvc_err rkvc_session_get_route(const rkvc_session *session, rkvc_route_plan *plan);
rkvc_port *rkvc_session_port(rkvc_session *session, const char *name);
rkvc_err rkvc_session_get_stats(const rkvc_session *session, rkvc_session_stats *stats);
void     rkvc_session_destroy(rkvc_session *session);

/** 文件模板：阻塞跑完整条管线 */
rkvc_err rkvc_session_run_file(rkvc_session *session);
```

### 文件转码示例

```c
rkvc_init();

rkvc_pipeline_desc d;
rkvc_pipeline_from_template(RKVC_TEMPLATE_FILE_TRANSCODE, &d);
d.input_path  = "in.mp4";
d.output_path = "out.mp4";
d.policy      = RKVC_POLICY_BALANCED;
d.bitrate     = 4000000;

rkvc_session *s = NULL;
rkvc_session_create(&d, &s);
rkvc_session_run_file(s);
rkvc_session_destroy(s);

rkvc_deinit();
```

### 端口流式示例

```c
rkvc_pipeline_desc d;
rkvc_pipeline_from_template(RKVC_TEMPLATE_FILE_ENCODE, &d);
d.output_path = "out.mp4";
d.width = 1920; d.height = 1080;

rkvc_session *s;
rkvc_session_create(&d, &s);
rkvc_session_start(s);

rkvc_port *capture = rkvc_session_port(s, "capture");
for (int i = 0; i < 300; i++) {
    rkvc_buffer *buf = NULL;
    rkvc_buffer_alloc_video_host(&buf, 1920, 1080, RKVC_PIX_FMT_NV12);
    rkvc_buffer_set_pts(buf, i);
    // 填充像素 ...
    rkvc_port_push(capture, buf);
    rkvc_buffer_unref(buf);
}

rkvc_session_stop(s);
rkvc_session_destroy(s);
```

## Buffer

```c
typedef enum { RKVC_BUF_VIDEO, RKVC_BUF_BITSTREAM } rkvc_buffer_kind;
typedef enum { RKVC_MEM_HOST, RKVC_MEM_DMABUF } rkvc_mem_type;

rkvc_buffer *rkvc_buffer_ref(rkvc_buffer *buf);
void         rkvc_buffer_unref(rkvc_buffer *buf);

rkvc_err rkvc_buffer_alloc_video_host(rkvc_buffer **out,
                                      int w, int h, rkvc_pix_fmt fmt);
rkvc_err rkvc_buffer_alloc_bitstream(rkvc_buffer **out,
                                     const uint8_t *data, size_t size, int copy);
rkvc_err rkvc_buffer_get_video_planes(rkvc_buffer *buf,
                                      uint8_t *planes[4], int strides[4]);
rkvc_err rkvc_buffer_get_bitstream(const rkvc_buffer *buf,
                                   rkvc_buffer_bitstream_view *view);
rkvc_err rkvc_buffer_set_pts(rkvc_buffer *buf, int64_t pts);
```

## 上采样算法

```c
typedef enum {
    RKVC_UPSCALE_NONE, RKVC_UPSCALE_NEAREST, RKVC_UPSCALE_BILINEAR,
    RKVC_UPSCALE_BICUBIC, RKVC_UPSCALE_AI_SR,  // rkvc_sr（需 RKNN 模型路径）
} rkvc_upscale_algo;

int rkvc_upscale_algo_from_name(const char *name, rkvc_upscale_algo *out);
const char *rkvc_upscale_algo_name(rkvc_upscale_algo algo);
```

## CLI 工具

| 工具 | 用途 |
|------|------|
| `rkvc_encode` | 原始 NV12 → MP4（`-p realtime\|balanced\|quality`；`--svt-lp`/`--svt-rtc`） |
| `rkvc_decode` | 容器/码流 → 原始 NV12 |
| `rkvc_transcode` | 容器 → 容器，Router 选 codec（`--svt-lp`/`--svt-rtc`） |
| `rkvc_bench` | 三档 policy E2E fps 对比（**须** `-i INPUT.mp4`） |
| `rkvc_info` | 硬件能力查询（`-j` JSON） |
| `rkvc_session_upscale` | 硬解 + 后处理上采样（含 `rkvc_sr` + `--rkvc-sr-model`） |
| `rkvc_yuv_upscale` | YUV420p 批处理 RGA 缩放 |

```bash
rkvc_encode -i raw.nv12 -o out.mp4 -s 1920x1080 -p balanced \
  --rc-mode cbr -b 4000000 --enc-scale-denom 2 --post-upscale bilinear \
  --svt-lp 4 --svt-rtc 0

rkvc_transcode -i in.mp4 -o out.mp4 -p quality -b 6000000 --svt-lp 4

rkvc_session_upscale -i stream.mp4 -o out.nv12 --width 1920 --height 1080 \
  --enc-scale-denom 2 --post-upscale rkvc_sr --rkvc-sr-model model.rknn

rkvc_bench -i clip.mp4
rkvc_info -j
```

`rkvc_encode` 的 `--post-upscale` 仅支持 `nearest`/`bilinear`/`bicubic`；**AI 超分**请用 `rkvc_session_upscale` 或 Session 字段 `post_upscale_algo=RKVC_UPSCALE_AI_SR`。

## v1 → v2 迁移

详见 [migration.md](migration.md)。

| v1 | v2 |
|----|-----|
| `rkvc_encoder_send_frame` | `rkvc_port_push(capture, buf)` 或 `rkvc_session_run_file` |
| `rkvc_encoder_receive_packet` | `rkvc_port_pull(output, &buf, timeout)` |
| `rkvc_decoder_receive_frame` | 解码模板 + `port_pull("output")` |
| `rkvc_frame_alloc` | `rkvc_buffer_alloc_video_host` |
| `rkvc_stream_push/pull` | Session 端口队列 |
| `rkvc_encode --testsrc` | `example_encode_file` 或自备 NV12 输入 |
