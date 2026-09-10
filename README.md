# rockchip-video-codec

rkvc 是面向 Rockchip Linux 的 C++20 会话式媒体运行库（无异常、无 RTTI）。
它只保留**一个稳定 C ABI**（当前 0.5.0：context / session / frame /
diagnostic）、**一个核心静态库**（`rkvc-core-static`）、**四个 codec 插件
工程**、**一个 pspack 静态库**与**一个 `rkvc` CLI**。媒体实现经
`rkvc_plugin_query` 握手（`kPluginAbi=1` + 工具链指纹）接入，核心不链接
MPP、RGA、RKNN 或 SVT 类型。

## 能力

- **核心**（`core/`）：会话规划与执行、有界队列背压、EOS / 取消 / flush、
  FILE 与 FRAME_SINK 双端点、RKMDL1 模型容器与注册表、文本诊断
- **h264h265**（`codecs/h264h265/`）：MPP H.264 / HEVC 硬编码 + 硬解码
- **av1**（`codecs/av1/`）：SVT-AV1 软件编码
- **mlvc**（`codecs/mlvc/`）：NPU 神经视频编解码，`.mlvc` 容器（流格式字节
  `0x02`），P-only，无 B 帧
- **sr**（`codecs/sr/`）：Phase-RLFN 固定 3× NPU 超分（NHWC 输入 / NCHW
  输出，以 `core_w` / `core_h` 拆分非方形）
- **pspack**（`codecs/pspack/`）：GB28181 PS 打包与解包（pack / system /
  PSM / PES，AU 组装），静态库，无插件入口
- **CLI**（`cli/`）：`caps` / `version` / `inspect` / `encode` / `decode` /
  `upscale`，长选项，无转码、无 `-i/-o` 短选项、无 low-delay 开关
  （MLVC 天生 P-only，MPP 默认无 B 帧）

MLVC 编码/解码链路：

![MLVC 架构](docs/images/mlvc-architecture.png)

## 构建

要求 Linux、CMake 3.21+、C++20 编译器、Ninja、pthread 与 dl。
顶层是薄聚合（core + codecs + cli + CTest），每个子工程也可独立配置
（codec 工程回退 `add_subdirectory(core)`）。

~~~bash
cmake --preset default
cmake --build --preset default

./.build/release/rkvc version
./.build/release/rkvc caps
./.build/release/rkvc inspect backends
./.build/release/rkvc inspect models
~~~

默认产物位于 `.build/release/`：`rkvc`（CLI）与四个 `rkvc_*.so` 插件。
预设只有三个：`default`、`debug`、`tests`（后者多开
`RKVC_CORE_BUILD_TESTS=ON`）。顶层开关只有两个：

| 选项                | 默认 | 说明                               |
| ------------------- | ---- | ---------------------------------- |
| `RKVC_BUILD_CLI`    | ON   | 构建 `rkvc` CLI                    |
| `RKVC_BUILD_CODECS` | ON   | 构建 codec 工程（4 插件 + pspack） |

各 codec/test 工程的测试开关（`RKVC_*_BUILD_TESTS`）默认全开，core 的
`RKVC_CORE_BUILD_TESTS` 默认关闭、由 `tests` 预设或 CI 打开。
aarch64 链接自动加 `-static-libstdc++ -static-libgcc`，产物审计见
`tools/check-symbols.sh`（GLIBC ≤ 2.34、禁动态 C++ 运行时；只在板端有意义，
x86 构建产物天然动态链接 libstdc++，不跑此审计）。

MPP / SVT / RKNN 等第三方依赖以前缀方式提供，见各 codec 工程的
`CMakeLists.txt` 与 `third_party/` 说明；在 x86 本机只能构建与测试纯软路径
（SVT 编码、MLVC 算法表、SR 后处理），MPP / NPU 路径须上板验证。

## CLI

`rkvc` 把长选项翻译成 session 请求后调用 C ABI，输入输出为后端可直接
消费的裸帧或 elementary stream（mlvc 为 `.mlvc` 容器）：

