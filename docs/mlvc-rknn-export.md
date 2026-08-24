# MLVC ONNX → RKNN 导出

把 [Microsoft MLVC](https://github.com/microsoft/mlvc) 的固定分辨率 ONNX 与 PMF JSON，转成 `rkvc` 运行时（`lib/node_mlvc.c`）使用的 `.rknn` + `PMF1` 二进制表。

本仓库**不 vendoring** 上游训练/转换代码。`.venv/bin/python tools/mlvc/export_rknn.py --from-mlvc` 会浅克隆官方仓、下载公开 checkpoint、跑 `convert.py export --target-device generic`，再做图处理与 RKNN 转换。

全仓库只有一个 Python 环境：仓库根目录 `.venv`（`pyproject.toml` + `uv.lock` **固定版本号**）。`convert.py` 与 rknn-toolkit2 共用该环境。官方 MLVC 声明 `torch==2.10.0`，但 rknn-toolkit2 2.3.2 要求 `torch<=2.4.0`，且本板 A72 上 2.10 会 SIGILL，因此锁在 **torch==2.2.2**（CPU wheel）+ **scipy==1.11.4**（与 numpy 1.26.4 兼容）。不要在 `tools/` 或 `.build/deps/mlvc/` 再 `uv sync`。

## 运行时 I/O 约定

C 侧不向 NPU 喂 qp（qp 只用于 rANS：`z_idx = qp * ZC + c`）。因此导出时必须把 `q_index_shifted` **折成常量**。

| 部件   | 输入                                                 | 输出                                                |
| ------ | ---------------------------------------------------- | --------------------------------------------------- |
| 编码器 | 按名字：图像（`x` / `New_input_x`）、`ref_feature`   | 按名字：`feature` / `z_raw` / `y_raw_0` / `y_raw_1` |
| 解码器 | 按名字：`z_raw`、`y_raw_0`、`y_raw_1`、`ref_feature` | 按名字：`x_hat`、`feature`                          |

编码器的保底路径成对使用 `rknn_inputs_set()` / `rknn_outputs_get()`：
输入按 NHWC 交给 runtime，输出按逻辑 NCHW 读取，再把 `feature` 转成
下一帧 NHWC reference。默认的高性能路径只把输入换成 `rknn_set_io_mem()`，
输出仍保持 `rknn_outputs_get()` 的逻辑 NCHW 契约。
不要把 native feature 输出直接复制到下一帧 native `ref_feature` 输入：即使查询
到的元素数与 NC1HWC2 维度相同，这个图的 producer/consumer native layout 也不具备
可直接互换的契约；标准 MLVC 的 256 通道 reference 会在第二帧被错误解释并产生
NaN/Inf。解码器的四输入/两输出 native 布局已单独验证，可继续使用零拷贝路径。

encoder 混合 I/O 按查询到的 native input attr（含 C2 和 `w_stride`）
把逻辑 `feature` 直接打包进下一帧 reference I/O memory，不会恢复错误的
native-output `memcpy`。RK3576 上 MLVC/MLVC-S 各 70 帧、3 轮递归已与标准
I/O 码流逐字节一致，因此该路径默认开启。设置
`RKVC_MLVC_ENCODER_ZERO_COPY=0` 可回退到标准 host I/O；若 native attr 查询或
几何校验不通过，运行时也会自动回退。

默认把解码器尾部 `DepthToSpace(mode=DCR)+Clip(0,1)` 拆出图外（`--no-extract-tail` 关闭）。此时 RKNN 的 `x_hat` 是 shuffle 前的 head conv（640×368 时为 `[1,192,46,80]`），`node_mlvc.c` 按 native 通道数自动做 CPU DCR + clip；旧的整图 `x_hat=[1,3,H,W]` 模型不用改。

官方 split `dmc61sbr_e1d1` / `dmc61sr_e1d1` 的 ONNX 还带 `q_index_shifted`（编码器 3 入、解码器 5 入）。折叠后才是上表的 2 / 4 输入。

## 依赖

图处理需要 `onnx`（及 numpy）。RKNN 转换需要 [rknn-toolkit2](https://github.com/airockchip/rknn-toolkit2)（PyPI 提供 x86_64 / aarch64 manylinux wheel，Python 3.8–3.12）。

```bash
uv sync          # 仓库根目录；Python 3.12，版本见 pyproject.toml
```

请使用仓库根目录 `pyproject.toml` + `uv.lock` 安装，不要直接 `pip install rknn-toolkit2`。配置已去掉无 ARM wheel、且转换不需要的 `onnxoptimizer`，并把 torch / torchvision 绑定到 PyTorch CPU-only index。

`do_quantization=False`，`float_dtype=float16`。不要套 YOLO 那套 `mean/std=255` 图像预处理。

## 上游 ONNX

一条命令走通（浅克隆 [microsoft/mlvc](https://github.com/microsoft/mlvc) 到 `.build/deps/mlvc`，下载公开 checkpoint，跑 `convert.py export --target-device generic`）：

```bash
uv sync
.venv/bin/python tools/mlvc/export_rknn.py \
  --from-mlvc --out-dir models/mlvc --platform rk3588 --qp 21
# 只要 ONNX：.venv/bin/python tools/mlvc/export_onnx.py
```

或在已有 microsoft/mlvc 仓库里自行导出后再喂给本工具：

```bash
# 在 microsoft/mlvc 的 video/ 目录
python convert.py export --model-version dmc61sbr_reglu --model-type onnx \
    --target-device generic --model-width 640 --model-height 368
```

MLVC-S 使用单独的 checkpoint，工具不会拿普通 MLVC 权重代替。导出时必须显式指定：

```bash
.venv/bin/python tools/mlvc/export_rknn.py --from-mlvc \
  --model-version dmc61sbr_reglu_s \
  --weights-path /path/to/mlvc-s-psnr-v1.ckpt \
  --out-dir models/mlvc-s --platform rk3576
```

两种变体使用平行、互不混用的 bundle 目录：

```text
models/
├── mlvc/
│   ├── MLVCEncoder_<soc>.rknn
│   ├── MLVCDecoder_<soc>.rknn
│   ├── gaussian.bin
│   ├── bitest.bin
│   ├── qp_patches/
│   └── mlvc_rknn_export_manifest.json
└── mlvc-s/
    ├── MLVCEncoder_<soc>.rknn
    ├── MLVCDecoder_<soc>.rknn
    ├── gaussian.bin
    ├── bitest.bin
    ├── qp_patches/
    └── mlvc_rknn_export_manifest.json
```

省略 `--out-dir` 时，工具会按 `--model-version` 自动选择 `models/mlvc/` 或 `models/mlvc-s/`。

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
.venv/bin/python tools/mlvc/export_rknn.py \
    --onnx-dir /path/to/onnx-generic/640x368 \
    --out-dir models/mlvc \
    --platform rk3588 \
    --qp 21
```

常用选项：

| 选项                            | 说明                                                                                                         |
| ------------------------------- | ------------------------------------------------------------------------------------------------------------ |
| `--qp 21`                       | 折进图的 `q_index`（默认 21，与 `node_mlvc` 一致）                                                           |
| `--qp-list 10,21,30,40`         | 每个 qp 一份模型，写入 `{out-dir}/{platform}_qp_models/qpXX/`；`--qp` 若在列表中则再拷到 bundle 根目录作默认 |
| `--skip-rknn`                   | 只做 PMF + ONNX 折叠/重写（无 toolkit 的 CI / 板端可用）                                                     |
| `--pmf-only`                    | 只把 JSON 写成 `gaussian.bin` / `bitest.bin`                                                                 |
| `--inspect`                     | 只打印 ONNX I/O                                                                                              |
| `--no-rewrite`                  | 不做 SpaceToDepth / Max / Div 替换                                                                           |
| `--no-extract-tail`             | 保留解码器尾部 DepthToSpace+Clip 在图内（默认拆到 CPU）                                                      |
| `--no-fold-qp`                  | 保留 `q_index` 输入（C 运行时目前不能喂）                                                                    |
| `--keep-onnx`                   | 在输出目录保留处理后的 ONNX                                                                                  |
| `--platform rk3588`             | RKNN `target_platform`                                                                                       |
| `--from-mlvc`                   | 浅克隆 microsoft/mlvc 并先导出 ONNX（权重与占位 YUV 在 `.build/deps/mlvc-data`）                             |
| `--mlvc-dir` / `--weights-path` | 覆盖上游源码或 checkpoint 路径                                                                               |
| `--onnx-frame-count`            | `convert.py` tracing 帧数（默认 2；官方默认 48）                                                             |
| `--patch-dir DIR`               | QPP1 输出目录（默认 `{out-dir}/qp_patches`）                                                                 |

`--onnx-dir` 也可指向更上层目录，工具会递归查找 `MLVCEncoder.onnx` 等文件名。

## 图重写

对齐 NPU profile 的 CPU fallback：

| 原算子                            | 替换                                                    | 说明                              |
| --------------------------------- | ------------------------------------------------------- | --------------------------------- |
| `SpaceToDepth`                    | `Reshape` + `Transpose(perm=[0,3,5,1,2,4])` + `Reshape` | 与 ONNX 规范等价                  |
| `Max(x, const)` / `Min(x, const)` | `Clip`                                                  | RKNN 上 Max 走 CPU，Clip 可上 NPU |
| `Div(x, const)`                   | `Mul(x, 1/const)`                                       |                                   |

需要固定 NCHW 形状才能展开 SpaceToDepth；动态维会跳过并打日志。

## PMF1

JSON 字段与上游 `GaussianCoderPmf` / `BitEstimatorPmf` 一致：`pmf_lengths` / `pmf_offsets` / `pmf_table`，gaussian 另有 `scale_min` / `scale_max` / `scale_levels` / `index_space`，bitest 另有 `qp_num` / `channels`。

二进制布局见 `lib/node_mlvc.c` `load_pmf()`。`node_mlvc.c` **要求** gaussian 的 `index_space=1`。

## 上板

```bash
./.build/release/rkvc_transcode -i in.mp4 -o out.mlvc -p neural \
  --mlvc-enc models/mlvc/MLVCEncoder_rk3588.rknn \
  --mlvc-dec models/mlvc/MLVCDecoder_rk3588.rknn \
  --mlvc-gaussian-pmf models/mlvc/gaussian.bin \
  --mlvc-bitest-pmf models/mlvc/bitest.bin \
  --mlvc-qp 21
```

分辨率须与导出时 `--model-width` / `--model-height` 一致（现网 640×368）。`--mlvc-qp` 须与折叠进图的 `--qp` 一致，否则 rANS 码表与 NPU 嵌入错位。

## 多 QP 单模型（QPP1）

各 qp 折叠进图后，RKNN 权重局部不同、文件大小相同。不必在运行时切换整份 `.rknn`：在对应 bundle 内保留一份基座（默认 `MLVCEncoder_rk3588.rknn` / `MLVCDecoder_rk3588.rknn`），打开时按 qp 打一次二进制补丁。

```bash
.venv/bin/python tools/mlvc/export_rknn.py \
    --onnx-dir /path/to/onnx-generic/640x368 \
    --out-dir models/mlvc --platform rk3588 \
    --qp 21 --qp-list 10,21,30,40

# 或对已有 qpXX/*.rknn 目录单独生成：
.venv/bin/python tools/mlvc/make_qp_patches.py \
    --models-dir models/mlvc/rk3588_qp_models --base-qp 21 --out-dir models/mlvc/qp_patches
```

产物：`models/mlvc/qp_patches/{enc|dec}_qp{N}.qppatch`（含基座 qp 的空补丁）。格式为 48 字节小端头 `QPP1` + 合并后的 `(offset, length)` 区间 + payload；头里带基座 / payload CRC32。缺补丁或 CRC 不对会打开失败，不会静默用错权重。

```bash
./.build/release/rkvc_transcode -i in.mp4 -o out.mlvc -p neural \
  --mlvc-enc models/mlvc/MLVCEncoder_rk3588.rknn \
  --mlvc-gaussian-pmf models/mlvc/gaussian.bin \
  --mlvc-bitest-pmf models/mlvc/bitest.bin \
  --mlvc-qp-patch-dir models/mlvc/qp_patches \
  --mlvc-qp 30
```

编码用 `--mlvc-qp` 选 `enc_qpN.qppatch`；解码用容器头里的 qp 选 `dec_qpN.qppatch`。不传 `--mlvc-qp-patch-dir` 时行为与单模型相同（基座须已是该 qp）。不做运行时热切换。
