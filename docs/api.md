# rkvc v2 API 参考

v2 以 **Session + Pipeline + Codec Router** 替代 v1 的 `encoder` / `decoder` / `stream` / `frame` 四套 API。

## 头文件

| 头文件       | 说明                                             |
| ------------ | ------------------------------------------------ |
| `types.h`    | 错误码、像素格式、码率控制、上采样枚举           |
| `rkvc.h`     | 主入口：版本、init、日志、caps、探测、RGA 上采样 |
| `buffer.h`   | `rkvc_buffer` 视频/码流统一缓冲                  |
| `policy.h`   | `rkvc_policy`、`rkvc_codec`、Codec Router        |
| `pipeline.h` | `rkvc_pipeline_desc`、管线模板                   |
| `session.h`  | `rkvc_session` 生命周期与统计                    |
| `port.h`     | `rkvc_port_push` / `rkvc_port_pull`              |

```c
#include "rkvc/rkvc.h"   // 包含以上全部头文件
```

也可按需单独包含子头文件。

---

## 全局初始化与版本

```c
rkvc_err rkvc_init(void);              // 线程安全，可多次调用（pthread_once）
void     rkvc_deinit(void);            // 不反初始化 FFmpeg 全局状态
const char  *rkvc_version(void);       // 与 CMake project(VERSION) 一致
uint32_t     rkvc_version_number(void); // major<<16 | minor<<8 | patch
const char  *rkvc_err_str(rkvc_err err);
```

- `rkvc_init()`：注册 FFmpeg 日志回调、探测硬件；`rkvc_session_create` 也会隐式调用。
- `rkvc_deinit()`：仅标记未初始化，避免多实例共享 FFmpeg 时崩溃。

---

## 日志

```c
void rkvc_set_log_level(int level);  // FFmpeg AV_LOG_*，如 AV_LOG_INFO、AV_LOG_DEBUG
int  rkvc_get_log_level(void);
```

同时作用于 `[rkvc]` 前缀日志与 FFmpeg `av_log`。

---

## 能力与权限

```c
typedef struct {
    int has_h264_enc, has_hevc_enc, has_av1_enc;   // has_av1_enc = SVT-AV1
    int has_h264_dec, has_hevc_dec, has_av1_dec;
    int has_dma_heap, has_rga, has_rknn;  // has_rknn：RKNN 已编译且 NPU 可访问
    int max_width, max_height;   // RK3588：7680×4320
} rkvc_caps;

rkvc_err rkvc_query_caps(rkvc_caps *caps);
rkvc_err rkvc_check_hw_permissions(void);  // → RKVC_ERR_PERMISSION
```

`rkvc_query_caps` 在设备权限不足时将对应 `has_*_enc/dec` 置 0。`has_rknn` 需 `RKVC_ENABLE_RKNN` 构建且 NPU 可访问（`/sys/kernel/debug/rknpu/version` 或 `/dev/dri/by-path/*npu-render*`）。`rkvc_info --json` 字段与此结构对应（含 `rknn`）。

`rkvc_check_hw_permissions` 检查 `/dev/mpp_service`、DMA heap（`/dev/dma_heap/system-uncached`）或 DRM/Ion 分配器。

---

## 输入格式探测

编码 CLI 在打开原始 NV12 前探测文件头；压缩视频误作 raw 输入时返回 `RKVC_ERR_FORMAT`。

```c
typedef enum {
    RKVC_INPUT_UNKNOWN = 0,          // 无法判定（原始 YUV 无魔数）
    RKVC_INPUT_RAW_VIDEO,            // 保留；当前实现不返回此值
    RKVC_INPUT_COMPRESSED_VIDEO,     // 检测到 MP4/MKV/Annex-B 等
} rkvc_input_format_probe;

rkvc_input_format_probe rkvc_probe_input_format(const uint8_t *data, size_t size);
```

建议传入 ≥ 12 字节文件头。原始 NV12 文件通常返回 `UNKNOWN`，需由路径/参数约定格式。

---

## 错误码

