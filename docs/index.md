# rkvc

面向 Rockchip Linux 的 C++20 会话式媒体运行库：一个稳定 C ABI（0.5.0）、
一个核心静态库（`rkvc-core-static`）、四个独立 codec 插件与一个 `rkvc` CLI。
无异常、无 RTTI。

## 能力

| 组件       | 能力                                                                                                     |
| ---------- | -------------------------------------------------------------------------------------------------------- |
| core       | 会话规划与执行、有界队列背压、EOS / 取消 / flush、FILE 与 FRAME_SINK 双端点、RKMDL1 模型注册表、文本诊断 |
| `h264h265` | MPP H.264 / HEVC 硬编码与硬解码                                                                          |
| `pspack` | GB28181 H.265/H.264 over PS 打包与解包（pack/system/PSM/PES，AU 组装）                                   |
| `av1`      | SVT-AV1 软件编码                                                                                         |
| `mlvc`     | NPU 神经视频编解码，`.mlvc` 容器，P-only、无 B 帧                                                        |
| `sr`       | Phase-RLFN 固定 3× NPU 超分（NHWC 输入 / NCHW 输出）                                                     |
| `rkvc` CLI | `caps` / `version` / `inspect` / `encode` / `decode` / `upscale`，只用长选项                             |

MLVC 编解码链路：

![MLVC 架构](images/mlvc-architecture.png)

## 文档

- [快速开始](getting-started.md)：构建、上手命令、`.build/` 目录与开关
- [架构](architecture.md)：运行路径、插件握手、会话与端点、C ABI
- [API 引用](api/)：C ABI 与 C++ 核心头文件的符号参考（Doxide 生成）
- [部署](deployment.md)：发布产物、板端部署、第三方运行时、符号审计
- [测试](testing.md)：C++ / Python / Bash 三类测试与板端回归
- [MLVC RKNN 导出](mlvc-rknn-export.md)：ONNX → RKNN 模型生产与 NPU I/O 约定
- [SR 模型规格](sr-model-yuv-spec.md)：Phase-RLFN I/O 契约与实测数据
- [语义编解码 SDK 集成](semantic-codec-sdk-integration.md)：宿主内嵌契约与排障
- [GB28181 推流](gb28181-streaming.md)：PS over RTP 媒体面、PS 容器格式与 pspack 契约
