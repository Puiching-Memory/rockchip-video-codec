# MLVC NPU 执行 Profile 报告

> 更新日期：2026-08-24 ｜ 当前数据：RK3576 ｜ 标准 MLVC 与 MLVC-S 使用同一测量口径

> 历史性能记录（2026-08-24，RK3576）。数据来自归档版 `rkvc_transcode`。

## 1. 结论

标准 host I/O 修复了第二帧 NaN/Inf；在此正确性基线上，encoder 进一步
改为“native 零拷贝输入 + 逻辑 NCHW 输出”的混合 I/O。MLVC 和 MLVC-S 各
70 帧、3 轮连续编码的码流均与标准 I/O 逐字节一致，解码输出也保持
既有基线哈希。

| 变体   | 编码节点 (ms/帧) | 编码节点吞吐 | 解码节点 (ms/帧) | 解码节点吞吐 | 70 帧码流 | bpp      | Y MS-SSIM |
| ------ | ---------------- | ------------ | ---------------- | ------------ | --------- | -------- | --------- |
| MLVC   | **95.293±0.136** | 10.49 fps    | **81.225±0.122** | 12.31 fps    | 18,207 B  | 0.008835 | 0.974157  |
| MLVC-S | **47.781±0.265** | 20.93 fps    | **40.475±0.035** | 24.71 fps    | 11,898 B  | 0.005773 | 0.970691  |

这里的“节点”只统计 MLVC 节点内完成一帧所需的所有阶段，不含模型初始化、容器输入解析和进程退出。以下完整 CLI 墙钟是混合 I/O 落地前的标准 I/O 基线，仅作对照：

| 变体   | 编码墙钟      | 编码整程吞吐 | 解码墙钟      | 解码整程吞吐 |
| ------ | ------------- | ------------ | ------------- | ------------ |
| MLVC   | 9.253±0.020 s | 7.57 fps     | 6.177±0.069 s | 11.33 fps    |
| MLVC-S | 5.071±0.010 s | 13.80 fps    | 3.202±0.078 s | 21.86 fps    |

混合 I/O 不传递 producer 的 native feature：它仍从 `rknn_outputs_get` 取逻辑
NCHW，然后按 consumer 的 native input attr 显式打包。因此既保留了修复的布局
契约，又去掉了每帧约 28/18 ms 的 runtime input conversion。

## 2. 硬件、软件与模型

| 项目          | 值                                                                        |
| ------------- | ------------------------------------------------------------------------- |
| SoC / 板卡    | Rockchip RK3576 EVB1 v1.0                                                 |
| 内核          | Linux 6.1.118，aarch64                                                    |
| NPU           | 2 核，采样时 950 MHz，`rknpu_ondemand`                                    |
| CPU / 绑核    | policy0 / policy4 均为 `performance`；进程固定 CPU4–7（2.208 GHz 大核簇） |
| RKNN Runtime  | 2.3.2 (`429f97ae6b`)                                                      |
| RKNN Driver   | 0.9.8                                                                     |
| 分辨率 / 类型 | 640×368 / FP16                                                            |
| QP            | 21                                                                        |
| 采样末温度    | SoC 44.4 °C / NPU 43.5 °C                                                 |

本次模型指纹：

| 变体   | 模型    | SHA-256                                                            |
| ------ | ------- | ------------------------------------------------------------------ |
| MLVC   | encoder | `f8212344e56fad866fa2d16903e7a70abc09ef6cbef43d5ba2748f359ef6921f` |
| MLVC   | decoder | `cbe3c50788717f9aed2528b0e0f8fe9d59d4fe45e92e78307855ded323c88e33` |
| MLVC-S | encoder | `1e817c4314f26d2b08f31006f05b45c5d7b08de96d9ce7307d51451b8445efdc` |
| MLVC-S | decoder | `f4e78cc506b554f048dfdf370a787365dbb82d98b101cc32d218f2c85492d2fd` |

## 3. 测量方法

- 输入是同一个 640×368、YUV420、60 fps 的 3 帧验证片段循环到 70 帧；两种变体均使用 QP 21。
- 使用 Release 构建（`-O3 -DNDEBUG`），并以 `taskset -c 4-7` 固定在大核簇，避免单线程 CPU 阶段在大小核之间迁移。
- 每个变体的 encoder 和 decoder 各启动 3 个独立进程。每轮先预热 10 帧，再对后 60 帧做节点内分阶段聚合。
- 表中 `±` 是三轮“每轮均值”的样本标准差，而不是 180 个单帧样本混算后的标准差。
- 码流和解码 YUV 写入 `/dev/shm`。整程墙钟包含模型初始化、输入解析和文件 I/O；节点计时不包含这些项目。
- encoder 默认使用混合 I/O（native 输入、逻辑 NCHW 输出）；decoder 保持 native NC1HWC2 零拷贝 I/O。
- 质量值是 70 帧 Y 通道 5-scale MS-SSIM。输入由 3 帧循环得到，适合回归与性能复测，不应当作为自然视频 RD 曲线结论。

