# rk3588-ai-video-codec

面向 RK3588 的多码率视频管线 C 库（**rkvc v2**）：Session + Codec Router，支持 H.264 / HEVC / AV1，基于 DMA-BUF 热路径与 ffmpeg-rockchip 容器层。

## 功能

- **Codec Router** — `REALTIME`→H.264、`BALANCED`→HEVC、`QUALITY`→SVT-AV1 p11、`OFFLINE`→SVT-AV1 p4（非实时）
- **Session API** — `rkvc_session` + 命名端口 `capture` / `output`（`preview` 占位）
- **DMA-BUF 缓冲** — `rkvc_buffer` 统一视频/码流；RGA NV12 缩放
- **后处理上采样** — 解码路径：RGA 插值或 RKNN 超分（`rkvc_sr`）；编码路径仅 `enc_scale_denom` 下采样
- **模板管线** — 文件编解码、转码、AV1 存储、**LiveCapture（V4L2）**
- **ROI / 配额** — `rkvc_session_set_roi`（MPP 硬区域 QP）；`rkvc_runtime_set_quota`
- **UDP/RTP 原语** — `rkvc_net_send` / `recv`（分片重组；无国标信令）

## 性能 (RK3588, 1080p E2E, bench/)

| 路线                    | E2E fps | policy                    |
| ----------------------- | ------- | ------------------------- |
| H.264 RKMPP             | ~36     | REALTIME                  |
| HEVC RKMPP              | ~27     | BALANCED                  |
| SVT-AV1 p11 + av1_rkmpp | ~24     | QUALITY                   |
| SVT-AV1 p4 + av1_rkmpp  | ~2      | OFFLINE（非实时，≥1 fps） |

## 快速开始

```bash
git submodule update --init --depth 1
./scripts/build-svt.sh
./scripts/install-librga.sh         # third_party/librga → .build/deps/librga-install
./scripts/rebuild-ffmpeg-rkmpp.sh   # h264/hevc/av1 硬解 + h264/hevc 硬编

cmake --preset default
cmake --build --preset default

./.build/release/example_encode_file -o /tmp/bench_in.mp4 -s 640x480 -n 30
./.build/release/rkvc_bench -i /tmp/bench_in.mp4
```

完整依赖与权限见 [docs/getting-started.md](docs/getting-started.md)。构建目录约定见 [docs/build-layout.md](docs/build-layout.md)。

## 许可证

本项目以 **GNU Affero General Public License v3 (AGPLv3)** 开源（见 [LICENSE](LICENSE)）。

- 允许商用、修改、再分发，但**衍生/合并作品须以 AGPLv3 开源**；通过网络提供本程序服务的（含 SaaS）须向用户提供对应源码
- 闭源商业使用需另行取得商业授权（配合 `RKVC_ENABLE_LICENSE` 授权机制）
- `third_party/` 内各组件（ffmpeg-rockchip、SVT-AV1、mpp、librga、libsodium）版权归其各自所有者，适用各自许可证

## RD 基准测试（bench/）

端到端码率-画质与性能对比，默认四路：**H.264 / H.265 / SVT-AV1 / rkvc**。

![RD 曲线（1080p E2E）](docs/images/bench/rd_curve_e2e.png)

![E2E 性能对比](docs/images/bench/perf_e2e.png)

```bash
./scripts/run-bench.sh /path/to/1080p.mp4
PLOT_ONLY=1 ./scripts/run-bench.sh          # 仅重绘图表
RUN_CODECS=h264,rkvc ./scripts/run-bench.sh clip.mp4
```

详见 [bench/README.md](bench/README.md)。

## v2 API 示例

```c
rkvc_pipeline_desc d;
rkvc_pipeline_from_template(RKVC_TEMPLATE_FILE_TRANSCODE, &d);
d.input_path = "in.mp4";
d.output_path = "out.mp4";
d.policy = RKVC_POLICY_BALANCED;

rkvc_session *s = NULL;
rkvc_session_create(&d, &s);
rkvc_session_run_file(s);
rkvc_session_destroy(s);
```

## 测试

```bash
cmake --preset tests
cmake --build --preset tests
ctest --preset tests -j1 --output-on-failure

# RK3588 硬件（每用例独立进程，串行；产物在 .build/tests/）
RKVC_RUN_HARDWARE_TESTS=1 ctest --test-dir .build/tests -j1 -R 'test_session_' --output-on-failure
```

## 依赖

- Rockchip BSP、`third_party/` 子模块：MPP、ffmpeg-rockchip、SVT-AV1、[librga](https://github.com/airockchip/librga)
- libdrm；可选 `librknnrt`（`rkvc_sr`；可移植包可自带）

## 文档

| 文档                                                   | 说明                             |
| ------------------------------------------------------ | -------------------------------- |
| [docs/index.md](docs/index.md)                         | 文档首页与导航                   |
| [docs/getting-started.md](docs/getting-started.md)     | 构建与首次运行                   |
| [docs/build-layout.md](docs/build-layout.md)           | 构建目录约定（全部在 `.build/`） |
| [docs/api.md](docs/api.md)                             | v2 API 完整参考                  |
| [docs/architecture.md](docs/architecture.md)           | Session / Router / 节点架构      |
| [docs/benchmark.md](docs/benchmark.md)                 | 性能与 RD 基准                   |
| [docs/testing.md](docs/testing.md)                     | 测试矩阵                         |
| [docs/packaging.md](docs/packaging.md)                 | 可便携包与分发                   |
| [docs/delivery.md](docs/delivery.md)                   | 客户交付清单                     |
| [docs/sr-model-yuv-spec.md](docs/sr-model-yuv-spec.md) | YUV-native SR 设计稿             |
| [docs/release/](docs/release/)                         | 发布包用户文档                   |
| [CHANGELOG.md](CHANGELOG.md)                           | 版本变更记录                     |

版本：见 `CMakeLists.txt` `project(VERSION)` / `rkvc_version()`（Session + Codec Router API）