| 错误码                | 值  | 含义                        |
| --------------------- | --- | --------------------------- |
| `RKVC_OK`             | 0   | 成功                        |
| `RKVC_ERR_NOMEM`      | -1  | 内存分配失败                |
| `RKVC_ERR_INVALID`    | -2  | 参数无效                    |
| `RKVC_ERR_NOT_FOUND`  | -3  | 编解码器或设备未找到        |
| `RKVC_ERR_IO`         | -4  | I/O 错误                    |
| `RKVC_ERR_HW`         | -5  | 硬件加速初始化失败          |
| `RKVC_ERR_EOF`        | -6  | 流结束                      |
| `RKVC_ERR_AGAIN`      | -7  | 队列满/空、需重试（非致命） |
| `RKVC_ERR_MUX`        | -8  | 封装器错误                  |
| `RKVC_ERR_INTERNAL`   | -9  | 内部 FFmpeg 错误            |
| `RKVC_ERR_PERMISSION` | -10 | 设备节点权限不足            |
| `RKVC_ERR_FORMAT`     | -11 | 输入数据格式不匹配          |

---

## 像素格式

| 格式    | 枚举                   | 说明              |
| ------- | ---------------------- | ----------------- |
| NV12    | `RKVC_PIX_FMT_NV12`    | 默认，VPU 原生    |
| YUV420P | `RKVC_PIX_FMT_YUV420P` | Planar 4:2:0      |
| NV16    | `RKVC_PIX_FMT_NV16`    | 4:2:2 semi-planar |
| P010    | `RKVC_PIX_FMT_P010`    | 10-bit 4:2:0      |

---

## 码率控制

```c
typedef enum {
    RKVC_RC_VBR = 0,   // 可变码率
    RKVC_RC_CBR = 1,   // 恒定码率（pipeline 默认）
    RKVC_RC_CQP = 2,   // 固定 QP
} rkvc_rc_mode;
```

数值与 MPP / FFmpeg `rkmppenc` 的 `rc_mode` 一致。

---

## Policy、Codec 与 Router

```c
typedef enum {
    RKVC_POLICY_REALTIME = 0,  // H.264 RKMPP，≥30fps@1080p
    RKVC_POLICY_BALANCED,      // HEVC RKMPP（高帧率 1080p+ 回退 H.264）
    RKVC_POLICY_QUALITY,       // SVT-AV1 preset 11 + av1_rkmpp（近实时）
    RKVC_POLICY_OFFLINE,       // SVT-AV1 preset 4 + av1_rkmpp（非实时，≥1fps@1080p）
} rkvc_policy;

typedef enum {
    RKVC_CODEC_H264, RKVC_CODEC_HEVC, RKVC_CODEC_AV1, RKVC_CODEC_AUTO,
} rkvc_codec;

typedef enum {
    RKVC_ENC_BACKEND_NONE, RKVC_ENC_BACKEND_MPP, RKVC_ENC_BACKEND_SVT,
} rkvc_enc_backend;

typedef enum {
    RKVC_DEC_BACKEND_NONE, RKVC_DEC_BACKEND_MPP,
} rkvc_dec_backend;
```

```c
typedef struct {
    rkvc_codec       codec;
    rkvc_enc_backend enc_backend;
    rkvc_dec_backend dec_backend;
    const char      *enc_name;     // "h264_rkmpp" / "svt-av1" 等
    const char      *dec_name;
    int              svt_preset;   // SVT enc_mode（如 4–11）
    const char      *reason;       // 选型原因（静态字符串）
} rkvc_route_plan;

rkvc_err rkvc_route_resolve(const rkvc_pipeline_desc *desc, rkvc_route_plan *plan);

const char *rkvc_codec_name(rkvc_codec codec);    // "h264" / "hevc" / "av1" / "auto"
const char *rkvc_policy_name(rkvc_policy policy); // "realtime" / "balanced" / "quality" / "offline"
```

`desc->codec != RKVC_CODEC_AUTO` 时强制对应路线，忽略 policy 自动规则。

---

## 管线描述

