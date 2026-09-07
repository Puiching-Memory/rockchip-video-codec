# rockchip-video-codec

rkvc 是面向 Rockchip Linux 的 C17 媒体图运行库。它只保留
**一个公共 API**（context / request / job / frame / diagnostic）、
**一个执行内核**、**一个 `rkvc` CLI** 和 **一条发布路径**，媒体实现全部
通过版本化的后端 DSO ABI 接入。核心库不链接 FFmpeg、MPP、RGA 或 RKNN 类型。

## 能力

- **图执行内核**：有界队列背压、EOS、取消、逆序回滚和确定性后端回退；
  文件与流式端点
- **MPP 后端 DSO**：H.264/HEVC/AV1 解码（含 Annex-B 首包探测），
  H.264/HEVC 硬编码（DMA-BUF 零拷贝导入、CBR/FIXQP）
- **RGA 缩放后端 DSO**，并作为超分的无 NPU 回退路径
- **RKNN 超分后端 DSO**：Phase-RLFN 3×（NV12 逐像素相位打包、bicubic
  基座 + 残差融合）
- **MLVC 神经视频编解码后端 DSO**：NPU 双输入编码 → rANS 熵编码，
  四输入解码；`.mlvc` 容器与 `.rkmodel` 多载荷交付
- **SVT-AV1 软编** 与 **FFmpeg 容器 demux/mux** 后端（按需启用）

MLVC 编码/解码链路：

![MLVC 架构](docs/images/mlvc-architecture.png)
- **逐帧 ROI**、运行时码率/GOP 更新与强制 IDR（side-data 契约）
- **DMA-BUF / HOST 帧所有权**与逐行 stride 填充写出
- **`.rkmodel` 容器与模型注册表**（可信目录扫描、候选失败只淘汰）
- **可复现打包**：固定 sysroot、交叉构建、SBOM/provenance、ELF/glibc
  2.31 基线验证、确定性归档与 QEMU 冒烟

## 构建

要求 Linux、CMake 3.21+、C17 编译器、Ninja、pthread 与 dl。

~~~bash
cmake --preset default
cmake --build --preset default

./.build/release/rkvc version
./.build/release/rkvc inspect device --json
./.build/release/rkvc inspect backends --json
./.build/release/rkvc inspect models --json
./.build/release/rkvc license --json
~~~

默认产物位于 `.build/release/`：`librkvc.so`、`librkvc.a` 与 `rkvc`。
常用预设：`default`、`debug`、`tests`、`asan`（ASan+UBSan）、`coverage`、
`portable`。

### 后端

各后端默认关闭，需要对应的目标 SDK 前缀，配置时显式启用：

| 选项                        | 后端                                  | 需要前缀                  |
| --------------------------- | ------------------------------------- | ------------------------- |
| `RKVC_BUILD_BACKEND_MPP`    | `mpp.decode` / `mpp.encode`           | `MPP_INSTALL_PREFIX`      |
| `RKVC_BUILD_BACKEND_RGA`    | RGA 缩放                              | `RGA_INSTALL_PREFIX`      |
| `RKVC_BUILD_BACKEND_RKNN`   | `rknn.upscale`（Phase-RLFN 3×）       | `RKNN_INSTALL_PREFIX`     |
| `RKVC_BUILD_BACKEND_MLVC`   | `mlvc.encode` / `mlvc.decode`         | `RKNN_INSTALL_PREFIX`     |
| `RKVC_BUILD_BACKEND_SVT`    | `svt.encode`（AV1 软编）              | `SVT_AV1_INSTALL_PREFIX`  |
| `RKVC_BUILD_BACKEND_FFMPEG` | `ffmpeg.demux` / `ffmpeg.mux`（容器） | 树内 ffmpeg-rockchip 构建 |

RKNN 前缀必须含 `rknn_api.h`（或 `include/rknn/rknn_api.h`）及
`lib/librknnrt.so`。FFmpeg 后端链接 `third_party/ffmpeg-rockchip` 源码树内
`--enable-shared` 产出的 `libavcodec`/`libavformat`/`libavutil`，启用后容器
输入输出自动走 demux/mux，裸码流仍回退 `file.source` / `file.sink`。

## CLI

`rkvc` 将参数转换为 `rkvc_request` 后即调用公共 API，输入输出为后端可直接
消费的原始帧或 elementary stream：

~~~bash
rkvc decode -i input.h264 -o output.nv12 --codec h264
rkvc encode -i input.nv12 -o output.h264 --width 1920 --height 1080 --codec h264
rkvc transcode -i input.h265 -o output.h264 --codec h264
rkvc upscale -i input.nv12 -o output.nv12 --width 640 --height 360
rkvc bench decode -i input.h264 -o output.nv12 --codec h264 \
  --warmup 1 --iterations 5 --frames 300 --json
~~~

- `decode` 支持 `--codec h264|hevc|av1|mlvc`；`.mp4/.mkv/.ts` 容器输入自动
  demux（需 FFmpeg 后端）。
- `encode` 支持 `--codec h264|hevc|av1|mlvc`，`av1` 走 SVT 软编、`mlvc` 需
  NPU 模型（`--qp` 为量化档，默认 21）；`--bitrate` 设码率。
- `upscale` 的 `--width/--height` 为输入尺寸，NPU 超分优先，无模型时回退
  RGA 2×；`--model ID` 覆盖注册表选择。
