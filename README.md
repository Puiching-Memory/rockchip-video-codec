# rockchip-video-codec

面向 Rockchip SoC 的多码率视频管线 C 库（**rkvc**）：Session + Codec Router，支持 H.264 / HEVC / AV1 与 **MLVC 神经视频编解码**，基于 DMA-BUF 热路径与 ffmpeg-rockchip 容器层，纯 C17 实现（无 C++ 依赖）。当前支持板卡：**RK3588**（权威）、**RV1126B**（骨架），通过板卡 profile 抽象层扩展。

## 功能特性

- **Codec Router** — 五档语义策略自动选路：

  | policy     | 编解码方案                              | 目标                   |
  | ---------- | --------------------------------------- | ---------------------- |
  | `realtime` | H.264 RKMPP 硬编硬解                    | ≥30 fps @1080p         |
  | `balanced` | HEVC RKMPP（高帧率 1080p+ 回退 H.264）  | 均衡                   |
  | `quality`  | SVT-AV1 preset 11 + `av1_rkmpp` 硬解    | 近实时高质量           |
  | `offline`  | SVT-AV1 preset 4 + `av1_rkmpp`          | 非实时高质量（≥1 fps） |
  | `neural`   | MLVC 神经编解码（RKNN NPU + 纯 C rANS） | 超低码率，固定 640×368 |

- **Session API** — `rkvc_session` + 命名端口 `capture` / `output`（`preview` 占位）
- **MLVC 神经编解码** — 与 264/265 平行的端到端一等编解码器：编码 `video → .mlvc`、纯解码 `.mlvc → .yuv`、转码 `.mlvc → .mp4`；自定义 `.mlvc` 容器；熵编码为纯 C rANS（算法源自 msrtc_rans，MIT）
- **DMA-BUF 缓冲** — `rkvc_buffer` 统一视频/码流；RGA NV12 缩放
- **后处理上采样** — 解码路径：RGA 插值或 RKNN 超分（`rkvc_sr`）；编码路径仅 `enc_scale_denom` 下采样
- **模板管线** — 文件编解码、转码、AV1 存储、MLVC 存储、**LiveCapture（V4L2）**
- **ROI / 配额** — `rkvc_session_set_roi`（MPP 硬区域 QP）；`rkvc_runtime_set_quota`
- **UDP/RTP 原语** — `rkvc_net_send` / `recv`（分片重组；无国标信令）

## 性能 (RK3588, 1080p E2E, tools/bench/)

| 路线                    | E2E fps | policy                    |
| ----------------------- | ------- | ------------------------- |
| H.264 RKMPP             | ~36     | REALTIME                  |
| HEVC RKMPP              | ~27     | BALANCED                  |
| SVT-AV1 p11 + av1_rkmpp | ~24     | QUALITY                   |
| SVT-AV1 p4 + av1_rkmpp  | ~2      | OFFLINE（非实时，≥1 fps） |

MLVC 神经路线（`neural`，固定 640×368，面向带宽极端受限场景）：~0.06 bpp（~66.7 kbps）时 PSNR Y ≈ 26.8 dB、SSIM Y ≈ 0.80。

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

## CLI 工具

| 工具                   | 说明                                                    |
| ---------------------- | ------------------------------------------------------- |
| `rkvc_info`            | 板卡 / 能力查询（文本 / JSON，含 `board` 字段）         |
| `rkvc_encode`          | 文件编码（`-p` 五档策略 / `-c` 显式 codec）             |
| `rkvc_decode`          | 文件解码                                                |
| `rkvc_transcode`       | 转码；`.mlvc` 编码 / 纯解码 / 转码三种操作              |
| `rkvc_bench`           | 内置性能基准（REALTIME / BALANCED / QUALITY / OFFLINE） |
| `rkvc_session_upscale` | 硬解 + 后处理上采样（RGA 插值 / `rkvc_sr` AI 超分）     |
| `rkvc_yuv_upscale`     | 原始 YUV 上采样（bench 参考帧 prep）                    |

MLVC 用法示例：

```bash
# 编码：mp4 → .mlvc（neural 档位自动选 MLVC）
./.build/release/rkvc_transcode -i in.mp4 -o out.mlvc -p neural \
  --mlvc-enc models/mlvc/MLVCEncoder_rk3588.rknn \
  --mlvc-gaussian-pmf models/mlvc/gaussian.bin --mlvc-bitest-pmf models/mlvc/bitest.bin

# 纯解码：.mlvc → .yuv（NV12，无再编码）
./.build/release/rkvc_transcode -i out.mlvc -o out.yuv --mlvc-dec models/mlvc/MLVCDecoder_rk3588.rknn \
  --mlvc-gaussian-pmf models/mlvc/gaussian.bin --mlvc-bitest-pmf models/mlvc/bitest.bin

# 多 QP：基座模型 + 打开时打补丁（编码用 --mlvc-qp，解码用容器头 qp）
./.build/release/rkvc_transcode -i in.mp4 -o out.mlvc -p neural \
  --mlvc-enc models/mlvc/MLVCEncoder_rk3588.rknn \
  --mlvc-gaussian-pmf models/mlvc/gaussian.bin --mlvc-bitest-pmf models/mlvc/bitest.bin \
  --mlvc-qp-patch-dir models/mlvc/qp_patches --mlvc-qp 30
```