```c
typedef enum {
    RKVC_TEMPLATE_FILE_ENCODE,
    RKVC_TEMPLATE_FILE_DECODE,
    RKVC_TEMPLATE_FILE_TRANSCODE,
    RKVC_TEMPLATE_LIVE_CAPTURE,
    RKVC_TEMPLATE_AV1_STORAGE,
} rkvc_pipeline_template;

typedef struct rkvc_pipeline_desc {
    rkvc_pipeline_template template_id;
    rkvc_policy            policy;
    rkvc_codec             codec;

    int            width, height;
    int            fps_num, fps_den;
    int64_t        bitrate;
    rkvc_pix_fmt   pixel_format;
    int            gop_size;
    int            low_latency;
    int            queue_depth;       // 端口队列深度，默认 3
    rkvc_rc_mode   rc_mode;
    int            qp_init;           // -1 表示编码器默认

    const char    *input_path;
    const char    *output_path;

    int            enc_scale_denom;           // 1=全分辨率编码
    rkvc_upscale_algo post_upscale_algo;
    const char    *post_upscale_rkvc_model_path;  // AI_SR 必填

    int            svt_lp;                    // 0=自动，1–6 手动
    int            svt_rtc;                   // 0/1

    const char    *codec_opts;                // FFmpeg key=val:key2=val2
} rkvc_pipeline_desc;
```

### 模板

| 模板                           | 默认 policy | 说明                                 |
| ------------------------------ | ----------- | ------------------------------------ |
| `RKVC_TEMPLATE_FILE_ENCODE`    | `REALTIME`  | 原始 NV12 → 编码文件                 |
| `RKVC_TEMPLATE_FILE_DECODE`    | `BALANCED`  | 容器 → 原始 NV12                     |
| `RKVC_TEMPLATE_FILE_TRANSCODE` | `BALANCED`  | 转码（Router 选 codec）              |
| `RKVC_TEMPLATE_AV1_STORAGE`    | `QUALITY`   | 强制 AV1 SVT 存储档                  |
| `RKVC_TEMPLATE_LIVE_CAPTURE`   | `REALTIME`  | 低延迟 H.264 + V4L2，`low_latency=1` |

### 默认值（`rkvc_pipeline_desc_defaults`）

| 字段              | 默认             |
| ----------------- | ---------------- |
| template          | `FILE_TRANSCODE` |
| policy            | `BALANCED`       |
| codec             | `AUTO`           |
| 分辨率            | 1920×1080@30     |
| bitrate           | 4_000_000        |
| pixel_format      | NV12             |
| gop_size          | 60               |
| queue_depth       | 3                |
| rc_mode           | CBR              |
| qp_init           | -1               |
| enc_scale_denom   | 1                |
| post_upscale_algo | NONE             |

```c
rkvc_pipeline_desc rkvc_pipeline_desc_defaults(void);
rkvc_err rkvc_pipeline_from_template(rkvc_pipeline_template tmpl,
                                     rkvc_pipeline_desc *desc);
```

---

## Session

```c
typedef struct {
    rkvc_route_plan route;
    int             running;
    uint64_t        frames_in, frames_out, frames_dropped;
    uint64_t        bytes_out;       // 输出累计字节数（码流/视频帧）
    double          avg_fps;
    double          decode_sec;    // 解码累计耗时（bench）
    double          rga_sec;         // RGA 上采样累计耗时
    double          write_sec;       // NV12 写盘累计耗时
    double          postproc_sec;    // rga_sec + write_sec
} rkvc_session_stats;

rkvc_err rkvc_session_create(const rkvc_pipeline_desc *desc, rkvc_session **out);
rkvc_err rkvc_session_start(rkvc_session *session);
rkvc_err rkvc_session_stop(rkvc_session *session);
rkvc_err rkvc_session_get_route(const rkvc_session *session, rkvc_route_plan *plan);
rkvc_port *rkvc_session_port(rkvc_session *session, const char *name);
rkvc_err rkvc_session_get_stats(const rkvc_session *session, rkvc_session_stats *stats);
void     rkvc_session_destroy(rkvc_session *session);

rkvc_err rkvc_session_run_file(rkvc_session *session);  // 文件模板阻塞跑完
```

- `create`：`post_upscale_algo == RKVC_UPSCALE_AI_SR` 时须提供 `post_upscale_rkvc_model_path`。
- `run_file`：内部 start → 处理路径 → stop；流式模板勿用。

### Session 端口

| 端口      | 方向 | 数据类型                                 | 说明                                              |
| --------- | ---- | ---------------------------------------- | ------------------------------------------------- |
| `capture` | 输入 | `RKVC_BUF_VIDEO`                         | 采集/原始帧入口                                   |
| `output`  | 输出 | `RKVC_BUF_VIDEO` 或 `RKVC_BUF_BITSTREAM` | 解码帧或编码码流                                  |
| `preview` | 输出 | `RKVC_BUF_VIDEO`                         | `LIVE_CAPTURE`：与 `capture` 同帧侧抽；满则丢最旧 |

