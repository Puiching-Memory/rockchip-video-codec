# RD 基准测试（bench/）

RK3588 端到端 **码率-画质（RD）** 与 **性能** 对比框架，已集成到 rkvc 项目。

## 对比路线（默认）

| codec 名                  | 方案                                                                                            |
| ------------------------- | ----------------------------------------------------------------------------------------------- |
| `h264`                    | FFmpeg `h264_rkmpp` 硬编硬解                                                                    |
| `h265`                    | FFmpeg `hevc_rkmpp` 硬编硬解                                                                    |
| `svt-av1`                 | SVT-AV1 编 + `av1_rkmpp` 硬解（默认 preset 11，偏实时）                                         |
| `svt-av1-hq`              | **非实时高质量**：SVT-AV1 更慢 preset（默认 `hq_preset=4`）+ `av1_rkmpp`；目标编码 ≥1 fps@1080p |
| `rkvc`                    | **rkvc Session** 四档语义（`RKVC_POLICIES` 展开）                                               |
| `rkvc-realtime`           | Session `realtime` → H.264 RKMPP                                                                |
| `rkvc-balanced`           | Session `balanced` → HEVC RKMPP                                                                 |
| `rkvc-quality`            | Session `quality` → SVT-AV1 p11 + av1_rkmpp（与 `svt-av1` 基线同 preset）                       |
| `rkvc-offline`            | Session `offline` → SVT-AV1 p4 + av1_rkmpp（非实时高质量，与 `svt-av1-hq` 对齐）                |
| `rkvc-neural`             | Session `neural` → MLVC 神经编解码（NPU + rANS，固定 640×368，qp 参数化）                       |
| `post-upscale`            | **下采样编码 + 上采样后处理**（RGA 插值或 `rkvc_sr`；与 Session 解码路径一致）                  |
| `svt-av1+up3x-bilinear`   | 单算法路线（`ENC_SCALE_DENOM=3` 时 CSV 名为 `svt-av1+up{N}x-{algo}`）                           |
| `svt-av1+up3x-rkvc_sr`    | 同上，AI 超分（需 `RKVC_SR_MODEL` / `paths.rkvc_sr_model`）                                     |
| `svt-av1-hq+up3x-rkvc_sr` | HQ preset 下采样编码 + AI 超分还原                                                              |
| `svt-av1+superres`        | **实验 / 搁置**：SVT-AV1 + AV1 内建 superres（见下节）                                          |

## AV1 内建 superres（实验，搁置）

SVT-AV1 开启 `--superres-mode` 等参数，在编码器内部做低分辨率编码 + 规范上采样。RD 图中 codec 名为 `svt-av1+superres`（虚线，深绿六边形标记）。

**状态：搁置。** `av1_rkmpp` 硬解 superres 码流时，`hwdownload` 会因显示宽（1920）与 DMA stride（编码宽 ~1182）不一致而崩溃；待 MPP/ffmpeg-rkmpp 修复前，显式启用本路线时自动改用系统 `libaom-av1` 软解（慢，但可跑通 RD）。

```bash
# 显式启用（不在默认 RUN_CODECS 中）
RUN_CODECS=svt-av1,svt-av1+superres ./bench/run_rd_benchmark.sh /path/to/1080p.mp4

# superres 参数（可选）
SVT_SUPERRES_MODE=4          # 默认 auto；1=fixed 3=qthresh
SVT_SUPERRES_DENOM=9         # fixed 时分母 9~16（8/9 ~ 8/16 水平缩放）
SVT_SUPERRES_FFMPEG=/path/to/ffmpeg-with-libaom   # 须在 config.json paths.superres_decode_ffmpeg 配置
```

## 下采样 + 后处理上采样

模拟「低分辨率编码 → 解码 → RGA / NN 上采样」管线。bench 的 post-upscale 与 **Session 解码产品路径**一致（`rkvc_session_upscale`：RKMPP 硬解 → RGA 或 `rkvc_sr`）：

