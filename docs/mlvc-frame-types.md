# MLVC 帧型与参考机制（I / P / LTR）

本文说明 `codecs/mlvc` 的 I 帧 / P 帧 / LTR 恢复帧语义——**帧型只决定「参考先验来自哪里」，
编解码各只有一个模型**。内容分两部分：上游 microsoft/mlvc 的原始设计，以及本仓 C++ 实现
与它的对应关系。

## 上游出处

- **MLVC: Multi-platform Learned Video Codec for Real-World Deployment**
  （Pärnamaa / Lumiste / Loot / Indenbom / Znobishchev / Saabas，arXiv 2606.28027）。
- 上游 README 声明起点是 **DCVC-RT**（`microsoft/DCVC`），熵编码适配自 **ryg_rans**。
- 本仓库不 vendoring 上游代码，模型由 `tools/mlvc/export_rknn.py --from-mlvc` 从
  `mlvc-psnr-v1.ckpt` 导出，见 `docs/mlvc-rknn-export.md`。

## 核心：帧型 = 参考来源，模型只有一个

上游 `video/conversion/_full_model/_dmc61sb_model.py` 的 `apply_feature_adaptor(dpb)`
承担全部帧型语义：

```python
def apply_feature_adaptor(self, dpb):
    if self.model_params.disable_feature_reset:              # ← 出货配置走这条
        feature_ref_feature, feature_ref_memory = dpb["ref_feature"].chunk(2, dim=1)
        feature_ref_feature = self.feature_adaptor_p(feature_ref_feature)
        return feature_ref_feature, feature_ref_memory
    else:                                                    # ← 通用变体
        frame = self.shift_input(dpb["ref_frame"])
        ref_frame = torch.nn.functional.pixel_unshuffle(frame, self.pixel_shuffle_factor)
        frame_feature = self.feature_adaptor_i(ref_frame)     # 自参考：用参考帧自己造先验
        frame_ref_feature, frame_ref_memory = frame_feature.chunk(2, dim=1)
        if self.chain_feature_adaptors:
            frame_ref_feature = self.feature_adaptor_p(frame_ref_feature)
        feature_ref_feature, feature_ref_memory = dpb["ref_feature"].chunk(2, dim=1)
        feature_ref_feature = self.feature_adaptor_p(feature_ref_feature)
        ref_exists = dpb["ref_exists"]                        # 0/1 软混合
        return (ref_exists * feature_ref_feature + (1 - ref_exists) * frame_ref_feature,
                ref_exists * feature_ref_memory  + (1 - ref_exists) * frame_ref_memory)
```

DPB 里的 `ref_feature` 是 **2F 通道**，即 `feature(F) ‖ memory(F)`（本仓 128+128=256），
与 `codecs/mlvc/src/decoder.cpp` 里 `Cat(feature, memory)` 写回的形状一致。

- **P 帧**：`ref_exists = 1` → 取 DPB 的 `ref_feature`，`feature` 半边过
  `feature_adaptor_p`（1×1 conv）。
- **I 帧**：`ref_exists = 0` → 不做时间预测，改用「参考帧自己」经
  `pixel_unshuffle(8) → feature_adaptor_i → chunk(2)` 现造一份 (feature, memory)。

`chain_feature_adaptors: true` 让 I 路产出的 feature 再过一次 `feature_adaptor_p`，
使两条路的通道统计对齐，模型才能共用。

## 出货配置关掉了 I 帧自参考支路

上游 `video/conversion/_full_model/model_configs_example.yaml` 的**两个条目**（标准 MLVC 与
MLVC-S）都写了：

```yaml
params:
  disable_feature_reset: true
  chain_feature_adaptors: true
```

`disable_feature_reset: true` 使 `apply_feature_adaptor` 永远走第一条分支：只读 DPB 的
`ref_feature`，`ref_exists` / `ref_frame` 不进图。这解释了导出契约——**编码图只有 2 个输入
`[x, ref_feature]`**（见 `tools/mlvc/onnx_rewrite.py` 的 `ENC_INPUT_KEYS`），而不是 split 模型
里的 5 输入 `[x, ref_frame, ref_feature, q_index_shifted, ref_exists]`。

因此 I 帧由**调用方给空参考**：上游 `video/conversion/_frame_loop.py` 在 IDR 时

```python
frame_type = FrameType.I_FRAME
cur_frame_idx = 0
latest_ltr_frame_idx = None
ref_manager_encoder.clear()
ref_manager_decoder.clear()
```

随后 `ref_frame_idx = None` → `ref_manager.load(None)` 返回
`video/conversion/_split_model/_base_split_model.py` 中的默认值
（`ref_exists=False`、`ref_feature` 全零）。