文件模式用 `rkvc_session_run_file()`，无需手动操作端口。详见 [architecture.md](architecture.md)。

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

### 拉取输出示例

```c
rkvc_port *out = rkvc_session_port(s, "output");
rkvc_buffer *pkt = NULL;
while (rkvc_port_pull(out, &pkt, 100) == RKVC_OK) {
    rkvc_buffer_bitstream_view view;
    rkvc_buffer_get_bitstream(pkt, &view);
    // 处理 view.data / view.size ...
    rkvc_buffer_unref(pkt);
    pkt = NULL;
}
```

---

## Port

```c
rkvc_err rkvc_port_push(rkvc_port *port, rkvc_buffer *buf);
rkvc_err rkvc_port_pull(rkvc_port *port, rkvc_buffer **buf, int timeout_ms);
```

| `timeout_ms` | 行为                                     |
| ------------ | ---------------------------------------- |
| `0`          | 非阻塞；队列空 → `RKVC_ERR_AGAIN`        |
| `> 0`        | 等待至多 N 毫秒；超时 → `RKVC_ERR_AGAIN` |
| `< 0`        | 无限阻塞直至有数据                       |

`push` 队列满时返回 `RKVC_ERR_AGAIN`（深度由 `queue_depth` 控制）。内部对 `buf` 引用计数 +1。

---

## Buffer

```c
typedef enum { RKVC_BUF_NONE, RKVC_BUF_VIDEO, RKVC_BUF_BITSTREAM } rkvc_buffer_kind;
typedef enum { RKVC_MEM_HOST, RKVC_MEM_DMABUF } rkvc_mem_type;

typedef struct {
    rkvc_mem_type  mem_type;
    int            fd;           // DMA-BUF fd，主机内存为 -1
    uint32_t       width, height;
    rkvc_pix_fmt   format;
    uint32_t       strides[4];
    int64_t        pts;
    uint64_t       modifier;     // DRM modifier
} rkvc_buffer_video_info;

typedef struct {
    const uint8_t *data;
    size_t         size;
    int64_t        pts, dts;
    int            key_frame;
} rkvc_buffer_bitstream_view;
```

```c
rkvc_buffer *rkvc_buffer_ref(rkvc_buffer *buf);
void         rkvc_buffer_unref(rkvc_buffer *buf);
rkvc_buffer_kind rkvc_buffer_kind_of(const rkvc_buffer *buf);

rkvc_err rkvc_buffer_alloc_video_host(rkvc_buffer **out,
                                      int w, int h, rkvc_pix_fmt fmt);
rkvc_err rkvc_buffer_alloc_bitstream(rkvc_buffer **out,
                                     const uint8_t *data, size_t size, int copy);
rkvc_err rkvc_buffer_get_video_info(const rkvc_buffer *buf,
                                    rkvc_buffer_video_info *info);
rkvc_err rkvc_buffer_get_video_planes(rkvc_buffer *buf,
                                      uint8_t *planes[4], int strides[4]);
rkvc_err rkvc_buffer_get_bitstream(const rkvc_buffer *buf,
                                   rkvc_buffer_bitstream_view *view);
rkvc_err rkvc_buffer_set_pts(rkvc_buffer *buf, int64_t pts);
```

- `alloc_bitstream(..., copy=1)`：深拷贝；`copy=0`：零拷贝引用 `data`，调用方须保持有效直至 `unref`。
- `get_video_planes`：仅主机帧 / 已映射 AVFrame；DMA-BUF 硬解帧请用 `get_video_info` + 节点下载。