~~~text
  rkvc caps [--backend-dir DIR]...
  rkvc version [--json]
  rkvc inspect backends|models [--backend-dir DIR]...
            [--model-dir DIR]... [--json]
  rkvc encode --codec h264|hevc|av1 --input IN --width W --height H
            --pixfmt nv12|yuv420p --output OUT [--backend-dir DIR]...
            [--model FILE]... [--model-dir DIR]... [--model-id ID]
            [--qp Q] [--bitrate BPS] [--gop G] [--fps N]
  rkvc decode --codec mlvc --input IN.mlvc --width W --height H
            --pixfmt nv12|yuv420p --output OUT [--backend-dir DIR]...
            [--model FILE]... [--model-dir DIR]... [--model-id ID]
  rkvc upscale --input IN --width W --height H
            --pixfmt nv12|yuv420p --output OUT [--backend-dir DIR]...
            [--model FILE]... [--model-dir DIR]... [--model-id ID]
~~~

- `encode` 的 `--codec` 另接受 `mlvc`（NPU，需 `--model-id` 选编码模型）；
  `--qp` 为量化档，`--gop`/`--fps` 显式控制关键帧周期与帧率。
- `decode --width/--height` 取**输出几何**（裸帧尺寸；`encode` 则取输入几何）。
- `upscale` 的 `--width/--height` 为输入尺寸，输出固定 3×；`--model-id` 选
  已注册的 RKMDL1（如 `phase-rlfn-bench`），模型 ID 即导出 stem。
- `--model FILE` 逐个注册 RKMDL1，`--model-dir DIR` 扫描目录注册。

## 示例

三个独立 C 示例（各带 `CMakeLists.txt`，以 `RKVC_CORE_DIR` 指向
`core/` 源码、直链 `rkvc-core-static`；内嵌形态见
[语义编解码 SDK 集成](docs/semantic-codec-sdk-integration.md)）：

| 示例                      | 端点            | 说明                                                         |
| ------------------------- | --------------- | ------------------------------------------------------------ |
| `examples/integration-c/` | FRAME_SINK 流式 | AV1 编码：wrap → push/try_pull 背压 → push_eos → pull 至 EOF |
| `examples/decode-file/`   | FILE            | 解码裸码流/` .mlvc` 到裸帧（与 `rkvc decode` 同参数形状）    |
| `examples/upscale-file/`  | FILE            | 固定 3× 超分（与 `rkvc upscale` 同参数形状）                 |

## API

~~~c
#include <rkvc/rkvc.h>

rkvc_context *ctx = NULL;
rkvc_context_options opts;
rkvc_context_options_init(&opts, sizeof(opts));
rkvc_context_create(&opts, &ctx);

rkvc_session_request req;
rkvc_session_request_init(&req, sizeof(req));
req.operation = RKVC_OP_DECODE;
req.codec = RKVC_CODEC_MLVC;
req.input.kind = RKVC_ENDPOINT_FILE;
req.input.uri = "clip.mlvc";
req.input.fmt = RKVC_FRAME_FMT_BITSTREAM;
req.input.width = 640;
req.input.height = 360;
req.output.kind = RKVC_ENDPOINT_FILE;
req.output.uri = "clip.nv12";
req.output.fmt = RKVC_FRAME_FMT_NV12;
req.output.width = 640;
req.output.height = 360;

rkvc_session *s = NULL;
rkvc_diagnostic *diag = NULL;
rkvc_session_create(ctx, &req, &s, &diag);
rkvc_session_start(s, &diag);
rkvc_session_wait(s); /* FILE 会话：等待整条管线跑完 */
rkvc_session_destroy(s);
rkvc_context_destroy(ctx);
~~~

流式用法（FRAME_SINK：wrap → push / try_pull / pull → push_eos）见
`examples/integration-c/main.c`。全部声明在
[core/include/rkvc/rkvc.h](core/include/rkvc/rkvc.h)，
语义见 [docs/architecture.md](docs/architecture.md)。注意所有结构体都走
`xxx_init(&x, sizeof(x))` 版本优先初始化，`struct_size` 不匹配即拒收。

