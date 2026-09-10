# MLVC ONNX → RKNN 导出

把 [Microsoft MLVC](https://github.com/microsoft/mlvc) 的固定分辨率 ONNX 与 PMF JSON 转成 `.rknn` + `PMF1` 二进制表。本文只描述模型生产格式。

本仓库**不 vendoring** 上游训练/转换代码。`.venv/bin/python tools/mlvc/export_rknn.py --from-mlvc` 会浅克隆官方仓、下载公开 checkpoint、跑 `convert.py export --target-device generic`，再做图处理与 RKNN 转换。

全仓库只有一个 Python 环境：仓库根目录 `.venv`（`pyproject.toml` + `uv.lock` **固定版本号**）。`convert.py` 与 rknn-toolkit2 共用该环境。官方 MLVC 声明 `torch==2.10.0`，但 rknn-toolkit2 2.3.2 要求 `torch<=2.4.0`，且本板 A72 上 2.10 会 SIGILL，因此锁在 **torch==2.2.2**（CPU wheel）+ **scipy==1.11.4**（与 numpy 1.26.4 兼容）。不要在 `tools/` 或 `.build/deps/mlvc/` 再 `uv sync`。

## 运行时 I/O 约定

C 侧不向 NPU 喂 qp（qp 只用于 rANS：`z_idx = qp * ZC + c`）。因此导出时必须把 `q_index_shifted` **折成常量**。

| 部件   | 输入                                                 | 输出                                                |
| ------ | ---------------------------------------------------- | --------------------------------------------------- |
| 编码器 | 按名字：图像（`x` / `New_input_x`）、`ref_feature`   | 按名字：`feature` / `z_raw` / `y_raw_0` / `y_raw_1` |
| 解码器 | 按名字：`z_raw`、`y_raw_0`、`y_raw_1`、`ref_feature` | 按名字：`x_hat`、`feature`                          |

当前 C++ 后端只走 host I/O（`codecs/mlvc/src/npu_rknn.cpp`）：输入用
`rknn_inputs_set()` 按 NHWC 喂，输出用 `rknn_outputs_get()` 按逻辑 NCHW 读，
再把 `feature` 转成下一帧 NHWC reference；解码器同为输入 NHWC、输出逻辑 NCHW。
不要把 native feature 输出直接复制到下一帧 native `ref_feature` 输入：即使查询
到的元素数与 NC1HWC2 维度相同，这个图的 producer/consumer native layout 也不具备
可直接互换的契约；标准 MLVC 的 256 通道 reference 会在第二帧被错误解释并产生
NaN/Inf。旧 C 树的 `rknn_set_io_mem()` 混合 I/O 与 `RKVC_MLVC_ENCODER_ZERO_COPY`
回退开关已随 C++ 重写删除。

默认把解码器尾部 `DepthToSpace(mode=DCR)+Clip(0,1)` 拆出图外（`--no-extract-tail` 关闭）。此时 RKNN 的 `x_hat` 是 shuffle 前的 head conv（640×368 时为 `[1,192,46,80]`），解码节点按 native 通道数自动做 CPU DCR + clip；旧的整图 `x_hat=[1,3,H,W]` 模型不用改。

官方 split `dmc61sbr_e1d1` / `dmc61sr_e1d1` 的 ONNX 还带 `q_index_shifted`（编码器 3 入、解码器 5 入）。折叠后才是上表的 2 / 4 输入。

## 依赖

图处理需要 `onnx`（及 numpy）。RKNN 转换需要 [rknn-toolkit2](https://github.com/airockchip/rknn-toolkit2)（PyPI 提供 x86_64 / aarch64 manylinux wheel，Python 3.8–3.12）。

```bash
uv sync          # 仓库根目录；Python 3.12，版本见 pyproject.toml
```

请使用仓库根目录 `pyproject.toml` + `uv.lock` 安装，不要直接 `pip install rknn-toolkit2`。配置已去掉无 ARM wheel、且转换不需要的 `onnxoptimizer`，并把 torch / torchvision 绑定到 PyTorch CPU-only index。

`do_quantization=False`，`float_dtype=float16`。MLVC 不支持 INT8 量化；转换脚本会在
Toolkit 无法显式接受 FP16 时直接失败，不会删掉精度参数继续导出。
运行时同样拒绝任何非 FP16 或带量化标记的模型 I/O。不要套 YOLO 那套
`mean/std=255` 图像预处理。

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

MLVC-S 使用单独的 checkpoint，工具不会拿普通 MLVC 权重代替。权重与标准版同在
`mlvideopub` 公开容器（匿名可读），缺省时自动下载并校验 SHA-256；该地址属上游非承诺资源，失效时可手动放置
`.build/deps/mlvc-data/pretrained/mlvc-s-psnr-v1.ckpt` 或用 `--weights-path` 显式指定：

```bash
.venv/bin/python tools/mlvc/export_rknn.py --from-mlvc \
  --model-version dmc61sbr_reglu_s \
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

多目标板：每次调用只转换一个 `--platform`；基座文件名带平台后缀
（`MLVC{Encoder,Decoder}_<soc>.rknn`），多个平台的模型可在同一 bundle 内并存，
ONNX 导出只需一次，后续用 `--onnx-dir` 复用。QP 补丁经 `--pack`
直接进 `.rkmodel`（多 rung 共存一文件），运行时只需 `--model-dir`
+ `--model-id`，不再需要单独传补丁目录。

产物目录形如：

```
.../onnx-generic/640x368/
  MLVCEncoder.onnx
  MLVCDecoder.onnx
  gaussian_pmf.json
  bit_estimator_pmf.json
  metadata.json
```

`--target-device` 建议 `generic` 或 `intel`。**不要用 `qualcomm`**：官方 Qualcomm 优化会把 PixelUnshuffle 收成 `SpaceToDepth`、把 Clip 收成 `Max`，在 RKNN 上都会 CPU fallback。即便 generic 默认 pass 里仍可能出现 `SpaceToDepth`，本工具会再写回 NPU 友好算子（替换项见 `tools/mlvc/onnx_rewrite.py` 头部）。

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
| `--qp 21`                       | 折进图的 `q_index`（默认 21）                                                                                |
| `--qp-list 10,21,30,40`         | 每个 qp 一份模型，写入 `{out-dir}/{platform}_qp_models/qpXX/`；`--qp` 若在列表中则再拷到 bundle 根目录作默认 |
| `--qp-dynamic`                  | 全 QP 单模型（QPT1）：Gather 改 q 行输入，q 表外置随 `.rkmodel` 分发；与 `--qp-list`/QPP1 互斥               |
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

## 打包与装载（RKMDL1）

`export_rknn.py` 默认一步到位打成 C++ 可直接装载的模型文件
（`--no-pack` 关闭；`--skip-rknn`/`--pmf-only` 下无 `.rknn` 则跳过）：

```text
<out-dir>/mlvc_<soc>_qp<base>_encoder.rkmodel
<out-dir>/mlvc_<soc>_qp<base>_decoder.rkmodel
<out-dir>/mlvc_<soc>_dynq_encoder.rkmodel      # --qp-dynamic 时
```

每份含 `rknn` + `pmf-gaussian` + `pmf-bitest`，再加全部
`qppatch`（多 rung 共存一文件）或 `qptab`（dynq）。
meta 的 `id` 即文件名 stem（如 `mlvc_rk3576_qp21_encoder`），
`target` 为 `--platform`。版式见 `tools/mlvc/rkmdl1.py`
（与 `core/src/rkmodel.cpp` 逐字节对齐；`pack`/`verify` 子命令
可独立打包校验任意载荷）。

装载（CLI 会话按 `--model-id` 选模型，不填则取首个）：

```bash
rkvc encode --codec mlvc --model-dir models/mlvc --model-id mlvc_rk3576_qp21_encoder ...
# 或逐个指定：--model a_encoder.rkmodel --model a_decoder.rkmodel
```

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

二进制布局由 `tools/mlvc/pmf.py` 定义；gaussian 的 `index_space` 必须为 1。

## 上板

模型可继续导出和校验。

分辨率须与导出时 `--model-width` / `--model-height` 一致（现网 640×368）。单 rung 文件的会话 `--qp` 须与折叠进图的 `--qp` 一致，否则 rANS 码表与 NPU 嵌入错位；多 rung 文件由码控/帧头在覆盖区间内选档。

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

**运行时 rung 集合**（0.4 满血化后）：后端在 `bind_model` 时收集
`.rkmodel` 载荷表中的全部 qppatch 为一个升序去重的 rung 集合
（≤8 档），每档独立 `rknn_init` 出上下文。恒 QP 模式下集合退化为
单 rung（会话 qp 必须在集合内，否则 FORMAT 拒绝）。CBR 模式下编码端
按码控逐帧求解的 q_index 选最近 rung（并列取低），解码端按帧记录
q_index 精确命中 rung（多 rung 无命中即 FORMAT）。因此 CBR 部署的
`.rkmodel` 应携带覆盖目标 q 区间的若干档补丁（如
`--qp-list 10,21,30,40`），否则逐帧 q 会被钳到仅有的档位上。

## 全 QP 单模型（--qp-dynamic，QPT1）

QPP1 多 rung 是用空间换时间的折衷：每档一份上下文，切换有钳位误差。
`--qp-dynamic` 走零损路线——把每个 `Gather(q_table, q_index)` 改写为
FP16 `[1,C,1,1]` 图输入（行向量），q 表本体摘出图外随 `.rkmodel`
分发，运行时宿主按 q 查表把整行喂进 NPU，图内只剩常量广播乘（≤5 个
`Mul`）。单 RKNN 上下文覆盖全部 72 行 q 档，无 rung、无切换、无钳位：

```bash
.venv/bin/python tools/mlvc/export_rknn.py \
    --onnx-dir /path/to/onnx-generic/640x368 \
    --out-dir models/mlvc-dynq --platform rk3576 \
    --qp-dynamic
# 默认已打包：models/mlvc-dynq/mlvc_rk3576_dynq_{encoder,decoder}.rkmodel
# （rknn + pmf-gaussian + pmf-bitest + qptab；--no-pack 可关）
```

模型 I/O 契约（encoder 追加 3 输入、decoder 追加 3 输入，均 FP16 未量化）：

| 部件   | q 行输入                                            | 对应表（rows×cols）                  |
| ------ | --------------------------------------------------- | ------------------------------------ |
| 编码器 | `q_encoder_row` / `q_decoder_row` / `q_feature_row` | `[72,256]` / `[72,128]` / `[72,256]` |
| 解码器 | `q_feature_row` / `q_decoder_row` / `q_recon_row`   | 均 `[72,256]`                        |

`qptab_{encoder,decoder}.bin` 为 **QPT1** 线格式：`"QPT1"` magic +
u32 表数；每表 u32 name_len + name（≤32B，无 NUL）+ u32 rows + u32 cols +
rows×cols×2B FP16 行主序。rows=72（64 qp + 8 extra）。

运行时（`MlvcEncoderNode`/`MlvcDecoderNode`）在 bind 时扫输入名发现
`q_*_row` 即要求 `.rkmodel` 携带 qptab 载荷（kind `"qptab"`），恒 QP
直喂、CBR 逐帧直喂、
解码按帧记录头 q_index 直喂；q 不变时跳过行拷贝。_qp-dynamic 模型不得
再携带 QPPATCH rung（FORMAT 拒绝）_。折叠模型（无 `q_*_row` 输入）
自动走原 QPP1 路径，两者可在同一 registry 并存。

实测（RK3576 640×368×70 帧）：与折叠基线比特数偏差 +0.49%（常量预融合
vs 行输入的舍入差，画质等价）；编码 fps +33%（免 rung 上下文重建）。
RV1126B 单核：fps 偏差 -0.33%（噪声内，零损）。
