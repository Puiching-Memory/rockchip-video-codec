# 快速开始

要求 Linux、CMake 3.21+、C++20 编译器、Ninja、pthread 和 dl。
x86 本机只能验证纯软路径；MPP / NPU 路径须上板。

~~~bash
cmake --preset default
cmake --build --preset default
~~~

默认产物在 `.build/release/`：`rkvc` CLI 与 `rkvc_h264h265.so`、
`rkvc_mlvc.so`、`rkvc_av1.so`、`rkvc_sr.so` 四个插件。

~~~bash
.build/release/rkvc version
.build/release/rkvc caps --backend-dir .build/release
.build/release/rkvc inspect backends --backend-dir .build/release
.build/release/rkvc inspect models --backend-dir .build/release \
    --model-dir /path/to/models
~~~

`caps` 输出 `soc / mpp_enc / mpp_dec / rknn / npu_cores` 一行；
x86 本机 mpp/rknn 项为 0 属正常，只代表探测结果，不代表插件缺失。

编一个裸帧文件（以 SVT AV1 软编码为例，x86 可跑）：

~~~bash
head -c $((640*360*3/2)) /dev/urandom > in.nv12
.build/release/rkvc encode --codec av1 --input in.nv12 --width 640 \
    --height 360 --pixfmt nv12 --output out.obu \
    --backend-dir .build/release --qp 32 --gop 64 --fps 30
~~~

MLVC 与超分需要 NPU 与已注册的 RKMDL1（`--model FILE` 或
`--model-dir DIR`，`--model-id` 按导出 stem 选择），见
[RKNN 导出](mlvc-rknn-export.md)。

各子工程可独立配置（codec 工程自动回退 `add_subdirectory(core)`），
顶层只有 `RKVC_BUILD_CLI` / `RKVC_BUILD_CODECS` 两个开关。
测试与基准见 [测试](testing.md)，目录约定见 [构建目录](build-layout.md)。
