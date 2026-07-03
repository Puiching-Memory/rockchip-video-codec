# rk3588-ai-video-codec

面向 RK3588 的多码率视频管线 C 库（**rkvc v2**）：Session + Codec Router，支持 H.264 / HEVC / AV1，基于 DMA-BUF 热路径与 ffmpeg-rockchip 容器层。

## 功能

- **Codec Router** — `REALTIME`→H.264 RKMPP、`BALANCED`→HEVC、`QUALITY`→SVT-AV1+硬解
- **Session API** — `rkvc_session` + 命名端口 `capture`/`output`/`preview`
- **DMA-BUF 缓冲** — `rkvc_buffer` 统一视频/码流；RGA NV12 缩放
- **后处理上采样** — RGA 插值（`nearest`/`bilinear`/`bicubic`）或 RKNN 超分（`rkvc_sr`）
- **模板管线** — 文件编解码、转码、AV1 存储、LiveCapture（V4L2 待接）

## 性能 (RK3588, 1080p E2E, bench/)

| 路线 | E2E fps | policy |
|------|---------|--------|
| H.264 RKMPP | ~36 | REALTIME |
| HEVC RKMPP | ~27 | BALANCED |
| SVT-AV1 + av1_rkmpp | ~24 | QUALITY |

## 快速开始

```bash
git submodule update --init --depth 1 third_party/SVT-AV1
./scripts/build-svt.sh
./scripts/rebuild-ffmpeg-rkmpp.sh   # h264/hevc/av1 硬解 + h264/hevc 硬编

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4

./build/example_encode_file -o /tmp/bench_in.mp4 -s 640x480 -n 30
./build/rkvc_bench -i /tmp/bench_in.mp4
```

## RD 基准测试（bench/）

端到端码率-画质与性能对比，默认四路：**H.264 / H.265 / SVT-AV1 / rkvc v2**。

![RD 曲线（1080p E2E）](docs/images/bench/rd_curve_e2e.png)

![E2E 性能对比](docs/images/bench/perf_e2e.png)

```bash
./scripts/run-bench.sh /path/to/1080p.mp4
PLOT_ONLY=1 ./scripts/run-bench.sh          # 仅重绘图表
RUN_CODECS=h264,rkvc-v2 ./scripts/run-bench.sh clip.mp4
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
cmake -B build -DRKVC_BUILD_TESTS=ON
cmake --build build -j4
ctest --test-dir build -j1 --output-on-failure

# RK3588 硬件（每用例独立进程，串行）
RKVC_RUN_HARDWARE_TESTS=1 ctest --test-dir build -j1 -R test_session
```

## 依赖

- Rockchip BSP、MPP、`third_party/ffmpeg-rockchip`（`rebuild-ffmpeg-rkmpp.sh`）
- SVT-AV1 submodule、libdrm、RGA

## 文档

| 文档 | 说明 |
|------|------|
| [docs/index.md](docs/index.md) | 文档首页与导航 |
| [docs/getting-started.md](docs/getting-started.md) | 构建与首次运行 |
| [docs/api.md](docs/api.md) | v2 API 参考 |
| [docs/architecture.md](docs/architecture.md) | Session / Router / 节点架构 |
| [docs/migration.md](docs/migration.md) | v0.1.x → v0.2.x 迁移 |
| [docs/benchmark.md](docs/benchmark.md) | 性能与 RD 基准 |
| [docs/testing.md](docs/testing.md) | 测试矩阵 |
| [docs/packaging.md](docs/packaging.md) | 可移植包与分发 |
| [docs/delivery.md](docs/delivery.md) | 客户交付清单 |

版本：**0.2.1**（v2 Session API；0.2.0 起破坏性替换 v1 `encoder`/`decoder`/`stream` API）
