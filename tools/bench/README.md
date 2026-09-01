# rkvc 性能基准

这里存放面向 Rockchip 实机的性能基准，不属于单元测试。`benchmark.py` 包裹
当前统一 CLI 的 `decode`、`encode`、`transcode` 命令，以独立进程运行预热和
多轮采样，结果写成 JSON 与 CSV。脚本只使用 Python 标准库，便于随可移植包
复制到板卡。

## 单项基准

~~~bash
python3 tools/bench/benchmark.py \
  --rkvc .build/release/rkvc \
  --operation decode --codec h264 \
  --input media/sample-1080p.h264 \
  --frames 300 --duration-seconds 10 \
  --warmup 1 --iterations 5
~~~

`--frames` 用于计算 FPS，`--duration-seconds` 用于计算实时倍速。decode 的
`--width/--height` 只参与 MP/s 统计，不会传给 CLI，因此不会意外引入缩放；
encode 输入被视为连续 NV12，提供宽高后，若文件大小可整除单帧字节数，帧数会
自动推导。transcode 的宽高则表示请求的输出尺寸。

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