```bash
# rkvc_session_upscale：硬解 + 后处理上采样（bench 内自动调用）
.build/release/rkvc_session_upscale -i stream.mp4 -o out.nv12 \
  --width 1920 --height 1080 --enc-scale-denom 3 --post-upscale bilinear --print-timing

# AI 超分（需模型文件）
.build/release/rkvc_session_upscale -i stream.mp4 -o out.nv12 \
  --width 1920 --height 1080 --enc-scale-denom 3 \
  --post-upscale rkvc_sr --rkvc-sr-model models/rkvc_sr_x3.crypt.rknn --print-timing
```

下采样参考帧仍用 `rkvc_yuv_upscale`（仅 prep 阶段）。

```bash
# 仅跑 post-upscale 路线（对比 SVT-AV1 全分辨率基线）
RUN_CODECS=svt-av1,post-upscale ./bench/run_rd_benchmark.sh /path/to/1080p.mp4

# 3× 下采样 + RGA 三档插值
ENC_SCALE_DENOM=3 UPSCALE_ALGOS=nearest,bilinear,bicubic \
  RUN_CODECS=svt-av1,post-upscale ./bench/run_rd_benchmark.sh /path/to/1080p.mp4

# 含 AI 超分（默认 config 的 upscale_algos 不含 rkvc_sr，需显式打开）
ENC_SCALE_DENOM=3 UPSCALE_ALGOS=bilinear,rkvc_sr \
  RUN_CODECS=svt-av1,post-upscale ./bench/run_rd_benchmark.sh /path/to/1080p.mp4
```

管线：`REF → 1/N 下采样 → 编码 → 硬解 → 上采样 → 与全分辨率 REF 比 PSNR/SSIM`。

Session 字段：`enc_scale_denom`、`post_upscale_algo`。**编码 CLI**（`rkvc_encode`）只做下采样；上采样请用 `rkvc_session_upscale` 或 decode 模板。

## 快速开始

```bash
# 1. 构建依赖与 rkvc
./scripts/build-svt.sh
./scripts/rebuild-ffmpeg-rkmpp.sh
cmake -B .build/release -DCMAKE_BUILD_TYPE=Release
cmake --build .build/release -j4

# 2. （可选）绘图 Python 环境
cd bench && uv sync    # 或 pip install matplotlib numpy

# 3. 跑基准（默认从源视频中间截取 4s，17 个码率点 25–1000 kbps）
./bench/run_rd_benchmark.sh /path/to/1080p.mp4

# 仅重绘图表
PLOT_ONLY=1 ./bench/run_rd_benchmark.sh
```

## 输出

| 路径                             | 说明                                    |
| -------------------------------- | --------------------------------------- |
| `bench/results/rd_data.csv`      | 原始数据（**默认仅含本次 RUN_CODECS**） |
| `bench/results/session.meta`     | 本次跑分元数据（码率点、clip、模式等）  |
| `bench/results/session.codecs`   | 本次 CSV 中的 codec 列表                |
| `bench/results/rd_curve_e2e.png` | RD 曲线（横轴 log）                     |
| `bench/results/perf_e2e.png`     | E2E 性能对比                            |
| `bench/work/`                    | 中间文件（可删）                        |

## 配置（bench/config.json）

基准参数、路径、RD 校准表集中在 `bench/config.json`，**不再依赖系统 `/usr/bin/ffmpeg`**，也不再在 shell 里硬编码 QP/CRF 表。

```bash
# 校验配置与项目 ffmpeg 是否就绪
python3 bench/tools/config.py validate bench/config.json

# 查看将生效的默认值
python3 bench/tools/config.py defaults bench/config.json /path/to/rockchip-video-codec

# 单次覆盖（环境变量优先于 config）
RUN_CODECS=svt-av1,post-upscale ENC_SCALE_DENOM=3 \
  ./bench/run_rd_benchmark.sh /path/to/1080p.mp4

# 使用自定义配置
BENCH_CONFIG=bench/my_config.json ./bench/run_rd_benchmark.sh clip.mp4
```

主要配置项：