节点内 profile 默认关闭。启用后只增加分段时间戳，并在节点关闭时打印聚合值：

```bash
export RKVC_MLVC_PROFILE=1
export RKVC_MLVC_PROFILE_WARMUP=10
```

encoder profile 会额外打印 `io=standard|hybrid`，原 `inputs_set` 阶段在日志中
统一记为 `input_prepare`。可用以下方式回退复测标准 I/O：

```bash
export RKVC_MLVC_ENCODER_ZERO_COPY=0
```

混合路径默认开启；将开关设为 0 才使用 `io=standard`。

`RKVC_MLVC_PROFILE_WARMUP` 缺省为 10；设为 0 可统计所有帧。以标准 MLVC 为例：

```bash
RKVC_BIN=/path/to/archived/rkvc_transcode
MLVC_DIR=models/mlvc
INPUT_70F=/path/to/640x368-70f.y4m

taskset -c 4-7 "$RKVC_BIN" -i "$INPUT_70F" -o /dev/shm/profile.mlvc -c mlvc \
  --mlvc-enc "$MLVC_DIR/MLVCEncoder_rk3576.rknn" \
  --mlvc-gaussian-pmf "$MLVC_DIR/gaussian.bin" \
  --mlvc-bitest-pmf "$MLVC_DIR/bitest.bin" --mlvc-qp 21

taskset -c 4-7 "$RKVC_BIN" -i /dev/shm/profile.mlvc -o /dev/shm/profile.yuv \
  --mlvc-dec "$MLVC_DIR/MLVCDecoder_rk3576.rknn" \
  --mlvc-gaussian-pmf "$MLVC_DIR/gaussian.bin" \
  --mlvc-bitest-pmf "$MLVC_DIR/bitest.bin"
```

复测 MLVC-S 时只把 `MLVC_DIR` 改成 `models/mlvc-s`，不能跨 bundle 混用模型、PMF 或 QP patch。

## 4. Encoder 分阶段

以下是修复 NaN 后的标准 I/O 基线，用于展示原始优化目标：

| 阶段               | MLVC (ms)   | MLVC 占比 | MLVC-S (ms) | MLVC-S 占比 | 说明                                        |
| ------------------ | ----------- | --------- | ----------- | ----------- | ------------------------------------------- |
| YUV→fp16 NHWC      | 0.611       | 0.5%      | 0.613       | 0.9%        | 两种变体输入尺寸相同                        |
| `rknn_inputs_set`  | 28.026      | 22.3%     | 18.289      | 27.3%       | host copy + runtime layout 处理             |
| **`rknn_run`**     | **88.014**  | **70.0%** | **44.162**  | **66.0%**   | 模型执行                                    |
| `rknn_outputs_get` | 1.923       | 1.5%      | 1.258       | 1.9%        | 取逻辑 NCHW 输出                            |
| 输出后处理         | 5.684       | 4.5%      | 1.965       | 2.9%        | finite 校验、latent 转换、feature NCHW→NHWC |
| `extract_scales`   | 0.162       | 0.1%      | 0.064       | 0.1%        | 熵模型索引                                  |
| rANS + packet      | 1.359       | 1.1%      | 0.540       | 0.8%        | 熵编码与帧包分配                            |
| **合计**           | **125.778** | **100%**  | **66.891**  | **100%**    | 三轮均值；分项显示值有舍入                  |

标准 MLVC 的 `inputs_set + outputs_get + 输出后处理` 为 35.63 ms/帧，是混合 I/O 的主要优化来源。混合路径显式保留逻辑 feature 边界并通过 70 帧递归一致性测试；仍不能仅凭 `n_elems` 或 native shape 相同直接 `memcpy`。

混合 I/O 与同轮标准 I/O A/B 如下（均为 3 轮均值的样本统计）：

