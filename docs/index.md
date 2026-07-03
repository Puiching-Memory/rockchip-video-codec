# rkvc — RK3588 多码率视频编解码库 (v2)

面向 RK3588 的 C 库，基于 [ffmpeg-rockchip](https://github.com/nyanmisaka/ffmpeg-rockchip) RKMPP 硬件加速与 SVT-AV1，提供 **Session + Pipeline + Codec Router** 统一 API。

当前版本：**0.2.1**

## 功能特性

- **Codec Router** — `REALTIME`→H.264 RKMPP、`BALANCED`→HEVC RKMPP、`QUALITY`→SVT-AV1 + `av1_rkmpp` 硬解
- **Session API** — `rkvc_session` + 命名端口 `capture` / `output` / `preview`
- **DMA-BUF 缓冲** — `rkvc_buffer` 统一视频帧与码流；RGA 硬件缩放
- **模板管线** — 文件编解码、转码、AV1 存储、LiveCapture（V4L2 待接）
- **下采样 + 后处理上采样** — `enc_scale_denom` + `post_upscale_algo`（RGA 插值 / `rkvc_sr` RKNN 超分）

## 性能 (RK3588, 1080p E2E)

| 路线 | E2E fps | policy |
|------|---------|--------|
| H.264 RKMPP | ~36 | `REALTIME` |
| HEVC RKMPP | ~27 | `BALANCED` |
| SVT-AV1 + av1_rkmpp | ~24 | `QUALITY` |

完整 RD 码率-画质对比见 [bench/README.md](../bench/README.md)。

## 导航

### 开发与集成

- [快速开始](getting-started.md) — 依赖构建、编译、首次运行
- [架构](architecture.md) — Session 图、Codec Router、节点管线
- [API 参考](api.md) — v2 公共 API（完整函数/结构体说明）
- [v1 → v2 迁移](migration.md) — 破坏性升级迁移指南
- [YUV 超分模型规格](sr-model-yuv-spec.md) — YUV-native SR 训练/推理/RKVC 模型导出规范（设计稿）

### 运维与交付

- [基准测试](benchmark.md) — `rkvc_bench` 与 RD 套件
- [测试](testing.md) — 测试矩阵与质量门禁
- [打包与分发](packaging.md) — 可移植包、DEB、CPack
- [交付文档](delivery.md) — 客户交付清单与故障排查

### 发布包用户文档（`docs/release/`）

可移植包内同步携带，面向终端用户与二次开发：

- [release/README.md](release/README.md) — 包内快速上手
- [release/USAGE.md](release/USAGE.md) — CLI 工具说明
- [release/DEVELOPMENT.md](release/DEVELOPMENT.md) — 集成与 API 示例
- [release/EXAMPLES.md](release/EXAMPLES.md) — 示例程序索引

### Doxygen

构建时加 `-DRKVC_BUILD_DOCS=ON` 可从 `include/rkvc/*.h` 生成 HTML API 文档（与 [api.md](api.md) 互补）。