## API 示例

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

## RD 基准测试（tools/bench/）

端到端码率-画质与性能对比，默认四路：**H.264 / H.265 / SVT-AV1 / rkvc**，可选 `rkvc-neural`（MLVC）、`post-upscale`（下采样编码 + RGA / AI 超分）等路线。

![RD 曲线（1080p E2E）](docs/images/bench/rd_curve_e2e.png)

![E2E 性能对比](docs/images/bench/perf_e2e.png)

```bash
./tools/bench/run_rd_benchmark.sh /path/to/1080p.mp4
PLOT_ONLY=1 ./tools/bench/run_rd_benchmark.sh          # 仅重绘图表
RUN_CODECS=h264,rkvc ./tools/bench/run_rd_benchmark.sh clip.mp4
```

详见 [tools/bench/README.md](tools/bench/README.md)。

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
- libdrm；`librknnrt`（`rkvc_sr` 超分与 MLVC）：`./scripts/install-rknnrt.sh` 下载到 `.build/deps/rknn-install/`，可移植包自带
- MLVC 与 MLVC-S 各自使用自包含 bundle：`models/mlvc/`、`models/mlvc-s/`，目录内包含 RKNN、PMF、QP 补丁和 manifest。可用 `.venv/bin/python tools/mlvc/export_rknn.py --from-mlvc` 从 [microsoft/mlvc](https://github.com/microsoft/mlvc) 导出（见 [docs/mlvc-rknn-export.md](docs/mlvc-rknn-export.md)）；SR 模型 `models/rkvc_sr_x3.crypt.rknn` 为商业授权加密模型

## 许可证

本项目采用**双许可**模式（见 [LICENSE](LICENSE) 顶部声明）：

- **开源（默认，AGPLv3）**：源码树按 GNU Affero General Public License v3 提供。
  - 允许商用、修改、再分发，但**衍生/合并作品须以 AGPLv3 开源**；通过网络提供本程序服务的（含 SaaS）须向用户提供对应源码
  - AGPL 版（`RKVC_ENABLE_LICENSE=OFF`，默认）无任何附加授权限制
- **商业授权**：闭源商业使用需另行取得商业授权，授权范围含强制授权机制（`RKVC_ENABLE_LICENSE`）与加密模型（`models/rkvc_sr_x3.crypt.rknn`，明文模型与训练细节仅在商业授权内提供）
- `third_party/` 内各组件（ffmpeg-rockchip、SVT-AV1、mpp、librga、libsodium）版权归其各自所有者，适用各自许可证；分发包随附全部许可证文本、SVT-AV1 专利许可及对 ffmpeg-rockchip 的修改补丁（见 [docs/packaging.md](docs/packaging.md)）

## 文档

| 文档                                                   | 说明                             |
| ------------------------------------------------------ | -------------------------------- |
| [docs/index.md](docs/index.md)                         | 文档首页与导航                   |
| [docs/getting-started.md](docs/getting-started.md)     | 构建与首次运行                   |
| [docs/build-layout.md](docs/build-layout.md)           | 构建目录约定（全部在 `.build/`） |
| [docs/api.md](docs/api.md)                             | API 完整参考                     |
| [docs/architecture.md](docs/architecture.md)           | Session / Router / 节点架构      |
| [docs/benchmark.md](docs/benchmark.md)                 | 性能与 RD 基准                   |
| [docs/testing.md](docs/testing.md)                     | 测试矩阵                         |
| [docs/packaging.md](docs/packaging.md)                 | 可便携包与分发                   |
| [docs/delivery.md](docs/delivery.md)                   | 客户交付清单                     |
| [docs/mlvc-rknn-export.md](docs/mlvc-rknn-export.md)   | MLVC ONNX/PMF → RKNN 导出        |
| [docs/mlvc-npu-profile.md](docs/mlvc-npu-profile.md)   | MLVC NPU 算子级 profile          |
| [docs/sr-model-yuv-spec.md](docs/sr-model-yuv-spec.md) | YUV-native SR 设计稿             |
| [docs/release/](docs/release/)                         | 发布包用户文档                   |
| [CHANGELOG.md](CHANGELOG.md)                           | 版本变更记录                     |

版本：见 `CMakeLists.txt` `project(VERSION)` / `rkvc_version()`（Session + Codec Router API）
