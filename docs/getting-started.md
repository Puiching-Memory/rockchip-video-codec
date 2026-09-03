# 快速开始

要求 Linux、CMake 3.21+、C17 编译器、Ninja、pthread 和 dl。

~~~bash
cmake --preset default
cmake --build --preset default
~~~

默认产物位于 .build/release：librkvc.so、librkvc.a 和 rkvc。
默认同时编译 0.3 时期的 10 个示例名，并全部改用 0.4 API：文件编解码/
转码、流式端口、合成采集、UDP loopback、ROI、自适应码率、实时端口转码
和 upscale context；可用 `-DRKVC_BUILD_EXAMPLES=OFF` 关闭。

~~~bash
.build/release/rkvc version --json
.build/release/rkvc inspect device --json
.build/release/rkvc inspect models --json
~~~

MPP DSO 需要一个目标架构匹配的 MPP 安装前缀，并在配置时设置
RKVC_BUILD_BACKEND_MPP=ON 与 MPP_INSTALL_PREFIX。

SVT-AV1 软件编码后端（`svt.encode`，AV1）与 FFmpeg 容器后端
（`ffmpeg.demux` / `ffmpeg.mux`）按需启用：

~~~bash
# SVT：先构建安装到 .build/deps/svt-av1-install（含 include/svt-av1 与 lib/libSvtAv1Enc.so）
cmake -S . -B .build/svt -G Ninja \
  -DRKVC_BUILD_BACKEND_SVT=ON \
  -DRKVC_BUILD_BACKEND_FFMPEG=ON
~~~

FFmpeg 后端链接 `third_party/ffmpeg-rockchip` 源码树内的共享库，需先
configure && make（--enable-shared）产出 libavcodec/libavformat/libavutil
三个 `.so`。启用后，`decode`/`transcode` 的 `.mp4/.mkv/.ts` 等容器输入
自动走 `ffmpeg.demux`，`encode`/`transcode` 的容器输出自动走
`ffmpeg.mux`；裸码流路径不受影响，仍回退到 `file.source` / `file.sink`。

RKNN DSO 同样要求显式目标 SDK 前缀：

~~~bash
cmake -S . -B .build/rknn -G Ninja \
  -DRKVC_BUILD_BACKEND_RKNN=ON \
  -DRKNN_INSTALL_PREFIX=/opt/rknn-runtime-aarch64
~~~

前缀须含 `include/rknn_api.h`（或 `include/rknn/rknn_api.h`）及
`lib/librknnrt.so`。运行 `rkvc upscale` 时，注册表自动选择 role=upscale
的 `.rkmodel`；`--model ID` 可覆盖选择。

~~~bash
.build/release/rkvc bench decode -i sample.h264 -o /tmp/out.nv12 \
  --codec h264 --warmup 1 --iterations 5 --frames 300 --json
.build/release/rkvc license --json
~~~

MPP 后端加载成功后，可以直接运行逐帧 side-data 示例：

~~~bash
.build/release/example_roi_encode roi.h264
.build/release/example_adaptive_bitrate adaptive.h264
~~~

这两个示例均使用 FRAME_SINK 端点。前者为每个输入帧附带 ROI 矩形，后者
每 30 帧通过 `rkvc_encode_control` 更新码率/GOP 并请求 IDR。
