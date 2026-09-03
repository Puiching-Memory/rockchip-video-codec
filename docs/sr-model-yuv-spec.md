# Phase-RLFN 超分模型：ONNX → RKNN → bundle

RKVC 的 `rkvc_sr` 仅支持开源项目
[Puiching-Memory/rknn-super-resolution](https://github.com/Puiching-Memory/rknn-super-resolution)
的 3×、单输入 fallback 部署 core。旧 RGB 端到端 RKNN 与 codec-aware 双输入模型均不兼容。

## 固定运行时契约

| 项目          | 约定                                                                                                |
| ------------- | --------------------------------------------------------------------------------------------------- |
| 颜色空间      | MLVC BT.709 full-range YCbCr444 `[0,255]`                                                           |
| 输入          | `phases`: uint8 **NHWC** 字节流 `1×180×320×12`（逻辑 `1×12×180×320`，640×360 经 PixelUnshuffle(2)） |
| 输出          | `phase_residual`: **NCHW** 平面字节流 `1×108×180×320`（PixelShuffle(6) 后为 1920×1080）             |
| 倍率          | 固定 3×                                                                                             |
| codec context | 关闭；必须导出 `--no-codec-context` 的单输入 fallback core                                          |
| RKNN target   | 默认 `rk3588`                                                                                       |

运行时流程：

```text
NV12 640×360
  ├─ Y/UV → YCbCr444 → PixelUnshuffle(2) → RKNN Phase-RLFN core ─┐
  └─ RGA bicubic → NV12 1920×1080                              │
                                                               ↓
                PixelShuffle(6) residual → Y 逐点相加 / UV 2×2 平均 → NV12
```

实现位于 `lib/node_rkvc_sr.c` 与 `lib/rkvc_sr_phase.c`。创建上下文时会检查模型
恰好为单输入/单输出、`12→108` 通道契约；宿主输入属性被工具链记为 NHWC
（dims `1×180×320×12`，rknn-toolkit2 对 NCHW 图的常见记法）时同样接受。旧 3 通道模型或双输入模型直接失败。

> **布局契约（实机测定，勿凭直觉修改）**：rknn 驱动直接按属性 `dims` 解释宿主
> 缓冲的线性字节序，**不做自动转置**，且输入/输出属性不对称：输入记为 NHWC
> （`1×H×W×12`）、输出记为 NCHW（`1×108×H×W`）。因此 `rkvc_sr_phase_pack_nv12`
> 必须逐像素交错打包，而 `rkvc_sr_phase_add_residual_nv12` 必须按平面主序读取。
> 用恒等模型（`out = x * 1.0`，输入值编码线性索引）+ x86 模拟器可交叉验证：
> 错误字节序会使 NPU 输出与 ONNX 参考差到 rms≈10 的量级，看起来像“FP16 精度
> 不足”，实为布局问题。

### RGA 基座与训练假设的一致性（实测）

训练基座为 `F.interpolate(bicubic, align_corners=False)`（见上游 `PhaseRLFNSR.bicubic_base`）；
运行时基座为 RGA 硬件 `imresize INTER_CUBIC`（NV12 3×，同一 `node_rga.c` 路径）。
RK3576 实机实测（librga 1.10.6_[3]，Johnny 640×360→1920×1080）：

| 平面 | MAE  | MAX | PSNR   |
| ---- | ---- | --- | ------ |
| Y    | 0.82 | 38  | 41.2dB |
| U    | 0.34 | 29  | 48.1dB |
| V    | 0.29 | 15  | 49.8dB |

1px 细线合成帧线能量比 0.98（无细节丢失）。差异来自两种 bicubic 实现的
系数精度/裁剪差异，属预期范围；模型残差学习对此鲁棒。
注意：RGA 的 RGB→YUV420 CSC 存在色度下采样取 2×2 块左上角而非均值的已知问题
（[librga#122](https://github.com/airockchip/librga/issues/122)，1.10.6_[3] 实测复现，
细线场景色度严重失真）；本项目管线全 NV12 域、无 RGB→YUV420 CSC 调用，
不受影响；若未来新增该方向转换，勿用 RGA 硬件 CSC 处理含细线/文字内容。

### FP16 与 INT8（KL 校准）实机对照

均在上述**正确宿主字节序**下重测（早期所有“INT8 误差大于信号”的结论
均因输入被转置而失效）：

| 配置                    | 模型体积 | Y-PSNR | SSIM Y | NPU 侧 ms/帧（inputs_set+run+outputs_get） | 30 帧端到端 |
| ----------------------- | -------- | ------ | ------ | ------------------------------------------ | ----------- |
| RGA bicubic 基线        | —        | 25.33  | 0.857  | —                                          | 1.4s        |
| FP16（`--no-quantize`） | 601KB    | 26.30  | 0.879  | 9.4+20.7+19.5 = 49.6                       | 3.9s        |
| INT8 + KL 校准          | 375KB    | 26.25  | 0.878  | 6.8+11.1+15.2 = 33.1                       | 3.3s        |

测试条件：RK3576，Johnny 640×360→1920×1080，30 帧，rknnrt 2.3.2 / NPU 驱动 0.9.8。
INT8 相对 FP16 仅 −0.05dB / −0.001 SSIM，但 NPU 侧快约 1.5×、体积小 38%；
`export_model.py` 单独运行时默认导出 INT8，FP16 可用于排查量化回归。自动打包路径当前固定传入 `--no-quantize`，因此可移植包默认携带 FP16 模型，避免在没有代表性校准集时生成误导性的 PTQ 产物；交付 INT8 前应准备校准集并显式导出到 `.build/models/<platform>/rkvc-sr/`。

其他实测要点：

- 融合阶段（`rkvc_sr_phase_add_residual_nv12`）是单线程 CPU 热路径，对大小核
  敏感：A72 22ms/帧 vs A53 70ms/帧。 profiling 按 `docs/mlvc-npu-profile.md` 的
  约定用 `taskset -c 4-7` 固定到大核簇。
- `rknn_outputs_get(want_float=1)` 的 FP32 转换要占 15–20ms/帧（输出 6.2M 元素）；
  改为 `want_float=0` 取回原生布局可再省 7ms（FP16）/11ms（INT8），但融合
  路径需自行反量化。
- ONNX float32 参考（x86，onnxruntime）与板端 FP16 输出在正确布局下一致到
  rms 0.004 / max 0.09，与理想融合（ONNX 残差 + 管线 RGA 基座）的一致性 73.5dB，
  即可认为 NPU 路径无额外损失。

## 1. 准备项目 Python 环境

```bash
uv sync
```

本仓将上游源码固定到 `tools/sr/export_model.py` 中的 commit，并默认浅克隆到
`.build/deps/rknn-super-resolution/`。上游训练环境绑定 Python 3.13/CUDA，RKNN
Toolkit 与其 Torch 版本冲突；本适配器只导入上游模型定义，在本仓 Python 3.12
环境中完成静态 ONNX 导出和 RKNN 转换。

## 2. 准备 checkpoint

传入上游训练生成的 `best_ema.pth`（默认按 codec-aware QAT checkpoint 读取）：

```text
/path/to/checkpoints/phase-rlfn-codec-v1/best_ema.pth
```

官方 QAT checkpoint 已托管在 HuggingFace
[Sail2Dream/phase-rlfn-codec-v1](https://huggingface.co/Sail2Dream/phase-rlfn-codec-v1)（`best_ema.pth`，
SHA-256 `0cf78cee...c84070`）。当前需由导出工具显式下载或传入该权重；
手动导出也可直接下载后传给 `--weight`。若使用 float checkpoint（仓内 `float/best.pth`），
导出时加 `--no-from-qat`。

QAT checkpoint 除可学习权重外还包含 observer/fake-quant 状态；这些训练算子不能由
本仓固定的 Torch 2.2 legacy ONNX exporter 导出。适配器会按名称和 shape 严格提取
deploy graph 的全部可学习参数，拒绝缺失/错形状，然后导出不含训练观测器的干净
单输入 core。未传 `codec_feature`，其 adapter 分支不会进入 ONNX。

## 3. 生成 RKNN 校准集

从代表实际低分辨率解码分布的图片生成单输入 `.npy`。工具会先模拟 NV12
4:2:0 色度采样，再使用与 C 运行时相同的双线性 4:4:4 扩展和 PixelUnshuffle：

```bash
.venv/bin/python tools/sr/build_calibration.py /path/to/lr-images \
  --output-dir .build/sr-calibration/tensors \
  --output-list .build/sr-calibration/calibration.txt \
  --width 640 --height 360 --limit 100
```

生产转换应优先使用真实“编码→解码”后的 LR 帧。上游自带的
`rknn-super-resolution-build-rknn-calibration` 也可生成 MLVC 重建分布；本项目
只消费其单输入 calibration list，不消费 `codec_feature` 第二列。

## 4. 一键导出 ONNX、RKNN 和 bundle

```bash
.venv/bin/python tools/sr/export_model.py \
  --weight /path/to/best_ema.pth \
  --calibration-list .build/sr-calibration/calibration.txt \
  --target rk3588 \
  --output-dir models/rkvc-sr
```

默认执行：静态单输入 ONNX → 契约校验 → INT8 RKNN → SHA-256 manifest →
上游 MIT LICENSE/SOURCE 信息。常用变体：

```bash
# 只导出并检查 ONNX
.venv/bin/python tools/sr/export_model.py --weight /path/to/best_ema.pth --onnx-only

# 从 HF QAT checkpoint 免校准导出（打包自动流程同款，不做 PTQ，无需校准集）
.venv/bin/python tools/sr/export_model.py \
  --weight /path/to/best_ema.pth --no-quantize --target rk3576

# 从已审核 ONNX 开始转换
.venv/bin/python tools/sr/export_model.py \
  --onnx /path/to/phase_rlfn_sr_x3.onnx \
  --calibration-list .build/sr-calibration/calibration.txt

# FP16 调试模型，不量化
.venv/bin/python tools/sr/export_model.py \
  --weight /path/to/float.pth --no-from-qat --no-quantize
```

> 自动下载 HF QAT checkpoint 并用 `--no-quantize` 转换，无需校准集；手动
> INT8 校准变体（§3）可用于自定义校准分布。

完整 bundle：

```text
models/rkvc-sr/
├── phase_rlfn_sr_x3.onnx
├── phase_rlfn_sr_x3.rknn
├── sr_export_manifest.json
├── LICENSE.rknn-super-resolution-MIT
└── SOURCE.md
```

模型产物默认被 Git 忽略，manifest、LICENSE 与 SOURCE 由每次导出刷新。不要混用
不同 checkpoint、ONNX、RKNN 或 manifest。

可单独校验 bundle；模型适配器必须执行同一大小/SHA-256 门禁：

```bash
python3 tools/sr/verify_bundle.py models/rkvc-sr
```

## 5. 构建、打包与实机门禁

SR 模型必须进入 `.rkmodel` 注册表，由 request 约束选择。实机门禁要求
RKNN NPU 与 RGA，并建立正确性、性能和长稳基线。
