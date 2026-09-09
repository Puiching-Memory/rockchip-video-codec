# MLVC 码流网络传输规范（草案 v0.1）

> 状态：**设计草案**。第 2 章容器格式与 `codecs/mlvc/src/container.cpp`
> 实现一致；其余各章为待实现的传输映射设计。落地状态见文末实现状态矩阵。

本文定义 MLVC 神经视频码流在标准协议上的承载方式，目标是：传输、复用、
信令层全部使用标准协议（GB/T 28181、RTSP/RTP、MPEG-TS/SRT），MLVC 仅以
"标准封装 + 已注册的私有 codec 标识"出现，下游客户的适配收敛为
"标准协议栈 + MLVC 解码 SDK"，无需理解任何私有协议。

本文不定义 SIP 会话流程（GB/T 28181-2016/2022 已定义）、不定义 MLVC
编解码本身（见 `docs/semantic-codec-sdk-integration.md` 与官方格式），
只定义码流的**分帧、标识、带外参数、时钟与丢包语义**。

## 1. 术语与设计原点

- **配置记录（Configuration Record）**：`.mlvc` 容器的 64B 头，
  流式传输时作为带外参数集传递（类比 H.264 的 avcC/SPS-PPS）。
- **帧单元（Frame Unit）**：`.mlvc` 容器的帧记录（16B 记录头 + 载荷），
  是所有封装的原子传输单元。
- MLVC 编码为帧间预测结构（DPB 参考特征），rANS 熵编码对字节级偏差
  零容忍：任一帧载荷损坏或丢失，该帧及其后全部非关键帧均不可解码。
  这一性质决定了第 5 章的丢包语义与传输选型。

## 2. 基础容器线格式（已实现）

全部小端。与 `codecs/mlvc/src/container.cpp` 逐字段一致。

### 2.1 容器头（64B，即配置记录）

| 偏移 | 长度 | 字段               | 说明                                    |
| ---- | ---- | ------------------ | --------------------------------------- |
| 0    | 5    | magic              | `"MLVC1"`                               |
| 5    | 1    | format             | 固定格式标识 0x02；不符的接收端必须拒绝 |
| 6    | 2    | pad                | 0                                       |
| 8    | 4    | width              | 像素宽                                  |
| 12   | 4    | height             | 像素高                                  |
| 16   | 4    | fps_num            | 标称帧率分子（默认 30）                 |
| 20   | 4    | fps_den            | 标称帧率分母（默认 1）                  |
| 24   | 4    | qp                 | 会话基准 q_index（rANS 初始化依赖）     |
| 28   | 4    | frame_count        | 写 0；接收端忽略，以流尾为准            |
| 32   | 4    | iframe_period      | IDR 周期；0 = 仅首帧                    |
| 36   | 4    | ltr_start_idx      | 首个 LTR 帧序（GOP 内相对帧序，信息性） |
| 40   | 4    | ltr_period         | LTR 周期（GOP 内相对帧序）；0 = 关闭    |
| 44   | 4    | hdr_flags          | bit0 PROACTIVE_LTR，bit1 CBR（信息性）  |
| 48   | 4    | target_bitrate_bps | CBR 目标码率（信息性；0 = 恒 QP）       |
| 52   | 12   | reserved           | 0                                       |

### 2.2 帧记录（16B 头 + 载荷）

| 偏移 | 长度 | 字段         | 说明                                              |
| ---- | ---- | ------------ | ------------------------------------------------- |
| 0    | 4    | payload_size | 载荷字节数；>64MiB 视为格式错误                   |
| 4    | 4    | q_index      | 本帧实际 q_index；-1 = 丢帧标记（见下）           |
| 8    | 4    | flags        | bit0 KEYFRAME / bit1 LTR_MARK / bit2 LTR_RECOVERY |
| 12   | 4    | reserved     | 0                                                 |
| 16   | n    | payload      | 不透明 rANS 码流，传输层不得解析                  |

**丢帧标记帧**：`q_index == -1` 且 `payload_size == 0`（记录共 16B，
无载荷）。表示编码端码控主动丢弃该帧；解码端重复上一输出帧补齐
时间轴。与传输层丢包（第 5 章）是完全不同的语义，不得混淆。

**flags 语义**：
- `KEYFRAME`（IDR）：不依赖任何解码历史，解码端清空 DPB（含 LTR 槽）。
- `LTR_MARK`：本帧解码后的 feature 被存入长期参考槽。
- `LTR_RECOVERY`：本帧以长期参考槽（而非上一帧）为参考。

编码端标记规则（信息性）：帧序按 GOP 内相对值计（IDR 后归零），
GOP 内帧序等于 `ltr_start_idx` 或大于它且为 `ltr_period` 整数倍的
P 帧打 `LTR_MARK`；已持有长期参考时置 `LTR_RECOVERY`（proactive
恢复）。解码端只依据 flags 行动，不重建标记规则。