| 变体   | 标准 I/O (ms/帧) | 混合 I/O (ms/帧) | 节省      | `input_prepare` 代表值 |
| ------ | ---------------- | ---------------- | --------- | ---------------------- |
| MLVC   | 125.607±0.571    | **95.293±0.136** | **23.0%** | 27.96 → 0.29 ms        |
| MLVC-S | 66.660±0.299     | **47.781±0.265** | **28.3%** | 18.27 → 0.29 ms        |

## 5. Decoder 分阶段

| 阶段                    | MLVC (ms)  | MLVC 占比 | MLVC-S (ms) | MLVC-S 占比 | 说明                        |
| ----------------------- | ---------- | --------- | ----------- | ----------- | --------------------------- |
| rANS + `extract_scales` | 2.200      | 2.7%      | 0.862       | 2.1%        | 熵解码                      |
| NCHW→NC1HWC2 fp16       | 0.247      | 0.3%      | 0.099       | 0.2%        | latent 打包                 |
| 输入写入 + sync         | 0.716      | 0.9%      | 0.301       | 0.7%        | 4 个 native tensor          |
| **`rknn_run`**          | **73.692** | **90.7%** | **36.005**  | **89.0%**   | 模型执行                    |
| x_hat→NV12              | 2.558      | 3.1%      | 2.562       | 6.3%        | CPU DepthToSpace DCR + NV12 |
| feature sync + copy     | 1.811      | 2.2%      | 0.647       | 1.6%        | 下一帧 native reference     |
| **合计**                | **81.225** | **100%**  | **40.475**  | **100%**    | 三轮均值；分项显示值有舍入  |

decoder 的 native feature 输出与下一帧 native reference 已经由当前模型链路验证可直接递归；它与发生问题的 encoder producer/consumer 对不是同一接口契约。标准 decoder 的首要瓶颈仍是模型执行，其次是 CPU 侧 `x_hat→NV12`。

## 6. 正确性与确定性

| 检查                     | MLVC                                                               | MLVC-S                                                             |
| ------------------------ | ------------------------------------------------------------------ | ------------------------------------------------------------------ |
| 70 帧连续 encode/decode  | 通过                                                               | 通过                                                               |
| 第二帧及后续 finite 检查 | 通过                                                               | 通过                                                               |
| 三轮码流逐字节一致       | 通过                                                               | 通过                                                               |
| 三轮解码 YUV 逐字节一致  | 通过                                                               | 通过                                                               |
| 码流 SHA-256             | `ff9f19261e028107ca9c2afbe94ac173cdadaf9c8ec40d19f33ba6a6c4ed1196` | `fb6c1710718e000da8fce7f8c5616c643ff7f0e9cf5414e4ff5a4536b0a36dd2` |
| 解码 YUV SHA-256         | `3b8c4971da0d34f576d8d57aa4162808b3dc745aa8de9cc0f8f02bd528678d0f` | `1fb39dee2c2e36bb520e1b1d0e942ec3d02094499108e11a911dd8175463cb26` |

## 7. 当前优化优先级

1. **NPU 模型执行（混合 I/O 后 MLVC encoder 约 91%）**：继续从 ONNX/RKNN 算子分配和网络结构入手；这已成为 encoder 的绝对主瓶颈。
2. **Encoder 输出逻辑化**：若继续优化，需在导出图边界显式证明 producer 到 consumer 的 native 映射；不能回退到 native `memcpy`。
3. **Decoder x_hat→NV12（MLVC/MLVC-S 均约 2.56 ms）**：两种变体固定成本几乎相同，可继续做 SIMD/并行化，但优先级低于 encoder I/O 和 NPU 模型本体。

## 附录 A：旧 RK3588 数据（仅作历史参考）

2026-08-13 的旧报告使用 RK3588、RKNN API 2.4.2a2、三 NPU 核和旧 encoder native I/O。平台、模型产物、运行时和 I/O 路径均与本次不同，不能用来计算本次修复的性能回退比例。

| 旧指标               | Encoder | Decoder  |
| -------------------- | ------- | -------- |
| 独立 `rknn_run`      | 88.0 ms | 178.2 ms |
| 旧端到端节点         | 98.4 ms | 188.4 ms |
| PERF_DETAIL 采集开销 | +15.1%  | +27.9%   |

旧 PERF_DETAIL 的相对分布显示 encoder CPU fallback 约 36.5%（主要为 SpaceToDepth、Max、Div），decoder 约 58.8% 的算子时间位于 NPU ConvTranspose。它们只用于解释当时的模型，不代表当前 RK3576 模型的算子分布；当前模型若要作相同结论，必须重新以 `RKNN_QUERY_PERF_DETAIL` 采样。
