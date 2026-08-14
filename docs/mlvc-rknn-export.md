# MLVC ONNX → RKNN 导出

把 [Microsoft MLVC](https://github.com/microsoft/mlvc) 的固定分辨率 ONNX 与 PMF JSON，转成 `rkvc` 运行时（`lib/node_mlvc.c`）使用的 `.rknn` + `PMF1` 二进制表。

本仓库**不 vendoring** 上游训练/转换代码。`python export_rknn.py --from-mlvc` 会浅克隆官方仓、下载公开 checkpoint、跑 `convert.py export --target-device generic`，再做图处理与 RKNN 转换。

微软仓与本工具用**两套 venv**：`convert.py` 要 Python 3.12（clone 目录 `.venv`）；rknn-toolkit2 用 `tools/mlvc/.venv`（Python 3.8–3.12）。

## 运行时 I/O 约定

C 侧不向 NPU 喂 qp（qp 只用于 rANS：`z_idx = qp * ZC + c`）。因此导出时必须把 `q_index_shifted` **折成常量**。

| 部件 | 输入（按下标） | 输出 |
| ---- | -------------- | ---- |
| 编码器 | `[0]` 图像 NHWC fp16、`[1]` `ref_feature` | 按名字匹配 `feature` / `z_raw` / `y_raw_0`；`y_raw_1` = `y_raw_0` 的下标 + 1 |
| 解码器 | `[0]` `z_raw`、`[1]` `y_raw_0`、`[2]` `y_raw_1`、最后一项 `ref_feature` | `[0]` `x_hat`、`[1]` `feature` |

官方 split `dmc61sbr_e1d1` / `dmc61sr_e1d1` 的 ONNX 还带 `q_index_shifted`（编码器 3 入、解码器 5 入）。折叠后才是上表的 2 / 4 输入。

## 依赖

图处理需要 `onnx`（及 numpy）。RKNN 转换需要 [rknn-toolkit2](https://github.com/airockchip/rknn-toolkit2)（PyPI 提供 x86_64 / aarch64 manylinux wheel，Python 3.8–3.12）。

```bash
cd tools/mlvc
uv sync          # Python 3.8–3.12；创建 .venv 并安装 rknn-toolkit2 / onnx / numpy / setuptools
```

aarch64 上请用 `uv sync`（`pyproject.toml` 已去掉 `onnxoptimizer`：该包无 ARM wheel，转换也不需要）。若直接 `pip install rknn-toolkit2` 因编译 `onnxoptimizer` 失败，改用：

```bash
uv pip install rknn-toolkit2 --no-deps
uv pip install -r requirements.txt
```

`do_quantization=False`，`float_dtype=float16`。不要套 YOLO 那套 `mean/std=255` 图像预处理。

## 上游 ONNX

一条命令走通（浅克隆 [microsoft/mlvc](https://github.com/microsoft/mlvc) 到 `.build/deps/mlvc`，下载公开 checkpoint，跑 `convert.py export --target-device generic`）：

```bash
cd tools/mlvc && uv sync
python export_rknn.py --from-mlvc --out-dir ../../models --platform rk3588 --qp 21
# 只要 ONNX：python export_onnx.py
```

或在已有 microsoft/mlvc 仓库里自行导出后再喂给本工具：

```bash
# 在 microsoft/mlvc 的 video/ 目录
python convert.py export --model-version dmc61sbr_reglu --model-type onnx \
    --target-device generic --model-width 640 --model-height 368
```

产物目录形如：

```
.../onnx-generic/640x368/
  MLVCEncoder.onnx
  MLVCDecoder.onnx
  gaussian_pmf.json
  bit_estimator_pmf.json
  metadata.json
```

`--target-device` 建议 `generic` 或 `intel`。**不要用 `qualcomm`**：官方 Qualcomm 优化会把 PixelUnshuffle 收成 `SpaceToDepth`、把 Clip 收成 `Max`，在 RKNN 上都会 CPU fallback（见 [mlvc-npu-profile.md](mlvc-npu-profile.md) §7.2）。即便 generic 默认 pass 里仍可能出现 `SpaceToDepth`，本工具会再写回 NPU 友好算子。

公开 checkpoint（约 70 MB）落到 `.build/deps/mlvc-data/pretrained/mlvc-psnr-v1.ckpt`。tracing 用占位 I420（640×360 灰帧；模型 640×368 由上游 EDGE padding）。`convert.py` 默认还会做导出后校验，本包装器用 `--no-validate-conversion` 跳过。

## 本仓库转换

```bash
python3 tools/mlvc/export_rknn.py \
    --onnx-dir /path/to/onnx-generic/640x368 \
    --out-dir models \
    --platform rk3588 \
    --qp 21
```

常用选项：

| 选项 | 说明 |
| ---- | ---- |
| `--qp 21` | 折进图的 `q_index`（默认 21，与 `node_mlvc` 一致） |
| `--qp-list 10,21,30,40` | 每个 qp 一份模型，写入 `models/{platform}_qp_models/qpXX/`；`--qp` 若在列表中则再拷到 `models/` 根目录作默认 |
| `--skip-rknn` | 只做 PMF + ONNX 折叠/重写（无 toolkit 的 CI / 板端可用） |
| `--pmf-only` | 只把 JSON 写成 `gaussian.bin` / `bitest.bin` |
| `--inspect` | 只打印 ONNX I/O |
| `--no-rewrite` | 不做 SpaceToDepth / Max / Div 替换 |
| `--no-fold-qp` | 保留 `q_index` 输入（C 运行时目前不能喂） |
| `--keep-onnx` | 在输出目录保留处理后的 ONNX |
| `--platform rk3588` | RKNN `target_platform` |
| `--from-mlvc` | 浅克隆 microsoft/mlvc 并先导出 ONNX（权重与占位 YUV 在 `.build/deps/mlvc-data`） |
| `--mlvc-dir` / `--weights-path` | 覆盖上游源码或 checkpoint 路径 |
| `--onnx-frame-count` | `convert.py` tracing 帧数（默认 2；官方默认 48） |
| `--patch-dir DIR` | QPP1 输出目录（默认 `{out-dir}/qp_patches`） |

`--onnx-dir` 也可指向更上层目录，工具会递归查找 `MLVCEncoder.onnx` 等文件名。

## 图重写

对齐 NPU profile 的 CPU fallback：

| 原算子 | 替换 | 说明 |
| ------ | ---- | ---- |
| `SpaceToDepth` | `Reshape` + `Transpose(perm=[0,3,5,1,2,4])` + `Reshape` | 与 ONNX 规范等价 |
| `Max(x, const)` / `Min(x, const)` | `Clip` | RKNN 上 Max 走 CPU，Clip 可上 NPU |
| `Div(x, const)` | `Mul(x, 1/const)` | |

需要固定 NCHW 形状才能展开 SpaceToDepth；动态维会跳过并打日志。

## PMF1

JSON 字段与上游 `GaussianCoderPmf` / `BitEstimatorPmf` 一致：`pmf_lengths` / `pmf_offsets` / `pmf_table`，gaussian 另有 `scale_min` / `scale_max` / `scale_levels` / `index_space`，bitest 另有 `qp_num` / `channels`。

二进制布局见 `lib/node_mlvc.c` `load_pmf()`。`node_mlvc.c` **要求** gaussian 的 `index_space=1`。

## 上板

```bash
./.build/release/rkvc_transcode -i in.mp4 -o out.mlvc -p neural \
  --mlvc-enc models/MLVCEncoder_rk3588.rknn \
  --mlvc-dec models/MLVCDecoder_rk3588.rknn \
  --mlvc-gaussian-pmf models/gaussian.bin \
  --mlvc-bitest-pmf models/bitest.bin \
  --mlvc-qp 21
```

分辨率须与导出时 `--model-width` / `--model-height` 一致（现网 640×368）。`--mlvc-qp` 须与折叠进图的 `--qp` 一致，否则 rANS 码表与 NPU 嵌入错位。

## 多 QP 单模型（QPP1）

各 qp 折叠进图后，RKNN 权重局部不同、文件大小相同。不必在运行时切换整份 `.rknn`：保留一份基座（默认 `MLVCEncoder_rk3588.rknn` / `MLVCDecoder_rk3588.rknn`），打开时按 qp 打一次二进制补丁。

```bash
python3 tools/mlvc/export_rknn.py \
    --onnx-dir /path/to/onnx-generic/640x368 \
    --out-dir models --platform rk3588 \
    --qp 21 --qp-list 10,21,30,40

# 或对已有 qpXX/*.rknn 目录单独生成：
python3 tools/mlvc/make_qp_patches.py \
    --models-dir models/rk3588_qp_models --base-qp 21 --out-dir models/qp_patches
```

产物：`models/qp_patches/{enc|dec}_qp{N}.qppatch`（含基座 qp 的空补丁）。格式为 48 字节小端头 `QPP1` + 合并后的 `(offset, length)` 区间 + payload；头里带基座 / payload CRC32。缺补丁或 CRC 不对会打开失败，不会静默用错权重。

```bash
./.build/release/rkvc_transcode -i in.mp4 -o out.mlvc -p neural \
  --mlvc-enc models/MLVCEncoder_rk3588.rknn \
  --mlvc-gaussian-pmf models/gaussian.bin \
  --mlvc-bitest-pmf models/bitest.bin \
  --mlvc-qp-patch-dir models/qp_patches \
  --mlvc-qp 30
```

编码用 `--mlvc-qp` 选 `enc_qpN.qppatch`；解码用容器头里的 qp 选 `dec_qpN.qppatch`。不传 `--mlvc-qp-patch-dir` 时行为与单模型相同（基座须已是该 qp）。不做运行时热切换。
