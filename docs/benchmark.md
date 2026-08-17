# 基准测试

## RD 端到端对比（码率-画质）

RK3588 上 H.264 / HEVC / SVT-AV1 / rkvc Session 的端到端 RD 与性能对比，见 **[bench/README.md](../bench/README.md)**。

```bash
./bench/run_rd_benchmark.sh /path/to/1080p.mp4
PLOT_ONLY=1 ./bench/run_rd_benchmark.sh
RUN_CODECS=h264,rkvc ./bench/run_rd_benchmark.sh clip.mp4
```

默认对比路线：`h264`、`h265`、`svt-av1`、`svt-av1-hq`、`rkvc`（展开为 realtime / balanced / quality / offline 四档）。

## rkvc_bench（Session E2E）

`rkvc_bench` 对同一输入文件分别跑 `REALTIME` / `BALANCED` / `QUALITY` / `OFFLINE` 四档 policy 的完整转码管线，输出 E2E fps。

```bash
./.build/release/rkvc_bench -i clip.mp4
./.build/release/rkvc_bench -i clip.mp4 -o /tmp/bench_out -s 1920x1080
```

须通过 `-i` 指定输入容器；可先运行 `example_encode_file -o test.mp4` 生成短片段。

### 输出示例

```
rkvc session E2E bench (input=clip.mp4)
  REALTIME (H.264): 36.2 fps
  BALANCED (HEVC):  27.1 fps
  QUALITY (AV1):    24.3 fps
  OFFLINE (AV1 HQ):  2.2 fps
```

### RK3588 实测 (1080p E2E 转码)

| policy     | 路线                    | E2E fps          |
| ---------- | ----------------------- | ---------------- |
| `REALTIME` | H.264 RKMPP             | ~36              |
| `BALANCED` | HEVC RKMPP              | ~27              |
| `QUALITY`  | SVT-AV1 p11 + av1_rkmpp | ~24              |
| `OFFLINE`  | SVT-AV1 p4 + av1_rkmpp  | ~2（非实时，≥1） |

> 吞吐细分测试请使用 RD 套件或示例程序。

## Session 创建耗时

`example_latency_test` 测量 `rkvc_session_create()` 耗时（无采集、无编解码、无 CLI 参数）：

```bash
./.build/release/example_latency_test
```

输出一行 `session_create: N.NN ms`。端到端帧延迟请用 `example_live_capture` / Session 端口自行打点，或看 `rkvc_bench` 的 E2E fps。

## 下采样 + 后处理上采样基准

评估低分辨率编码 + 上采样还原的画质损失：

```bash
RUN_CODECS=svt-av1,post-upscale ./bench/run_rd_benchmark.sh clip.mp4
ENC_SCALE_DENOM=2 UPSCALE_ALGOS=nearest,bilinear,bicubic \
  RUN_CODECS=post-upscale ./bench/run_rd_benchmark.sh clip.mp4
ENC_SCALE_DENOM=3 UPSCALE_ALGOS=bilinear,rkvc_sr \
  RUN_CODECS=post-upscale ./bench/run_rd_benchmark.sh clip.mp4
```

Session 字段：`enc_scale_denom`、`post_upscale_algo`。CLI 请用 `rkvc_session_upscale --enc-scale-denom N --post-upscale …`（编码工具 `rkvc_encode` 只做下采样，不接受 `--post-upscale`）。

独立 RGA 批处理（不经 Session）可用 `rkvc_yuv_upscale` 或 `rkvc_upscale_ctx_*` API；`rkvc_session_get_stats()` 的 `rga_sec` / `postproc_sec` 反映 Session 路径上的后处理耗时。
