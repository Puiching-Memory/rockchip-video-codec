# RK3588 RD 基准测试报告

> **平台**：RK3588 · **素材**：1080p 片段（默认从中间截取 4s）  
> **路线**：H.264 / H.265 / SVT-AV1 / **rkvc Session 三档** / post-upscale（可选）

## 核心结论

| 场景 | 推荐 | E2E fps（参考） |
|------|------|----------------|
| 实时（直播/监控） | rkvc `realtime` 或 H.264 RKMPP | **~62–69** |
| 画质均衡 | rkvc `balanced` 或 H.265 RKMPP | **~56** |
| 新一代存储 | rkvc `quality` 或 SVT-AV1 + av1_rkmpp | **~24–28** |

rkvc Session 三档分别路由到 H.264 / HEVC / AV1，RD 曲线可与底层 codec 基线直接对照。

## 复现

```bash
./bench/run_rd_benchmark.sh /path/to/video.mp4
```

输出：`bench/results/rd_data.csv`、`rd_curve_e2e.png`、`perf_e2e.png`。

## 项目结构

```
bench/
├── run_rd_benchmark.sh   # 主编排
├── plot_rd_curve.py
├── plot_perf.py
├── results/              # CSV + 图表
└── work/                 # 中间产物（gitignore）
```

入口脚本：`bench/run_rd_benchmark.sh`

详见 [README.md](README.md)。
