# 二次开发指南

面向基于 rkvc v2 库进行二次开发的开发者。

## 开发环境

- RK3588 平台，Linux 内核 5.10+
- GCC / Clang（C11）
- pkg-config 或 CMake

```bash
export PKG_CONFIG_PATH=/path/to/rkvc/share/pkgconfig:$PKG_CONFIG_PATH
export LD_LIBRARY_PATH=/path/to/rkvc/lib:$LD_LIBRARY_PATH   # 无 RPATH 时
```

## 集成方式

### pkg-config

```bash
gcc -o myapp myapp.c $(pkg-config --cflags --libs rkvc)
```

### CMake

```cmake
find_package(rkvc REQUIRED)
target_link_libraries(myapp PRIVATE rkvc::shared)
```

## API 概览

v2 核心概念：

| 概念 | 类型 | 说明 |
|------|------|------|
| 管线描述 | `rkvc_pipeline_desc` | 模板、policy、分辨率、路径等 |
| 会话 | `rkvc_session` | 管线实例 |
| 端口 | `rkvc_port` | `capture` / `output` / `preview` |
| 缓冲 | `rkvc_buffer` | 视频帧或码流 |
| 路由 | `rkvc_route_plan` | Router 解析结果 |

头文件：

```c
#include "rkvc/rkvc.h"   // 包含 types / buffer / policy / pipeline / session / port
```

完整 API 说明见源码树 [docs/api.md](../api.md)；可移植包内可参考下文各节及 `include/rkvc/*.h` 头文件注释。

| 头文件 | 主要内容 |
|--------|----------|
| `types.h` | `rkvc_err`、`rkvc_pix_fmt`、`rkvc_rc_mode`、`rkvc_upscale_algo` |
| `buffer.h` | `rkvc_buffer` 分配、平面访问、码流视图 |
| `policy.h` | `rkvc_policy`、`rkvc_codec`、`rkvc_route_resolve` |
| `pipeline.h` | `rkvc_pipeline_desc`、模板 |
| `session.h` | `rkvc_session_*`、统计 |
| `port.h` | `rkvc_port_push` / `rkvc_port_pull` |
| `rkvc.h` | init、日志、caps、探测、RGA 上采样 |

## 文件转码

```c
#include "rkvc/rkvc.h"
#include <stdio.h>

int main(void) {
    rkvc_init();

    rkvc_pipeline_desc d;
    rkvc_pipeline_from_template(RKVC_TEMPLATE_FILE_TRANSCODE, &d);
    d.input_path  = "input.mp4";
    d.output_path = "output.mp4";
    d.policy      = RKVC_POLICY_BALANCED;
    d.bitrate     = 4000000;
    d.width       = 1920;
    d.height      = 1080;

    rkvc_session *s = NULL;
    rkvc_err err = rkvc_session_create(&d, &s);
    if (err != RKVC_OK) {
        fprintf(stderr, "%s\n", rkvc_err_str(err));
        return 1;
    }

    err = rkvc_session_run_file(s);
    rkvc_session_destroy(s);
    rkvc_deinit();
    return err == RKVC_OK ? 0 : 1;
}
```

## 文件编码（端口 push）

```c
rkvc_pipeline_desc d;
rkvc_pipeline_from_template(RKVC_TEMPLATE_FILE_ENCODE, &d);
d.output_path = "out.mp4";
d.policy      = RKVC_POLICY_REALTIME;
d.width = 1920; d.height = 1080;

rkvc_session *s;
rkvc_session_create(&d, &s);
rkvc_session_start(s);

rkvc_port *cap = rkvc_session_port(s, "capture");
for (int i = 0; i < 300; i++) {
    rkvc_buffer *buf = NULL;
    rkvc_buffer_alloc_video_host(&buf, 1920, 1080, RKVC_PIX_FMT_NV12);
    rkvc_buffer_set_pts(buf, i);

    uint8_t *planes[4];
    int strides[4];
    rkvc_buffer_get_video_planes(buf, planes, strides);
    // 填充 NV12 像素到 planes[0] (Y) 和 planes[1] (UV)

    rkvc_port_push(cap, buf);
    rkvc_buffer_unref(buf);
}

rkvc_session_stop(s);
rkvc_session_destroy(s);
```

## 流式 pull 码流

```c
rkvc_port *out = rkvc_session_port(s, "output");
rkvc_buffer *pkt = NULL;
while (rkvc_port_pull(out, &pkt, 100) == RKVC_OK) {
    rkvc_buffer_bitstream_view view;
    rkvc_buffer_get_bitstream(pkt, &view);
    // 发送 view.data / view.size 到网络
    rkvc_buffer_unref(pkt);
    pkt = NULL;
}
```

### `timeout_ms` 语义

| 值 | 行为 |
|----|------|
| `0` | 非阻塞；队列空 → `RKVC_ERR_AGAIN` |
| `> 0` | 等待至多 N 毫秒；超时 → `RKVC_ERR_AGAIN` |
| `< 0` | 无限阻塞直至有数据 |

`push` 在队列满时返回 `RKVC_ERR_AGAIN`（深度由 `d.queue_depth` 控制，默认 3）。

## Buffer 进阶