- `bench OP` 复用媒体子命令参数并追加 `--warmup/--iterations/--frames/
  --duration`。

## 示例

`RKVC_BUILD_EXAMPLES=ON`（默认）按 0.4 API 构建 10 个示例：
`decode_file`、`encode_file`、`transcode`、`stream_ports`、`live_capture`、
`live_transcode_ports`、`net_loopback`、`roi_encode`、`adaptive_bitrate`、
`upscale_ctx`。其中 ROI 与热控示例展示逐帧 side-data 契约：

~~~bash
./.build/release/example_roi_encode roi.h264
./.build/release/example_adaptive_bitrate adaptive.h264
~~~

ffmpeg-rockchip 的对应下游 ROI/runtime-RC 补丁保存在
`patches/ffmpeg-rockchip/`，测试配置会对当前子模块 pin 执行 `git apply
--check`。

## API

~~~c
#include <rkvc/rkvc.h>

rkvc_context *context = NULL;
rkvc_job *job = NULL;
rkvc_request request;

rkvc_context_create(NULL, &context);
rkvc_request_init(&request, sizeof(request));
request.operation = RKVC_OPERATION_TRANSCODE;
request.input.kind = RKVC_ENDPOINT_FILE;
request.input.uri = "input.h265";
request.output.kind = RKVC_ENDPOINT_FILE;
request.output.uri = "output.h264";
request.codec = RKVC_CODEC_H264;

rkvc_job_create(context, &request, NULL, &job);
rkvc_job_start(job, NULL);
rkvc_job_wait(job);
rkvc_job_destroy(job);
rkvc_context_destroy(context);
~~~

公共接口见 [include/rkvc/api.h](include/rkvc/api.h)，后端扩展接口见
[include/rkvc/backend.h](include/rkvc/backend.h)。

## 测试

- **C**（`tests/c/`，CMocka + CTest）：图执行器、job 生命周期、媒体管线、
  frame 元数据、API/ABI 契约、后端 DSO 加载、`.rkmodel`、RKNN 与 MLVC 的
  fake Runtime 往返
- **Python**（`tests/python/`，`unittest`）：模型导出、发布校验与基准
- **Bash**（`tests/bash/`）：MLVC / 超分导出链路

~~~bash
cmake --preset tests
cmake --build --preset tests            # 含 check 目标
ctest --test-dir .build/tests -L c --output-on-failure
python3 -m unittest discover -s tests/python -p 'test_*.py' -v
~~~

真实 MPP/RGA/NPU 硬件路径仍须在 Rockchip 板卡验证；QEMU 只覆盖加载、CLI
与无硬件路径。详见 [docs/testing.md](docs/testing.md)。

## 性能基准

内建 `rkvc bench OP` 提供单项预热和重复采样；`tools/bench/benchmark.py`
在 Rockchip 实机上对 decode / encode / transcode 执行预热和多轮采样，
记录 FPS、实时倍速、吞吐、mean/median/p95/stdev、板卡温度与 CPU governor，
输出 JSON 与 CSV，并支持板卡回归门槛：

~~~bash
python3 tools/bench/benchmark.py --config tools/bench/config.local.json
~~~

配置矩阵与单项运行方法见 [tools/bench/README.md](tools/bench/README.md)。

新版 [RD 测试流程](tools/bench/README.md#rd-实测与报告)采用 UVG 经典七序列，
比较 MLVC、H.264、H.265、AV1 的低分辨率 RD，以及同一码流经传统插值与
3× SR 重建后的 1080p RD；传统编码器原生 1080p 作为额外参照。
报告按序列分图，提供实际码率、Y-PSNR／Y-SSIM、板端耗时和模型哈希。
旧版示例图不再作为当前版本性能依据。

## 模型

`.rkmodel` v1 是无摘要、无签名的结构容器（64B 固定头 + 有界 TLV + 载荷表），
由注册表从可信目录扫描加载，载荷经 SHA-256 校验后交付给后端。MLVC 以
`.rkmodel` 多载荷交付（`rknn` + `pmf-gaussian` + `pmf-bitest`，可选
`qppatch`），SR 交付单输入 Phase-RLFN 3× core。模型布局与导出见
[docs/mlvc-rknn-export.md](docs/mlvc-rknn-export.md) 与
[docs/sr-model-yuv-spec.md](docs/sr-model-yuv-spec.md)。

## 发布

~~~bash
python3 tools/rkvc-build package --jobs 6
~~~

发布编排器负责固定 sysroot、交叉构建依赖与目标、从安装树封装、生成
SBOM/provenance、验证 ELF 与 glibc 2.31 基线、确定性归档和 QEMU 冒烟；
重复归档字节级一致。详见 [docs/packaging.md](docs/packaging.md)。

## 文档

- [快速开始](docs/getting-started.md) · [架构](docs/architecture.md) ·
  [API](docs/api.md) · [测试](docs/testing.md) · [打包](docs/packaging.md)
- MLVC： [NPU 剖析](docs/mlvc-npu-profile.md) ·
  [RKNN 导出](docs/mlvc-rknn-export.md) ·
  [语义编解码 SDK 集成](docs/semantic-codec-sdk-integration.md)

## 许可

项目按 [LICENSE](LICENSE) 中的 AGPL-3.0-or-later 条款发布。