缓冲区生命周期详见 [architecture.md](architecture.md#缓冲区生命周期)。

---

## 上采样算法与 RGA API

```c
typedef enum {
    RKVC_UPSCALE_NONE, RKVC_UPSCALE_NEAREST, RKVC_UPSCALE_BILINEAR,
    RKVC_UPSCALE_BICUBIC, RKVC_UPSCALE_AI_SR,  // rkvc_sr（需 RKNN 模型路径）
} rkvc_upscale_algo;

int rkvc_upscale_algo_from_name(const char *name, rkvc_upscale_algo *out);
const char *rkvc_upscale_algo_name(rkvc_upscale_algo algo);
```

名称映射：`none` / `nearest` / `bilinear` / `bicubic` / `rkvc_sr`。

### 一次性缩放（调用方提供平面缓冲）

```c
rkvc_err rkvc_upscale_yuv420p(const uint8_t *src, uint8_t *dst,
                              int src_w, int src_h, int dst_w, int dst_h,
                              rkvc_upscale_algo algo);
rkvc_err rkvc_upscale_nv12(const uint8_t *src, uint8_t *dst,
                           int src_w, int src_h, int dst_w, int dst_h,
                           rkvc_upscale_algo algo);
```

NV12 布局：`width*height` 字节 Y + `width*height/2` 字节 UV。不支持 `NONE` / `AI_SR`。

### 批量上下文（复用 RGA import，适合 bench / 多帧）

```c
typedef struct rkvc_upscale_ctx rkvc_upscale_ctx;

rkvc_upscale_ctx *rkvc_upscale_ctx_create(int src_w, int src_h,
                                          int dst_w, int dst_h,
                                          rkvc_upscale_algo algo);
void rkvc_upscale_ctx_destroy(rkvc_upscale_ctx *ctx);

uint8_t *rkvc_upscale_ctx_src_buf(rkvc_upscale_ctx *ctx);
uint8_t *rkvc_upscale_ctx_dst_buf(rkvc_upscale_ctx *ctx);
size_t rkvc_upscale_ctx_src_bytes(const rkvc_upscale_ctx *ctx);
size_t rkvc_upscale_ctx_dst_bytes(const rkvc_upscale_ctx *ctx);
rkvc_err rkvc_upscale_ctx_process(rkvc_upscale_ctx *ctx);
```

内部缓冲为 NV12 紧凑布局，可直接 `pread`/`pwrite` 或 `memcpy`。对应 CLI：`rkvc_yuv_upscale`。

**路径分工**：`FILE_ENCODE` / `rkvc_encode` 仅应用 `enc_scale_denom`（编码前下采样）；`post_upscale_algo` 只在 **解码路径**（`FILE_DECODE` / `rkvc_session_upscale`）生效。AI 超分：`post_upscale_algo=RKVC_UPSCALE_AI_SR` + 模型路径，或 CLI `rkvc_session_upscale --post-upscale rkvc_sr`。

---

## CLI

源码在 `cli/`（构建选项 `RKVC_BUILD_CLI`；安装到 `bin/`）。

| 程序                   | 用途                                                                 |
| ---------------------- | -------------------------------------------------------------------- |
| `rkvc_encode`          | 原始 NV12 → MP4（`-p`；`--enc-scale-denom`；`--svt-lp`/`--svt-rtc`） |
| `rkvc_decode`          | 容器/码流 → 原始 NV12                                                |
| `rkvc_transcode`       | 容器 → 容器，Router 选 codec（`--svt-lp`/`--svt-rtc`）               |
| `rkvc_bench`           | 四档 policy E2E fps 对比（**须** `-i INPUT.mp4`）                    |
| `rkvc_info`            | 硬件能力查询（`-j` JSON）                                            |
| `rkvc_session_upscale` | 硬解 + 后处理上采样（RGA / `rkvc_sr` + `--rkvc-sr-model`）           |
| `rkvc_yuv_upscale`     | YUV420p 批处理 RGA 缩放（`rkvc_upscale_ctx_*`）                      |

```bash
rkvc_encode -i raw.nv12 -o out.mp4 -s 1920x1080 -p balanced \
  --rc-mode cbr -b 4000000 --enc-scale-denom 2 --svt-lp 4 --svt-rtc 0

rkvc_transcode -i in.mp4 -o out.mp4 -p quality -b 6000000 --svt-lp 4
rkvc_transcode -i in.mp4 -o hq.mp4 -p offline -b 6000000 --svt-lp 4

rkvc_session_upscale -i stream.mp4 -o out.nv12 --width 1920 --height 1080 \
  --enc-scale-denom 2 --post-upscale rkvc_sr --rkvc-sr-model model.rknn

rkvc_bench -i clip.mp4
rkvc_info -j
```

---

## Doxygen

启用 `-DRKVC_BUILD_DOCS=ON` 构建时，Doxygen 从 `include/rkvc/*.h` 生成 HTML API 文档（与本文档互补）。