载荷内部结构（三段 rANS 流）由解码器自描述，传输层只需
`payload + payload_size + q_index + flags` 四要素。

### 2.3 QP 语义

容器头 `qp` 为会话基准 q_index，决定解码端 rANS/模型初始化集合；
CBR 模式下逐帧实际 q_index 由记录头携带，允许会话内逐帧变化
（动态码控）。恒 QP 模式下逐帧 q_index 恒等于基准值。

## 3. 带外参数传递

配置记录以 base64(64B) 文本形式在各协议层带外递送：

| 场景       | 载体                                                                                                                     |
| ---------- | ------------------------------------------------------------------------------------------------------------------------ |
| RTSP/SDP   | `a=fmtp:96 mlvc-config=<base64>`（形状对齐 H.264 `sprop-parameter-sets`）                                                |
| MPEG-TS    | PMT ES 描述符：registration descriptor（tag 0x05，`format_identifier='MLVC'`）+ 私有描述符（tag 0xA0，64B 配置记录原文） |
| GB28181/PS | PSM 的 ES info 内同上两个描述符                                                                                          |

接收端在解出任何帧单元之前必须已获得配置记录；未获得而先收到帧
单元时必须缓存或丢弃至下一关键帧。

## 4. MLVC over RTP（RTSP / 直连 RTP）

### 4.1 RTP 头

- PT：动态范围 96–127，SDP 协商。
- 时钟：90000 Hz。
- Marker：一帧最后一个 RTP 包置 1。
- Timestamp：该帧的 90 kHz 采样时刻（见第 8 章）。

### 4.2 载荷头（1B，必现）

```text
 0 1 2 3 4 5 6 7
+-+-+-+-+-+-+-+-+
|F|K|  reserved |
+-+-+-+-+-+-+-+-+
```

- **F**：0 = 单包（载荷头后跟完整帧单元）；1 = 分片包。
- **K**：帧单元 flags.KEYFRAME 的冗余副本；分片时仅首片置 1。
  供丢包检测在重组完成前判定帧属性。
- reserved 恒 0，接收端忽略。

### 4.3 分片头（F=1 时第 2 字节）

```text
+-+-+-+-+-+-+-+-+
|S|E| reserved  |
+-+-+-+-+-+-+-+-+
```

- **S**：首片；**E**：末片。单帧超 MTU 时等长切片，除末片外每片
  尽量接近但不超 Path MTU 减协议开销（默认载荷上限 1400B）。

分片边界即字节区间拼接，重组结果必须等于原始帧单元，随后按
第 2.2 节解析。

### 4.4 SDP 示例

```sdp
m=video 5004 RTP/AVP 96
a=rtpmap:96 MLVC/90000
a=fmtp:96 mlvc-config=TUxWQzECAACAAgAAcAEAAB4AAAABAAAAFQAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA==
```

上例配置记录解码为：v2，640×368，30/1 fps，基准 q_index 21，
IDR 周期 64，LTR 关闭，恒 QP 模式。

## 5. 丢包语义与传输选型（强制性）

先区分两类"丢帧"：

- **带内丢帧标记**（编码端主动）：码控溢出时编码端产生
  `q_index=-1, size=0` 的 16B 标记记录，码流本身完整连续。
  解码端重复上一输出帧即可，**不触发错误恢复**。
- **传输层丢包**（被动）：RTP 序号空洞、重组失败、rANS
  `invalid stream`——按下述规则处理。

1. rANS 无容错：接收端检测到传输层丢包后，**必须丢弃其后全部
   非关键帧，直到收到关键帧（K=1 / flags.KEYFRAME）为止**，
   不得尝试续解。
2. 因此 MLVC 承载必须使用可靠传输，按优先级：
   - SRT（ARQ 重传，推荐跨公网）；
   - TCP（RTSP interleaved、GB28181 TCP 主动/被动）；
   - UDP 仅在受控链路可用，且编码端必须周期产生关键帧
     （建议间隔 ≤2s），接收端按第 1 条执行跳帧恢复。
3. **关键帧语义**：本规范中关键帧 = 不依赖任何解码历史的帧
   （IDR，解码端清空 DPB 与 LTR 槽）。编码端已实现周期 IDR
   （`rkvc_quality.gop_size` / 容器头 `iframe_period`）与运行中
   强制关键帧（`rkvc_encode_control.force_idr`，DPB/LTR 槽重置、
   flags.KEYFRAME=1）。
4. 晚加入：加入方必须持有配置记录并等待下一个关键帧。

## 6. GB/T 28181 映射

SIP 注册、保活、INVITE/ACK 与会话管理完全遵循 GB/T 28181-2016/2022，
本文不重新定义。媒体映射：

1. 媒体通道 = PS（MPEG-2 Program Stream）over RTP，PT=96，
   SDP 按国标 `a=rtpmap:96 PS/90000`，SSRC 经 `y=` 字段协商。
