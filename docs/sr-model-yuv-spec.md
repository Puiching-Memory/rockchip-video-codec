# YUV-native 超分模型规格（设计稿）

> **状态**：设计稿，尚未有现网模型。当前 `rkvc_sr` 使用 **RGB 域**训练的 RKNN 模型，推理路径含 NV12↔RGB CSC 开销（见 `lib/node_rkvc_sr.c`）。

## 目标

下一代 RKVC 超分模型在 **YUV 域**完成训练与推理，消除 RGB 色彩空间转换，降低延迟与带宽，并与 Session 后处理节点 `node_post_upscale` / `RKVC_UPSCALE_AI_SR` 对齐。

## 与现网 `rkvc_sr` 的差异

| 维度 | 现网（RGB） | YUV-native（目标） |
|------|-------------|-------------------|
| 训练域 | RGB24 NHWC | NV12 / YUV420P 平面 |
| 推理输入 | RGA CSC → RGB → int8 NCHW | 直接 Y/UV 量化 |
| 推理输出 | int8 NCHW → RGB → RGA CSC → NV12 | 直接 Y/UV 反量化 |
| CSC 开销 | 每帧 2 次 RGA 色彩转换 | 无（或仅下采样 RGA） |
| API | `post_upscale_algo=rkvc_sr` + `post_upscale_rkvc_model_path` | 相同字段，新 `.rknn` 模型 |

## 模型 I/O 约定（草案）

### 输入

- **低分辨率 NV12**（与 `enc_scale_denom` 下采样后的解码帧一致）
- 形状：`1 × (H/2 + H/2) × W` 打包 Y/UV，或双输入 `Y: 1×H×W`、`UV: 1×H/2×W`（导出时二选一，须在模型元数据中声明）
- 量化：int8，scale/zero-point 写入 RKNN 自定义 metadata 或固定头

### 输出

- **全分辨率 NV12**（与 Session `width`/`height` 一致）
- 与输入相同的平面布局约定

### 倍数

- 首版支持 **2× / 3×**（与 bench `ENC_SCALE_DENOM` 及 `rkvc_session_upscale --enc-scale-denom` 对齐）
- 模型文件名或 metadata 须标明 `scale=2` 或 `scale=3`

## RKNN 导出要求

1. 目标平台：**RK3588 NPU**（`librknnrt`）
2. 支持明文 `.rknn` 与项目已有的加密 `.rknn` 加载路径（见 `node_rkvc_sr.c`）
3. 双 slot 异步推理：输入/输出 buffer 尺寸固定，避免动态 shape
4. 与 `rkvc_sr_neon.c` 量化接口兼容，或提供 YUV 专用 NEON 例程并在 CMake 中切换

## RKVC 集成检查清单

- [ ] `RKVC_UPSCALE_AI_SR` 路径识别 YUV-native 模型（metadata 或文件名后缀 `_yuv`）
- [ ] 跳过 NV12↔RGB CSC，仅保留必要的 RGA 下采样预处理
- [ ] `rkvc_session_upscale --post-upscale rkvc_sr --rkvc-sr-model PATH` 无需改 CLI
- [ ] bench `post-upscale` 路线增加 `{codec}+up{N}x-rkvc_sr_yuv` 实验名（可选）
- [ ] 硬件回归：`test_session_encode_decode_upscale_3x` 扩展 AI_SR 用例（需模型文件）

## 训练数据建议

- 源：1080p 监控/自然场景 NV12 序列
- 退化：与产品路径一致 — 全分辨率 REF → RGA `1/N` 下采样 → 编码 → 解码 → 作为 LR 输入，REF 作为 HR 标签
- 损失：Y/UV 加权 L1 或 Charbonnier；可选 SSIM on Y 平面

## 参考实现

- 现网 RGB 路径：`lib/node_rkvc_sr.c`、`lib/rkvc_sr_neon.c`
- RGA 传统上采样：`lib/node_rga.c`、`lib/node_post_upscale.c`
- Bench 评估：`bench/README.md` post-upscale 路线
