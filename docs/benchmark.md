# 基准测试

## RD 端到端对比（码率-画质）

RK3588 上 H.264 / HEVC / SVT-AV1 / rkvc Session 的端到端 RD 与性能对比，见 **[bench/README.md](../bench/README.md)**。

```bash
./scripts/run-bench.sh /path/to/1080p.mp4
PLOT_ONLY=1 ./scripts/run-bench.sh
RUN_CODECS=h264,rkvc-v2 ./scripts/run-bench.sh clip.mp4
```

默认对比路线：`h264`、`h265`、`svt-av1`、`rkvc-v2`（展开为 realtime / balanced / quality 三档）。

## rkvc_bench（Session E2E）

v2 的 `rkvc_bench` 对同一输入文件分别跑 `REALTIME` / `BALANCED` / `QUALITY` 三档 policy 的完整转码管线，输出 E2E fps。

```bash
./build/rkvc_bench -i clip.mp4
./build/rkvc_bench -i clip.mp4 -o /tmp/bench_out -s 1920x1080
```

须通过 `-i` 指定输入容器；可先运行 `example_encode_file -o test.mp4` 生成短片段。

### 输出示例

```
rkvc v2 session E2E bench (input=clip.mp4)
  REALTIME (H.264): 36.2 fps
  BALANCED (HEVC):  27.1 fps
  QUALITY (AV1):    24.3 fps
```

### RK3588 实测 (1080p E2E 转码)

| policy | 路线 | E2E fps |
|--------|------|---------|
| `REALTIME` | H.264 RKMPP | ~36 |
| `BALANCED` | HEVC RKMPP | ~27 |
| `QUALITY` | SVT-AV1 + av1_rkmpp | ~24 |

> v0.1.x 的 `--quick` / `--stream` / `--encode-only` 等选项已在 v2 移除；吞吐细分测试请使用 RD 套件或示例程序。

## 端到端延迟测试

`latency_test` 模拟摄像头实时采集，经编码 → 解码全链路，测量每帧端到端延迟。

```bash
cd build

# 低延迟模式 (推荐)
./example_latency_test -l

# 自定义参数
./example_latency_test -s 1280x720 -r 60 -n 600 -b 8000000 -l
```

输出逐帧延迟明细和统计摘要（平均、P50、P95、P99）。详见示例源码 `examples/latency_test.c`。

### RK3588 延迟实测 (1080p, 低延迟模式)

| 指标 | 编码延迟 | 端到端延迟 |
| ---- | -------- | ---------- |
| 平均 | ~7 ms | ~69 ms |
| P50 | ~7 ms | ~76 ms |
| P95 | ~8 ms | ~84 ms |
| 最大 | ~8 ms | ~111 ms |

- **编码延迟**: 采集帧生成到编码包输出
- **端到端延迟**: 采集帧生成到解码帧输出，含编解码器硬件流水线
- 帧 0 延迟偏高 (~110ms) 为解码器初始化开销

## PSNR 质量测试

```bash
./build/example_psnr_test -i input.mp4
./build/example_psnr_test -i input.mp4 -v -n 100
```

输出 Y/U/V 平均 PSNR、加权平均 PSNR 及最低帧 PSNR。

## 下采样 + 后处理上采样基准

评估低分辨率编码 + 上采样还原的画质损失：

```bash
RUN_CODECS=svt-av1,post-upscale ./scripts/run-bench.sh clip.mp4
ENC_SCALE_DENOM=2 UPSCALE_ALGOS=nearest,bilinear,bicubic \
  RUN_CODECS=post-upscale ./scripts/run-bench.sh clip.mp4
ENC_SCALE_DENOM=3 UPSCALE_ALGOS=bilinear,rkvc_sr \
  RUN_CODECS=post-upscale ./scripts/run-bench.sh clip.mp4
```

Session 字段：`enc_scale_denom`、`post_upscale_algo`。CLI 请用 `rkvc_session_upscale --enc-scale-denom N --post-upscale …`（**不是** `rkvc_encode --post-upscale`；编码工具只做下采样）。

独立 RGA 批处理（不经 Session）可用 `rkvc_yuv_upscale` 或 `rkvc_upscale_ctx_*` API；`rkvc_session_get_stats()` 的 `rga_sec` / `postproc_sec` 反映 Session 路径上的后处理耗时。
