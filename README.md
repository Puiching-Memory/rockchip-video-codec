# rockchip-video-codec

rkvc 是面向 Rockchip Linux 的 C17 媒体图运行库，提供
context / request / job / frame / diagnostic 公共 API、一个 rkvc CLI
和版本化后端 ABI。

当前已接通：

- 有界队列、背压、EOS、取消、逆序回滚和确定性后端回退
- 文件与流式端点
- MPP 后端 DSO：H.264/HEVC/AV1 解码，H.264/HEVC 编码
- RGA 缩放与 Phase-RLFN RKNN 3× 超分后端 DSO
- DMA-BUF/HOST 帧所有权
- 逐帧 ROI、运行时码率/GOP 更新与强制 IDR
- .rkmodel 容器与模型注册表
- AArch64/glibc 2.31 可复现打包、ELF 验证和 QEMU 冒烟

## 构建

~~~bash
cmake --preset default
cmake --build --preset default

./.build/release/rkvc version
./.build/release/rkvc inspect device --json
./.build/release/rkvc inspect backends --json
~~~

构建 MPP/RGA/RKNN 后端时分别准备对应目标 SDK 前缀，再启用
`RKVC_BUILD_BACKEND_MPP`、`RKVC_BUILD_BACKEND_RGA` 或
`RKVC_BUILD_BACKEND_RKNN`。RKNN 前缀必须含 `rknn_api.h` 与
`lib/librknnrt.so`。

统一 CLI 的媒体命令为：

~~~bash
rkvc decode -i input.h264 -o output.nv12 --codec h264
rkvc encode -i input.nv12 -o output.h264 --width 1920 --height 1080 --codec h264
rkvc transcode -i input.h265 -o output.h264 --codec h264
rkvc upscale -i input.nv12 -o output.nv12 --width 640 --height 360
rkvc bench decode -i input.h264 -o output.nv12 --codec h264 \
  --warmup 1 --iterations 5 --frames 300 --json
rkvc license --json
~~~

输入输出为后端可直接消费的原始帧或 elementary stream。

## 性能基准

内建 `rkvc bench OP` 提供单项预热和重复采样；`tools/bench/benchmark.py`
在 Rockchip 实机上对 decode / encode / transcode
执行预热和多轮采样，并记录 FPS、实时倍速、吞吐、p95、JSON 与 CSV：

~~~bash
python3 tools/bench/benchmark.py --config tools/bench/config.local.json
~~~

配置矩阵和单项运行方法见 [tools/bench/README.md](tools/bench/README.md)。

`RKVC_BUILD_EXAMPLES=ON`（默认）会按 0.4 API 构建全部 10 个示例；其中
`example_roi_encode` 与 `example_adaptive_bitrate` 展示逐帧 side-data 契约。
ffmpeg-rockchip 的对应下游 ROI/runtime-RC 补丁保存在
`patches/ffmpeg-rockchip/`，测试配置会对当前子模块 pin 执行 apply-check。

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

## 发布

~~~bash
python3 tools/rkvc-build package --jobs 6
~~~

发布编排器负责固定 sysroot、交叉构建依赖与目标、从安装树封装、生成
SBOM/provenance、验证 ELF 与 glibc 2.31 基线、确定性归档和 QEMU 冒烟。

项目按 [LICENSE](LICENSE) 中的 AGPL-3.0-or-later 条款发布。
