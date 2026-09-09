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
  终端错误经 `rkvc_session_error_text` 取阶段明细。
- FRAME_SINK 会话：`wrap → push → try_pull / pull → push_eos → EOF`，
  背压由调用方消化（push 遇 `RKVC_AGAIN` 先 try_pull 排空再重试）；
  排空只在 EOS 后进行，flush 前排空会与攒帧编码器互等死锁。
- `quality = {bitrate_bps, qp, gop_size, fps}`，零值保留后端默认；
  `decode` 的宽高取输出几何，`encode` 取输入几何，`upscale` 输出固定 3×。

## 模型

RKMDL1 容器（魔数 `RKMDL1\x00\x00`、128B 头、88B 条目、多 qppatch 载荷），
`rkvc_context_add_model_file` 显式注册或 `--model-dir` 扫描注册，
`--model-id` 按导出 stem 选择。NPU 侧 I/O 契约：输入 NHWC、输出 NCHW。