本仓 C++ 与此一致：`codecs/mlvc/src/encoder.cpp` 在 `is_idr` 时清零 `ref_prev` / `ref_ltr`
并照常数喂 `ref_feature`；`codecs/mlvc/src/decoder.cpp` 收到 `kRecKeyframe` 时同样清零。
**I 帧 = 空先验 + 该帧自身图像**，同一模型、同一张图。

## 帧型状态机对照

| 上游 `video/conversion/_frame_loop.py` | 本仓 `codecs/mlvc/src/encoder.cpp` |
| --- | --- |
| `i == 0 or (iframe_period and cur_frame_idx % iframe_period == 0)` → `I_FRAME`，`cur_frame_idx = 0` | `is_idr`（首帧用 `frame_count == 0`） |
| IDR 时 `ref_manager.clear()`、`latest_ltr_frame_idx = None` | 清 `ref_prev` / `ref_ltr`，`have_ltr = false` |
| `mark_as_ltr = ltr_period>0 and (cur == ltr_start_idx or cur > ltr_start_idx and cur % ltr_period == 0)` | `mark_ltr`，同构 |
| `proactive_ltr_recovery and mark_as_ltr and latest_ltr_frame_idx is not None` → `LTR_RECOVERY`，`ref_frame_idx = latest_ltr_frame_idx` | `is_recovery`，参考取 `ref_ltr` |
| P 帧 `ref_frame_idx = cur_frame_idx - 1` | 参考取 `ref_prev` |
| 仅 `P_FRAME` 且非 `mark_as_ltr`、非 `feature_reset` 可丢，否则回退 `q_index = 0` | 仅 `RcFrameType::P` 且 `!mark_ltr` 可丢，否则 `solved = 0` |
| 丢弃帧解码时重复上一重建帧 | 用 `last_nv12` 重发（`q_index = -1` 记录） |
| `RateController.solve_q_index(pt, frame_type, reserved_overhead_bits)` | `rc->solve(pt, rc_type, kRecSize * 8)` |

码控帧型为三分（`codecs/mlvc/include/mlvc/ratectl.hpp` 的 `RcFrameType`）：

| 帧型 | 参考先验 | 记录 flags | 码控模型 |
| --- | --- | --- | --- |
| I | 空（全零） | `kRecKeyframe` 0x1 | `m_i_` / `m_p_idr_` |
| P | `ref_prev` | 无 | `m_p_ltr_` |
| LTR 恢复 | `ref_ltr` | `kRecLtrRecovery` 0x4 | `m_ltr_` |

标记帧另有 `kRecLtrMark` 0x2；三者与容器 64B 头里的 `iframe_period` / `ltr_start_idx` /
`ltr_period` / `hdr_flags` 一起构成 `.mlvc` 的帧型表达（`codecs/mlvc/include/mlvc/container.hpp`）。

## 待确认的口径问题

1. **架构图与出货配置不一致**：`docs/images/mlvc-architecture.svg` 写
   「首帧由 FeatureAdaptor-I 从参考帧生成」，即 `disable_feature_reset: false` 的那条支路；
   但转换配置写的是 `true`，导出的 2 输入图里没有 `feature_adaptor_i`。
   按配置推断本仓 I 帧是「零先验」，图注描述的却是「自参考先验」——两者只能有一个对得上
   实际 RNKN，需实测确认。
2. **DMCI 的角色**：上游另有一个独立图像模型 **DMCI-6.0**（DCVC-RT 的
   `cvpr2025_image.pth.tar`，`train_image.py` / `train_image-dcvcrt.yaml`），README 归类为
   *auxiliary*；基准锚点文件名为 `dcvcrt_with_iframe_metrics_*.json`。它不参与 `dmc61sbr`
   的导出图，主要用于 I 帧指标与对比。本仓没有第三个模型，I 帧只走「空参考」路径。
3. **`feature_reset` / `reset_period` 未实现**：上游有周期性特征重置（`reset_period`），
   出货配置为 `null`，故本仓 C++ 未实现，丢帧条件中也就少了 `feature_reset` 一项。

## 相关文件

- `codecs/mlvc/src/encoder.cpp`、`codecs/mlvc/src/decoder.cpp`：帧型判定与双槽 DPB
- `codecs/mlvc/include/mlvc/ratectl.hpp`：`RcFrameType` 与四个 R-Q 模型
- `codecs/mlvc/include/mlvc/container.hpp`：记录 flags 与头字段
- `tools/mlvc/onnx_rewrite.py`：导出图的 I/O 契约（`ENC_INPUT_KEYS` / `DEC_INPUT_KEYS`）
- `tools/mlvc/export_onnx.py`：MLVC-S 转换配置（含 `disable_feature_reset` / `chain_feature_adaptors`）
- `docs/images/mlvc-architecture.svg`：MLVC / MLVC-S 模型架构图
