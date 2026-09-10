# rkvc 性能基准

已完成的 RK3576 / UVG-7 实测包含 532 个评分点、可重绘快照和 SR 配对结果；
原始数据见 [uvg-rk3576-20260908.csv](../../docs/data/uvg-rk3576-20260908.csv)，
曲线图见 `docs/images/bench/`；源码版本与采样条件随报告记录。

## RD 实测与报告

`rd.py` 在板上调用当前 rkvc CLI；`plot_rd.py` 在主机生成浅色报告、PNG 和
SVG。页面与图表采用中文；绘图主机需安装微软雅黑、Noto Sans CJK SC、黑体或
文泉驿正黑等中文字体。四种编码器使用固定配色，原生编码与重建消融分图展示，避免十几条曲线
挤在同一张图中。HTML 可离线浏览，包含配置、板卡信息、模型哈希和失败列表。

测试矩阵为：

| 对比           | 输入 / 评价尺寸       | 系列                                               |
| -------------- | --------------------- | -------------------------------------------------- |
| 低分辨率编码   | 640×360 / 640×360     | MLVC、MPP H.264、MPP H.265、SVT-AV1                |
| 1080p 编码参照 | 1920×1080 / 1920×1080 | MPP H.264、MPP H.265、SVT-AV1                      |
| 3× 重建消融    | 640×360 / 1920×1080   | 同一码流解码后分别 bicubic、Lanczos、Phase-RLFN SR |

MLVC 不要求 1080p 模型。固定模型需要 640×368 时，编码前只在右侧／底部做
edge padding，解码后裁回 640×360；补边区域不参与失真计算。补边时间与裁剪时间
计入阶段耗时。SR 必须显式指定模型，输出帧数与 3× 几何严格校验；2× RGA 回退
不能成为有效 SR 点。

传统 bicubic 使用 FFmpeg 的 B=0、C=0.75，与当前 SR 后端 CPU bicubic 基座的
Keys 核 a=−0.75 对齐；两种实现的舍入／色度处理仍可能略有差异，因此差值代表
完整重建路径效果。当前 SR 是 NPU 残差网络＋CPU 基座／融合，并非全 NPU 或
全 RGA 重建。`cpu_affinity: [4,5,6,7]` 可将本次测量进程及子进程限制到指定
CPU，不会更改系统 governor。

### 数据集

