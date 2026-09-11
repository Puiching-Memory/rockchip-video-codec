# rkvc @VERSION@ 可移植包（linux-aarch64）

一次交叉构建的 **CLI + codec 插件 + 预编译 SDK + 随包第三方运行库**，解压即用、
整包可搬迁。它同时服务两类消费者：直接跑转码（`bin/rkvc`），或把 rkvc 链进
自己的程序（`include/ + lib/librkvc.so + lib/cmake + lib/pkgconfig`）。
构建日期 @DATE@，构建入口见仓库 `tools/portable/build.sh`。

## 目录

```text
bin/rkvc                   CLI（C++ 运行时已静态链接）
lib/rkvc/backends/*.so     codec 插件（h264h265 / av1 / mlvc / sr，按构建开关）
lib/librkvc.so.0.5.0       SDK：预编译 core（与上面插件同源同编译器）
include/rkvc/              16 个公开头文件（C 的 rkvc.h + C++ 头）
lib/cmake/rkvc/            find_package(rkvc CONFIG) 用的包文件（可搬迁）
lib/pkgconfig/rkvc.pc      pkg-config 描述（前缀由 pcfiledir 反推，可搬迁）
lib/*.so*                  随包运行库（rockchip_mpp / rknnrt / SvtAv1Enc，按需）
docs/                      全套文档（入口 docs/index.md）
examples/                  三个 C ABI 样板（integration-c / decode-file / upscale-file）
models/<bundle>/           可选模型目录（构建时 --models 收录）
licenses/                  许可证文本与来源清单（PROVENANCE.txt）
CHANGELOG.md               版本历史（docs 里有链接引到这里）
test.sh                    包内自测（含 SDK 面校验与 C 消费者冒烟）
MANIFEST.sha256            全包校验和
```

`docs/` 与 `examples/` 是仓库里那两份原样收录（不含 doxide 生成的 `docs/api/`，
它按头文件现生成、不入库）。示例支持两种构建方式：`-Drkvc_DIR=lib/cmake/rkvc`
（用包内 SDK，无需源码树）或 `-DRKVC_CORE_DIR=<core 目录>`（自带源码，此时插件
指纹须与自己的编译器一致）。`docs/` 里提到的 `tools/` 之类路径指的是仓库，不
在包内。

## 快速开始

```bash
./test.sh                                   # 先跑自测
./bin/rkvc caps                             # 设备探测
./bin/rkvc inspect backends                 # 插件握手一览
head -c $((640*368*3/2*30)) /dev/urandom > in.nv12
./bin/rkvc encode --codec h264 --input in.nv12 --width 640 --height 368 \
    --pixfmt nv12 --output out.h264 --qp 26 --gop 30 --fps 30
```

插件与运行库按包内相对路径自动发现（`bin/../lib/rkvc/backends`），无需
`--backend-dir`；要指向别处仍可显式传 `--backend-dir DIR`。MLVC 与超分
模型用 `--model-dir models`（`--model DIR` 为同义别名），`--model-id` 按导出
stem 选择。

## 当 SDK 用（把 rkvc 链进自己的程序）

```bash
# CMake 工程（推荐）
cmake -S . -B build -Drkvc_DIR=$PWD/lib/cmake/rkvc
#   之后在 CMakeLists.txt 里：find_package(rkvc CONFIG REQUIRED)
#                            target_link_libraries(host PRIVATE rkvc::core)

# pkg-config 或裸编译（纯 C 也行，librkvc.so 已静态链入 libstdc++）
PKG_CONFIG_PATH=$PWD/lib/pkgconfig pkg-config --cflags --libs rkvc
gcc-11 -Iinclude host.c -Llib -Wl,-rpath,'$ORIGIN/../lib' -lrkvc -o host
```

**零配置发现**：链接 `librkvc.so` 的宿主不用传 `backend_dirs`——插件发现会以
`librkvc.so` 自己的位置反查 `lib/rkvc/backends`（即本包布局）。示例：

```bash
gcc-11 -std=c99 -Iinclude examples/integration-c/main.c -Llib \
    -Wl,-rpath,'$ORIGIN/../lib' -lrkvc -o /tmp/intc_encode
/tmp/intc_encode lib/rkvc/backends/rkvc_av1.so 640 368 3   # → intc: 3 packets …
```

> 该示例走 FrameSink 推帧，**在 qemu-user 下必崩**（静态 core 编同一示例同样
> 崩、真板卡正常），请在板卡上跑运行校验。

把本包拼进下游宿主的摆法、工具链指纹约束与验收步骤，见仓库
`docs/portable-package.md`；注意插件不能单独搬运——它靠 `$ORIGIN/../..`
解析本包 `lib/` 里的第三方库。

## 运行环境

- aarch64 Linux，glibc ≥ 2.34（构建自 Ubuntu 22.04 交叉工具链）。
- 硬件路径：H264/HEVC 需要 `/dev/mpp_service`；MLVC/超分需要 NPU 设备节点
  与匹配的 NPU 驱动（启用时 `librknnrt.so` 随包提供）。
- `librknnrt.so` 自身动态依赖目标机的 `libstdc++.so.6` / `libgcc_s.so.1`
  （系统自带即可）；rkvc 自身产物（含 `librkvc.so`）不依赖动态 C++ 运行时。
- 用 SDK 编宿主需要 C/C++ 编译器；`librkvc.so` 的 C ABI 主版本为 0，
  与头文件 `RKVC_ABI_VERSION` 对应，大版本不符会被加载器挡在 `librkvc.so.0`。

## 自测

`./test.sh` 覆盖布局、校验和、依赖解析、插件握手、SDK 面（头文件/`librkvc.so`/
CMake config/pkg-config + 纯 C 消费者零配置建会话）、av1 软编码与 MPP 硬编
解码冒烟；无硬件或无编译器的项自动跳过。在 x86 上可用
`RKVC_RUNNER="qemu-aarch64-static -L /usr/aarch64-linux-gnu" ./test.sh`
做模拟运行（硬件项会跳过）。
