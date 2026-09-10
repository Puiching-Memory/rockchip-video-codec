# 架构

C++20、无异常、无 RTTI。错误经 `Status` / `Result<T>` / `Diag` 逐层携带，
不抛异常；插件边界只暴露 C ABI。

## 运行路径

~~~text
application / rkvc CLI / examples
        |
        v  core/include/rkvc/rkvc.h (C ABI 0.5.0, 唯一稳定接口)
rkvc_context -> plugin registry + model registry + device probe
        |
        v
rkvc_session_request -> planner -> pipeline
        |
        v
FILE source/sink 或 FRAME_SINK push/pull
        |
        v
session_wait (FILE) / pull 至 EOF (FRAME_SINK)
~~~

核心只含上下文、会话规划与执行、帧所有权、诊断、插件装载与模型注册表。
媒体实现在四个独立 codec 工程，各自 CMake、各自 doctest，
只依赖 `core/include`。核心不链接 MPP、RGA、RKNN、SVT 类型。

## 插件握手

插件是 MODULE 库（`rkvc_h264h265` / `rkvc_mlvc` / `rkvc_av1` / `rkvc_sr`），
唯一导出 `rkvc_plugin_query(host_abi)`，返回 `PluginDescriptor`
（`kPluginAbi=1` + 工具链指纹 + 工厂表）。指纹或 ABI 对不上即拒载，
只记诊断不中断。宿主经 `--backend-dir`（可重复）传入可信目录并扫描 `.so`；
`inspect backends` 可逐个 dlopen 探查。

## 会话与端点

- FILE 会话：`create → start → wait`，整条管线在库内跑完；
  终端错误经 `rkvc_session_error_text` 取阶段明细
  （`wait` 返回非 OK 后调用，OK 时 buf 置空）。
- FRAME_SINK 会话：`wrap → push → try_pull / pull → push_eos → EOF`，
  背压由调用方消化（push 遇 `RKVC_AGAIN` 先 try_pull 排空再重试）；
  排空只在 EOS 后进行，flush 前排空会与攒帧编码器互等死锁。
- 端点由 request 的 `input` / `output` 描述：`RKVC_ENDPOINT_FILE`（uri 路径，
  仅 FILE 会话可行）与 `RKVC_ENDPOINT_FRAME_SINK`。`RKVC_AGAIN` 表示有界
  队列暂时满/空，重试而非报错；`RKVC_EOF` 只在 EOS 排空后出现。
- `quality = {bitrate_bps, qp, gop_size, fps}`，零值保留后端默认；
  `decode` 的宽高取**输出**几何，`encode` 取输入几何，`upscale` 输出固定 3×。

## C ABI

应用只需包含 `rkvc/rkvc.h`（`core/include`）。C ABI 版本 0.5.0，
全部 22 个函数见头文件；下表只列对象职责。

| 对象                   | 责任                                                                           |
| ---------------------- | ------------------------------------------------------------------------------ |
| `rkvc_context`         | 插件扫描装载、设备探测（`rkvc_probe_device`）、模型注册                        |
| `rkvc_session_request` | operation / codec / 输入输出端点 / quality / model_id                          |
| `rkvc_session`         | 一次执行的 create / start / push / try_pull / pull / push_eos / wait / destroy |
| `rkvc_frame`           | 引用计数的 HOST 媒体数据（wrap 借用、release 归还）                            |
| `rkvc_diagnostic`      | create/start 失败的原因链（`rkvc_diag_fmt_text` 输出）                         |

所有可扩展公开结构版本优先：先调 `xxx_init(&x, sizeof(x))` 填
`struct_size`（与 `version`），尺寸不匹配即拒收，保证前向兼容。

- `operation`：`RKVC_OP_ENCODE / DECODE / UPSCALE`（无转码）；
  `codec`：`H264 / HEVC / AV1 / MLVC`（`AUTO` 留给 upscale）。
- `model_id` 透传到模型注册表（导出 stem，如 `mlvc_rk3576_qp21_decoder`）；
  `rkvc_context_add_model_file` 逐个注册 RKMDL1。
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

## 模型

RKMDL1 容器（魔数 `RKMDL1\x00\x00`、128B 头、88B 条目、多 qppatch 载荷），
`rkvc_context_add_model_file` 显式注册或 `--model-dir` 扫描注册，
`--model-id` 按导出 stem 选择。NPU 侧 I/O 契约：输入 NHWC、输出 NCHW。