2. PS 封装：每个 pack 序列在关键帧边界必须以 system header + PSM 起始
   （保证平台侧随时可加入）；PSM ES 表项：
   `stream_type=0x06`（私有 PES）、`elementary_stream_id=0xBD`
   （private_stream_1），ES info 携带第 3 节的 'MLVC' 注册描述符与
   配置记录描述符。
3. 每个 PES 载荷 = 恰好一个帧单元（16B 记录头 + 载荷），PES 头
   PTS 按第 8 章填写。
4. 传输模式：优先 TCP 被动/主动；UDP 模式受第 5 章约束。
5. 与 H.265 双码流并存时，MLVC 通道与标准 H.265 通道（PSM
   `stream_type=0x24`）各自独立 SSRC，平台按能力选择。

## 7. MPEG-TS / SRT 映射

1. PMT：`stream_type=0x06`，PES `stream_id=0xBD`，ES info 携带
   'MLVC' 注册描述符 + 配置记录描述符；PMT 按 MPEG-2 systems
   惯例周期重复（≤500ms）。
2. PES 载荷 = 恰好一个帧单元；PTS 必填。
3. SRT：caller/listener 均可，承载标准 TS；延迟参数建议
   ≥ 4×RTT；加密走 SRT AES，不在本规范重复定义。
4. 任何不识 'MLVC' 的 SRT 网关/路由器应能透明转发（TS 层面
   完全合法），这是选择标准封装的直接收益。

## 8. 时钟与时间戳

- 所有封装统一 90 kHz 时钟。
- 推流应用应保证编码输入帧 `pts` 有效并明确其时基，mux 按其
  换算 90 kHz；`pts` 未知（`RKVC_FRAME_TS_UNKNOWN`）时，以首帧为 0，
  按配置记录 `fps_num/fps_den` 标称间隔单调递增生成。
- MLVC 无 B 帧概念，DTS 恒等于 PTS。

## 9. 互操作一致性要求

1. 编码端与解码端的 PMF 表（`pmf-gaussian`/`pmf-bitest`）、模型
   结构与 QP 必须同源；任何不一致在解码端表现为 rANS
   `invalid stream`，接收端必须将其视为**会话级致命错误**并
   终止会话，而非帧级错误。
2. 跨实现/跨设备解码必须使用 FP32 参考实现（已验证：RKNN FP16
   与 torch FP16 交叉解码均不可行，详见仓库验收记录）；本规范的
   一致性测试以官方 FP32 参考解码器为准绳。
3. 版本协商：配置记录 version=2 为当前版本（v1 仅读取兼容）；
   后续版本必须更新 magic/version 并在本文增补章节，不允许静默扩展。

## 10. 不推荐路径

- **RTMP / Enhanced RTMP**：无 MLVC fourcc 生态，平台支持为零。
- *格式标识：配置记录 @5 的 format 字节（0x02）是本格式的固定
   标识，不做版本协商；格式演进必须同步更新该字节并在本文增补
   1）后进入。
- **裸 UDP + 自定义 FEC**：仅在第 5 章约束下的受控链路临时使用，
  不作为对外交付形态。

## 11. 实现状态矩阵

| 项                               | 状态   | 位置                                               |
| -------------------------------- | ------ | -------------------------------------------------- |
| 容器头/帧记录读写、流式 demux    | 已实现 | `codecs/mlvc/src/container.cpp`（格式字节 `0x02`） |
| 首帧关键帧 + KEYFRAME 标志       | 已实现 | `codecs/mlvc/src/encoder.cpp`（P-only，首帧关键帧） |
| 周期 IDR（`quality.gop_size`）   | 已实现 | `codecs/mlvc/src/encoder.cpp`                     |
| LTR 长期参考（MARK/RECOVERY）    | 已实现 | `codecs/mlvc/src/encoder.cpp`、容器记录 flags      |
| 闭环码控（CBR，官方算法移植）    | 已实现 | `codecs/mlvc/src/ratectl.cpp`（golden 对拍一致）   |
| 编码端主动丢帧（标记记录）       | 已实现 | `codecs/mlvc/src/encoder.cpp`                     |
| 解码端丢帧重复上一帧             | 已实现 | `codecs/mlvc/src/decoder.cpp`                      |
| 逐帧 q_index（rung 多上下文）    | 已实现 | `codecs/mlvc/src/codec.cpp`（QPPATCH 集合）        |
| RTP 打包/解包（第 4 章）         | 未实现 | 规划独立插件                                       |
| SDP fmtp 参数传递                | 未实现 | 同上                                               |
| PS 封装与 GB28181 信令           | 未实现 | 规划独立插件                                       |
| TS/SRT 封装                      | 未实现 | 本仓无容器后端（旧 FFmpeg demux/mux 已删），需外部封装 |
| `RKVC_ENDPOINT_STREAM` 端点           | 未实现 | C ABI 枚举已声明，管线未实现           |