默认使用 [UVG 官方](https://ultravideo.fi/dataset.html)经典七序列的 1080p
8-bit YUV420 原始视频：Beauty、Bosphorus、HoneyBee、Jockey、ReadySetGo、
ShakeNDry、YachtRide。UVG、HEVC、MCL-JCV 也是
[Microsoft 神经视频编码测试条件](https://github.com/microsoft/DCVC/blob/main/test_conditions.md)
列出的常用测试集。这里固定每序列前 96 帧，保留原始 120 fps 时间轴；这不是
完整序列或 JVET CTC 测试。官方素材采用 BY-NC 授权，使用时遵循来源条款并引用
Mercat 等人的 UVG 数据集论文。

主机准备（下载约数 GB，需 `py7zr`；缓存支持分块续传）：

```bash
uv pip install -r tools/bench/requirements-host.txt
python tools/bench/prepare_uvg.py --cache .temp/uvg-cache --output .temp/uvg
```

输出截取后的 I420 文件和 `rd.prepared.json`，记录归档及前 96 帧的 SHA-256。
可用 `--sequence ShakeNDry` 验证单序列流程，输出会明确标为子集。把整个输出
目录复制到板上；配置中的媒体路径相对配置文件解析。调整 `rkvc`、`ffmpeg`、
`sr_model` 和 `mlvc_model` 为板上实际值。

```bash
python3 tools/bench/rd.py --config /data/uvg/rd.prepared.json --output /data/rd-run-001 --dry-run
python3 tools/bench/rd.py --config /data/uvg/rd.prepared.json --output /data/rd-preflight --preflight
python3 tools/bench/rd.py --config /data/uvg/rd.prepared.json --output /data/rd-run-001
# 将 rd-run-001/rd.json 复制回主机，绘图依赖项目中的 matplotlib
python tools/bench/plot_rd.py /path/to/rd.json --output .temp/rd-report
```

输出目录必须不存在，以免覆盖已有实测。每点完成后落盘 JSON/CSV，错误点保留
原因并继续其他点；有失败时返回非零。默认删除生成的临时媒体，保留命令日志、
哈希和指标，原始输入不删除；`keep_media: true` 用于逐帧排查。
先用 `--preflight` 检查首序列两帧的全部 codec/QP/重建路径；报告会显式标为
PREFLIGHT ONLY，不能用来代替正式 RD 测试。

长跑建议把 stdout/stderr 重定向到日志并用 `nohup` 脱离 SSH 会话。中断后使用
相同配置和输出目录追加 `--resume`：会核对板卡、程序／库／模型哈希、FFmpeg
版本和 CPU affinity，跳过完整点，重新运行不完整点。原报告先备份，失败历史和
各次会话的温度、时间保留；配置或模型变化时拒绝续跑，不覆盖原报告。

### 测量口径

- 码率取完整输出码流字节数（含容器／头部）×8÷源序列时长，不能用目标 bitrate
  或网络估计熵替代。QP 数值不跨编码器等价；默认每种七档，按**实际码率**排序。
  H.264/H.265：22、27、32、37、42、47、51；AV1：24、32、40、48、54、59、63；
  MLVC：0、12、21、30、39、48、63。更大的 MLVC QP 对应更高质量，方向与传统编码器不同。
- MLVC 每个 QP 必须使用对应折叠模型或正确的 QPPATCH；只改 `--qp` 而继续使用
  单档折叠模型不构成有效多档对比。示例通过 `mlvc_models` 将每个 QP 映射至
  独立模型 ID，encoder/decoder 同 ID、不同 role，均携带匹配 PMF。
- `classical_decoder: "ffmpeg-rkmpp"` 使用板端 FFmpeg 的 h264/hevc/av1_rkmpp
  硬解、显式原始码流 demux 和 `hwdownload`，无软件解码回退。编码仍调用 rkvc。
  设为 `"rkvc"` 可测统一 CLI 解码路径。每条结果记录实际 decoder；比较时不要
  混用这两种路径的吞吐。AV1 编码输出是 OBU 流，按 `-f obu` 读取。
- Y-PSNR 先池化逐帧 luma MSE 再转 dB，Y-SSIM 为逐帧算术平均；不是 MS-SSIM。
  参考与重建保持同样的 NV12 数值域，不进行 RGB 往返，检查完整帧数量。
- 每个序列单独绘制，不把不同内容的点连成一条曲线，也不把不重叠的质量区间
  外推成 BD-rate。总览仅在该点包含全部配置序列时汇总：池化 luma MSE、平均码率
  和 SSIM（等长序列）；原生 1080p 与低分辨率直接评分使用不同参考，分面展示。
- 默认 1 次进程预热、3 次采样。吞吐包括进程启动、模型初始化和文件 I/O；
  stage-sum 为单独测量的缩小、编码、解码、重建阶段相加，**不是流水线实测延迟**。
  像素指标计算不计入吞吐。实时倍速为 fps / 源 fps，越大越快。
- 新配置显式设置 `gop: 64`，编码 CLI 统一传入 `--gop 64 --fps 120`。
  以下为 2026-09-08 存档实测所用的编码器设置（历史记录，非当前 `rd.py`
  行为——当前 AV1 经 rkvc CLI 编码，ffmpeg 仅用于像素指标与 GOP 核验）：
  MPP 使用固定 GOP 的 I/P 编码；SVT 使用 LOW_DELAY、闭合 GOP（intra_period_length=63），
  关闭场景切换关键帧检测；MLVC 关闭 LTR。各算法的参考帧数量与量化策略仍不同，
  这不是完整 CTC 或统一软件编码器排名。AV1 为板上 CPU 编码。
- 每条实测码流必须通过 `gop_audit`：H.264/H.265 用 FFprobe 逐帧解析，AV1 用
  `trace_headers` 读取 frame_type，MLVC 读取格式 2 的帧记录；96 帧应在第 0、64 帧
  出现关键帧。不通过的点不会作为有效曲线。核验不计入吞吐。
- 旧报告若没有 `gop` 字段，保留原来的后端默认设置并明确标注，不能与新协议混合汇总。
- 固定板卡、散热、负载和 governor 后再做性能结论；报告保留运行前后温度及
  governor、程序／模型哈希、FFmpeg 版本、逐条实际命令。

SR 权重导出及 RKNN 契约见 [SR 文档](../../docs/sr-model-yuv-spec.md)。
FP16 导出模型的宿主属性可以是浮点类型；运行时仍以 UINT8 NHWC、
`pass_through=0` 输入，由 RKNN 转换。不能因属性不是 UINT8 就静默回退到 RGA。

这里存放面向 Rockchip 实机的性能基准，不属于单元测试。`benchmark.py` 包裹
当前统一 CLI 的 `decode`、`encode` 命令，以独立进程运行预热和
多轮采样，结果写成 JSON 与 CSV。脚本只使用 Python 标准库，便于随可移植包
复制到板卡。

## 单项基准

~~~bash
python3 tools/bench/benchmark.py \
  --rkvc .build/release/rkvc \
  --operation decode --codec h264 \
  --input media/sample-1080p.h264 \
  --width 1920 --height 1080 \
  --frames 300 --duration-seconds 10 \
  --warmup 1 --iterations 5
~~~

`--frames` 用于计算 FPS，`--duration-seconds` 用于计算实时倍速。新 CLI 的
decode 要求 `--width/--height` 作为输出几何，因此单项 decode 也必须提供，
同时用于 MP/s 统计；
encode 输入被视为连续 NV12，提供宽高后，若文件大小可整除单帧字节数，帧数会
自动推导。

## 基准矩阵

复制示例配置并把媒体路径、帧数及性能门槛改为目标板卡的固定测试集：

~~~bash
cp tools/bench/config.example.json tools/bench/config.local.json
python3 tools/bench/benchmark.py --config tools/bench/config.local.json
python3 tools/bench/benchmark.py --config tools/bench/config.local.json \
  --case decode-h264-1080p
~~~

相对路径以配置文件所在目录为基准。配置中的 `output` 只是临时输出文件名；
每轮运行前都会清除，默认在系统临时目录中生成，不覆盖用户媒体。需要排查输出
时可加 `--keep-work`。

每项可配置以下门槛，任一失败则进程返回 1，适合板卡性能回归门禁：

- `min_fps`
- `min_realtime`
- `max_mean_seconds`

运行记录默认写入 `tools/bench/results/benchmark-<时间>.json` 和 `.csv`，其中
包含逐轮数据、mean/median/min/max/p95/stdev、机器信息及 Git revision。
提交配置前可使用 `--dry-run` 校验并打印最终命令。
