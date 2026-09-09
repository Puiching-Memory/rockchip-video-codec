# API

应用只需包含 `rkvc/rkvc.h`（`core/include`）。C ABI 版本 0.5.0，
全部 22 个函数见头文件；下表只列对象职责。

| 对象 | 责任 |
| ---- | ---- |
| `rkvc_context` | 插件扫描装载、设备探测（`rkvc_probe_device`）、模型注册 |
| `rkvc_session_request` | operation / codec / 输入输出端点 / quality / model_id |
| `rkvc_session` | 一次执行的 create / start / push / try_pull / pull / push_eos / wait / destroy |
| `rkvc_frame` | 引用计数的 HOST 媒体数据（wrap 借用、release 归还） |
| `rkvc_diagnostic` | create/start 失败的原因链（`rkvc_diag_fmt_text` 输出） |

所有可扩展公开结构版本优先：先调 `xxx_init(&x, sizeof(x))` 填
`struct_size`（与 `version`），尺寸不匹配即拒收，保证前向兼容。

- `operation`：`RKVC_OP_ENCODE / DECODE / UPSCALE`（无转码）。
  `codec`：`H264 / HEVC / AV1 / MLVC`（`AUTO` 留给 upscale）。
- `quality = {bitrate_bps, qp, gop_size, fps}`：零值保留后端默认；
  `decode` 的宽高是**输出**几何，`encode` 是输入几何，`upscale` 输出固定 3×。
- 端点：`RKVC_ENDPOINT_FILE`（uri 路径，`wait` 跑完全程）与
  `RKVC_ENDPOINT_FRAME_SINK`（`wrap/push/try_pull/pull/push_eos` 流式）。
  `RKVC_AGAIN` 表示有界队列暂时满/空，重试而非报错；`RKVC_EOF` 只在
  EOS 排空后出现。
- `model_id` 透传到模型注册表（导出 stem，如 `mlvc_rk3576_qp21_decoder`）；
  `rkvc_context_add_model_file` 逐个注册 RKMDL1。
- `rkvc_session_error_text` 取 FILE 会话终端错误的阶段明细
  （`wait` 返回非 OK 后调用，OK 时 buf 置空）。
- `policy`（REALTIME / BALANCED / QUALITY / OFFLINE）与 `queue_capacity`
  （0 取默认）控制执行形态；`rkvc_probe_device` 填 `rkvc_caps`
 （soc / has_mpp_encoder / has_mpp_decoder / has_rknn / npu_cores）。
- 像素格式另有 NV21 / NV16 / P010 / RGB24（CLI 只暴露 nv12 / yuv420p）；
  `rkvc_frame_spec` 另有 `domain`（HOST / DMABUF）、`stride` / `ver_stride` /
  `modifier`，帧标志有 KEYFRAME / DISCONTINUITY / CORRUPT；时间戳哨兵
  `RKVC_FRAME_TS_UNKNOWN`。

插件实现者看各 codec 工程的 `Factory` 接口与 `rkvc_plugin_query` 握手
（`core/include/rkvc/plugin.hpp`），以 `examples/integration-c/` 为对接样板。
上层内嵌契约见 [语义编解码 SDK 集成](semantic-codec-sdk-integration.md)。
