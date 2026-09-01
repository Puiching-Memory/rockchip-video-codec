# 快速开始

要求 Linux、CMake 3.21+、C17 编译器、Ninja、pthread/dl 和 autotools。

~~~bash
git submodule update --init third_party/libsodium
bash tools/install-libsodium.sh
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

MPP 后端加载成功后，可以直接运行逐帧 side-data 示例：

~~~bash
.build/release/example_roi_encode roi.h264
.build/release/example_adaptive_bitrate adaptive.h264
~~~

这两个示例均使用 FRAME_SINK 端点。前者为每个输入帧附带 ROI 矩形，后者
每 30 帧通过 `rkvc_encode_control` 更新码率/GOP 并请求 IDR。