| 节点                                               | 说明                                                         |
| -------------------------------------------------- | ------------------------------------------------------------ |
| `paths.ffmpeg` / `ffprobe`                         | 项目 `ffmpeg-rockchip` 构建产物                              |
| `target_kbps`                                      | RD 扫点码率列表                                              |
| `calibration.*`                                    | h264/h265/SVT CQP/CRF 校准表                                 |
| `run.codecs` / `enc_scale_denom` / `upscale_algos` | 对比路线（含 `svt-av1-hq`）                                  |
| `svt.preset` / `svt.hq_preset`                     | 实时档 / 非实时高质量档 SVT preset（默认 11 / 4）            |
| `svt.superres.enabled`                             | 默认 `false`（需另配 `paths.superres_decode_ffmpeg` 才启用） |

环境变量仍可覆盖 config 中的任意默认值（在调用脚本前 `export`）。

## 环境变量（覆盖 config）

- `BENCH_CONFIG` — 配置文件路径（默认 `bench/config.json`）
- `RUN_CODECS` — 见 `config.json` → `run.codecs`
- `TARGET_KBPS` — 见 `config.json` → `target_kbps`
- `SVT_RD_MODE` — SVT-AV1 RD 扫点：`calibrated`（默认，CRF/CQP 校准表）或 `vbr`（`--rc 1 --tbr`）
- `ENC_SCALE_DENOM` — post-upscale 编码下采样分母（默认 `2`）
- `UPSCALE_ALGOS` — 上采样算法，逗号分隔（默认 `nearest,bilinear,bicubic`；可加 `rkvc_sr`）
- `RKVC_SR_MODEL` — `rkvc_sr` 模型路径（默认见 `config.json` → `paths.rkvc_sr_model`）
- `RKVC_POLICIES` — rkvc 语义档位，默认 `realtime,balanced,quality,offline,neural`
- `MLVC_ENC_MODEL` / `MLVC_DEC_MODEL` — MLVC 编/解码 RKNN 模型路径（默认 `config.json` → `mlvc.*_model`，`{soc}` 占位符按探测到的 SoC 展开，如 rk3588）
- `MLVC_GAUSSIAN_PMF` / `MLVC_BITEST_PMF` — MLVC PMF 表路径（默认 `models/gaussian.bin` / `models/bitest.bin`）
- `MLVC_QP` — MLVC 质量参数（默认 21；`rkvc-neural` 不参与码率扫描，仅用此 qp）
- `CLIP_SEC` — 截取秒数（默认 `4`）
- `CLIP_OFFSET` — 截取位置：`middle`（默认，居中）| `start`
- `CLIP_START_SEC` — 显式起点秒数（设置后覆盖 `CLIP_OFFSET`）
- `RKVC_BUILD` — rkvc 构建目录（含 `rkvc_transcode`）
- `SVT_PRESET` — SVT 编码 preset（默认 11，用于 `svt-av1` / post-upscale / rkvc-quality）
- `SVT_HQ_PRESET` — 非实时高质量档 preset（默认 4，用于 `svt-av1-hq`；本机 1080p 约 ~2 fps，低于 1 fps 的 p2/p3 不采用）
- `RAMDISK_DIR` — YUV 放 tmpfs，减少 I/O 干扰
- `BENCH_CSV_MODE` — `session`（**默认**，`rd_data.csv` 只保留本次跑出的 codec）| `accumulate`（增量合并，未重跑的 codec 保留旧行）

### CSV 合并模式

默认 **`session`**：`finalize_csv` 只用本次 `bench/work/results_*.csv` 重写 `rd_data.csv`，不会把上次 h264/h265 等历史路线混进来。

若需多次跑分累积到同一张表（旧行为），显式设置：

```bash
BENCH_CSV_MODE=accumulate ./bench/run_rd_benchmark.sh clip.mp4
```

## 单独绘图

```bash
cd bench
python3 plot_rd_curve.py --csv results/rd_data.csv
python3 plot_perf.py --csv results/rd_data.csv --frames 62
```

详细结论见 [REPORT.md](REPORT.md)。