## 测试

- **C++**（doctest 2.4.11，`DOCTEST_CONFIG_NO_EXCEPTIONS`，随各工程构建、
  CTest 执行）：core（status/result、spec/frame、rkmdl1、queue、plugin、
  pipeline、C ABI）7 套；mlvc（tables、ratectl、rans、pixel、codec）5 套；
  av1、sr 后处理、h264h265 逻辑、CLI 参数各 1 套
- **Python**（`tests/python/`，`unittest`）：rd 核算、`benchmark.py` 命令生成、
  RKMDL1 容器、MLVC / SR 导出
- **Bash**（`tests/bash/`）：MLVC / 超分导出链路入口（优先 `.venv` 回退
  `python3`，不依赖调用者 cwd）

~~~bash
cmake --preset tests && cmake --build --preset tests
ctest --test-dir .build/tests --output-on-failure
python3 -m unittest discover -s tests/python -p 'test_*.py'
~~~

MPP / NPU 硬件路径须在 Rockchip 板卡验证；x86 只覆盖加载、CLI 与纯软路径。
详见 [docs/testing.md](docs/testing.md)。

## 性能基准

`tools/bench/` 是纯标准库工具（`rd.py` / `benchmark.py`，新 CLI 长选项协议，
自带 GOP 审计与显式逐 QP 模型 ID），在板端跑 UVG RD 与性能采样：

~~~bash
python3 tools/bench/benchmark.py --config tools/bench/rd.uvg.json
~~~

配置模板、矩阵与报告方法见 [tools/bench/README.md](tools/bench/README.md)。
`tools/bench/results/` 下的历史 `rd.json` 是旧协议归档，不代表当前版本性能。

![UVG 低分辨率编码 RD](docs/images/bench/uvg-rk3576-20260908-low_native-codecs.png)

## 模型

RKMDL1 容器（魔数 `RKMDL1\x00\x00`，128B 头 + 88B 条目 + 多 qppatch 载荷；
全仓版本 1 政策唯一例外是 `.mlvc` 流字节 `0x02`），由注册表从可信目录或
`--model FILE` 显式加载，失败只淘汰候选。NPU 侧 I/O 契约：输入 NHWC、
输出 NCHW。模型 ID 即导出 stem（如 `mlvc_rk3576_qp21_encoder`、
`phase-rlfn-bench`），CLI 与 bench 配置都用它经 `--model-id` 选模型。
布局与导出见 [docs/mlvc-rknn-export.md](docs/mlvc-rknn-export.md) 与
[docs/sr-model-yuv-spec.md](docs/sr-model-yuv-spec.md)。

## 发布

本仓**没有打包器、不设 install 规则**：发布产物就是构建树
（`rkvc` + `rkvc_*.so` 插件 + 所需 `.rkmdl`）。板端部署 = 复制这三样并用
`--backend-dir/--model-dir`（或 `--model FILE`）指向它们；aarch64 产物另跑
`tools/check-symbols.sh` 做 GLIBC 与 C++ 运行时审计。依赖（MPP / rknnrt /
SVT）来自目标机系统路径或随包复制的前缀目录，由运行时链接器解析。
历史可复现打包流程（rkvc-build / portable / SBOM）已随旧 C 树删除。

## 文档

- [快速开始](docs/getting-started.md) · [架构](docs/architecture.md) ·
  [部署](docs/deployment.md) · [测试](docs/testing.md)
- [MLVC RKNN 导出](docs/mlvc-rknn-export.md) ·
  [SR 模型规格](docs/sr-model-yuv-spec.md) ·
  [语义编解码 SDK 集成](docs/semantic-codec-sdk-integration.md)

## 许可

项目按 [LICENSE](LICENSE) 中的 AGPL-3.0-or-later 条款发布。