```c
// 查询种类与元数据（硬解 DMA-BUF 帧用 get_video_info，勿假设 get_video_planes 可写）
rkvc_buffer_kind kind = rkvc_buffer_kind_of(buf);
rkvc_buffer_video_info info;
rkvc_buffer_get_video_info(buf, &info);  // width/height/format/fd/modifier/pts

// 码流零拷贝包装（调用方保持 data 有效）
rkvc_buffer *pkt = NULL;
rkvc_buffer_alloc_bitstream(&pkt, data, size, /*copy=*/0);
```

## 日志与调试

```c
rkvc_set_log_level(AV_LOG_DEBUG);   // 与 FFmpeg AV_LOG_* 一致
// 或环境变量：export RKVC_LOG_LEVEL=debug
```

## 文件哈希（测试/校验）

```c
char hex[65];
rkvc_hash_file("clip.mp4", "sha256", hex, sizeof(hex));
```

## RGA 独立上采样 API

不经过 Session 时，可直接调用 RGA 缩放（对应 CLI `rkvc_yuv_upscale`）：

```c
// 一次性
rkvc_upscale_nv12(src, dst, sw, sh, dw, dh, RKVC_UPSCALE_BILINEAR);

// 批量（复用 RGA import，适合多帧文件）
rkvc_upscale_ctx *ctx = rkvc_upscale_ctx_create(sw, sh, dw, dh, RKVC_UPSCALE_BILINEAR);
uint8_t *src_buf = rkvc_upscale_ctx_src_buf(ctx);
uint8_t *dst_buf = rkvc_upscale_ctx_dst_buf(ctx);
// fread → src_buf → rkvc_upscale_ctx_process(ctx) → fwrite dst_buf
rkvc_upscale_ctx_destroy(ctx);
```

不支持 `RKVC_UPSCALE_NONE` / `RKVC_UPSCALE_AI_SR`；AI 超分须走 Session + `post_upscale_algo`。

## Codec Router

```c
rkvc_route_plan plan;
rkvc_route_resolve(&d, &plan);
printf("enc=%s dec=%s reason=%s\n",
       plan.enc_name, plan.dec_name, plan.reason);
```

policy 说明：

- `RKVC_POLICY_REALTIME` → `h264_rkmpp`
- `RKVC_POLICY_BALANCED` → `hevc_rkmpp`（高帧率 1080p+ 回退 H.264）
- `RKVC_POLICY_QUALITY` → `svt-av1` + `av1_rkmpp`

## 下采样 + 上采样

```c
d.enc_scale_denom   = 2;                    // 编码前宽高各减半
d.post_upscale_algo = RKVC_UPSCALE_BILINEAR; // 解码后 RGA 还原
d.post_upscale_rkvc_model_path = NULL;      // RKVC_UPSCALE_AI_SR 时必填
d.width  = 1920;  // 显示分辨率不变
d.height = 1080;
```

AI 超分：`post_upscale_algo = RKVC_UPSCALE_AI_SR` + `post_upscale_rkvc_model_path`；或 CLI `rkvc_session_upscale --post-upscale rkvc_sr`。YUV-native 模型规格见项目 `docs/sr-model-yuv-spec.md`。

## 能力查询

```c
rkvc_caps caps;
rkvc_query_caps(&caps);
if (!caps.has_hevc_enc)
    fprintf(stderr, "HEVC encoder unavailable\n");

rkvc_err perm = rkvc_check_hw_permissions();
if (perm == RKVC_ERR_PERMISSION)
    fprintf(stderr, "device permission denied\n");
```

## 错误处理

```c
rkvc_err err = rkvc_session_create(&d, &s);
if (err != RKVC_OK) {
    fprintf(stderr, "error: %s\n", rkvc_err_str(err));
    return 1;
}
```

## 性能优化建议

- 文件批处理用 `rkvc_session_run_file()`，避免逐帧端口开销
- `REALTIME` 策略优先用于监控/推流低延迟场景
- `enc_scale_denom > 1` 可降低编码负载，代价是上采样画质损失
- 复用 `rkvc_buffer`，避免频繁 alloc/free
- 查询 `rkvc_session_get_stats()` 监控丢帧与 fps；bench 路径另有 `decode_sec` / `rga_sec` / `write_sec` / `postproc_sec`

## 常见问题

**Q: 如何从 v0.1.x 迁移？**  
A: v1 的 `encoder`/`decoder`/`stream`/`frame` API 已移除，参见 `migration.md`。

**Q: 能否直接编码 MP4 输入？**  
A: 不能。`rkvc_encode -i` 仅接受原始 NV12。压缩文件请用 `rkvc_transcode` 或 decode 模板。

**Q: AV1 编码需要什么？**  
A: `QUALITY` policy 或 `RKVC_CODEC_AV1`，需要 `libSvtAv1Enc.so`（包内已携带）。

**Q: 低延迟模式？**  
A: 设置 `d.low_latency = 1`，使用 `RKVC_TEMPLATE_LIVE_CAPTURE`（V4L2 待接）。

**Q: 完整 API 在哪里？**  
A: 项目 `docs/api.md`；头文件 `include/rkvc/*.h` 含 Doxygen 注释；`-DRKVC_BUILD_DOCS=ON` 可生成 HTML。
