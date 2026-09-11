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

MLVC 与超分需要 NPU 与已注册的模型目录（`--model DIR`，`--model-id`
按导出 stem 选择），见
[MLVC RKNN 导出](mlvc-rknn-export.md)。

## 构建目录与开关

`.build/` 下只有三个预设目录（见 `CMakePresets.json`）：

| 路径              | 内容                                                             |
| ----------------- | ---------------------------------------------------------------- |
| `.build/release/` | `default` 预设：Release 构建（`rkvc` + `rkvc_*.so`）             |
| `.build/debug/`   | `debug` 预设：Debug 构建                                         |
| `.build/tests/`   | `tests` 预设：Debug + `RKVC_CORE_BUILD_TESTS=ON`，`ctest` 在此跑 |

各子工程可独立配置：codec 工程的 `rkvc::core` 按"仓库内 `core/` →
`-DRKVC_CORE_DIR=<core 目录>` → `find_package(rkvc CONFIG)`"三级解析，公共逻辑
收在 `cmake/RkvcCodec.cmake`，所以脱离仓库布局也能配置。顶层只有
`RKVC_BUILD_CLI` / `RKVC_BUILD_CODECS` 两个开关；各 codec 与测试工程的
`RKVC_*_BUILD_TESTS` 默认全开，core 的 `RKVC_CORE_BUILD_TESTS` 默认关闭，
由 `tests` 预设或 CI 打开。

默认不安装、不产安装树；`RKVC_INSTALL_SDK=ON` 时才装 SDK 面（core 与各 codec
的公开头文件 + 动态库 + CMake config + pkg-config），布局与用法见
[可移植包 × 宿主集成](portable-package.md)。板端部署见 [部署](deployment.md)；
测试与基准见 [测试](testing.md)。
