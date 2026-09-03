# semantic-codec-sdk 向上集成指南

> 面向把 rkvc（本仓库）内嵌进上层 SDK 的集成者——以 `semantic-codec-sdk`（ais SDK）为参考宿主。
> 覆盖：集成模型、构建配置、运行时约定、API 对接、已知陷阱、验证方法与故障排查。
> 内容基于 x86 容器与 RK3576 实板的端到端验证，随集成实践持续更新。

## 0. 快速事实

| 项                 | 值                                                                                           |
| ------------------ | -------------------------------------------------------------------------------------------- |
| 集成形态           | 宿主 CMake `add_subdirectory(rkvc)` + 链接 `rkvc_static`（纯 C 归档）；后端以 DSO 运行时发现 |
| rkvc 语言 / ABI    | C17（gnu17）；ABI 0.4（`RKVC_ABI_VERSION`，后端装载时强校验，major/minor/patch 全等）        |
| 参考宿主           | semantic-codec-sdk v0.1.x，C11 + C++17（视频适配层为 C++）                                   |
| CMake 版本         | 内嵌 rkvc 需 ≥ 3.21（rkvc 自身 `cmake_minimum_required`）                                    |
| 核心硬依赖         | Threads + dl                                                                                  |
| 可选后端 DSO       | MPP（硬编/解）、SVT-AV1（软编）、RGA、FFmpeg、RKNN、MLVC                                     |
| 上游集成的宿主接口 | 流式会话：open → send×N → flush → recv 至 EOF → close                                        |

## 1. 集成模型

```mermaid
flowchart LR
    App[宿主应用] -->|"ais_video_send / recv"| HostSo["libais_semantic_codec.so<br/>(C++ 适配层)"]
    subgraph HostSo
        RC[rkvc_static 核心符号<br/>(GLOBAL/DEFAULT 导出)]
    end
    RC -->|"dlopen + ABI 握手<br/>搜索 backend_dir"| BE1[rkvc_backend_mpp.so]
    RC --> BE2[rkvc_backend_svt.so]
    BE1 --> MPP["librockchip_mpp.so.1"]
    BE2 --> SVT["libSvtAv1Enc.so.4"]
```

要点：

1. **核心静态内嵌、后端动态发现**。rkvc 核心编译进宿主库；MPP/SVT 等后端构建为独立
   DSO，由 `rkvc_context_create` 触发 `rkvc_backend_dso_scan()` 装载（dlopen +
   `rkvc_backend_query()` ABI 握手 + probe 设备探测）。
2. **宿主库必须导出 rkvc 核心符号**。后端 DSO 引用 `rkvc_node_emit` 等核心符号，
   期望由宿主进程的动态符号表解析（见 §3.2）。
3. **规划器按需选后端**。宿主只描述"要做什么"（operation/codec/policy），图规划器
   在已装载后端的工厂表中挑选候选并按优先级/得分排序，open 失败自动回退次优候选。

## 2. 构建集成

### 2.1 宿主侧 CMake（参考实现）

```cmake
option(AIS_VIDEO_CODEC_RKVC "Video codec backed by rkvc" OFF)
set(AIS_RKVC_SOURCE_DIR "" CACHE PATH "rkvc source tree (>= CMake 3.21)")

if(AIS_VIDEO_CODEC_RKVC)
    # ... 校验 ${AIS_RKVC_SOURCE_DIR}/CMakeLists.txt 存在 ...

    # 只取核心静态库：关掉 rkvc 自身的 CLI/示例/测试与共享库
    set(RKVC_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(RKVC_BUILD_STATIC ON  CACHE BOOL "" FORCE)
    set(RKVC_BUILD_CLI OFF        CACHE BOOL "" FORCE)
    set(RKVC_BUILD_EXAMPLES OFF   CACHE BOOL "" FORCE)
    set(RKVC_BUILD_TESTS OFF      CACHE BOOL "" FORCE)

    # 目录属性隔离：宿主顶层若是严格 C11/-std=c 与 hidden 可见性，
    # 会污染 rkvc 子目录（gnu17 与 DSO 符号导出都依赖宽松设置）。
    # 进子目录前保存 C_STANDARD/C_EXTENSIONS/CXX_EXTENSIONS，设为宽松值，
    # add_subdirectory 后恢复。
    set(CMAKE_C_STANDARD 17)
    set(CMAKE_C_EXTENSIONS ON)
    set(CMAKE_CXX_EXTENSIONS ON)
    add_subdirectory(${AIS_RKVC_SOURCE_DIR} rkvc)
    # ... 恢复保存的属性 ...
endif()

# 视频适配层链入核心归档
target_link_libraries(ais_semantic_codec PRIVATE rkvc_static rkvc_instrumentation)
if(NOT AIS_BUILD_SHARED AND UNIX)
    target_link_options(ais_semantic_codec INTERFACE -Wl,--export-dynamic)
endif()
```

两个历史踩坑（均已在本仓/参考宿主修复，集成时勿回退）：

- rkvc 依赖脚本全部使用 `CMAKE_CURRENT_SOURCE_DIR`（而非 `CMAKE_SOURCE_DIR`）,
  `add_subdirectory` 内嵌时路径才不会错位；
- rkvc 顶层显式 `set(CMAKE_C_VISIBILITY_PRESET default)`，中和宿主全局 hidden
  设置，保证核心符号进入动态符号表。改可见性后必须**全量重建**。

### 2.2 rkvc 侧构建开关

| 开关                                            | 默认                            | 说明 / 所需前缀                                                                              |
| ----------------------------------------------- | ------------------------------- | -------------------------------------------------------------------------------------------- |
| `RKVC_BUILD_SHARED` / `RKVC_BUILD_STATIC`       | OFF / ON                        | 内嵌取 STATIC                                                                                |
| `RKVC_BUILD_CLI` / `_EXAMPLES` / `_TESTS`       | —                               | 内嵌全关                                                                                     |
| `RKVC_BUILD_BACKEND_MPP`                        | OFF                             | 需 `MPP_INSTALL_PREFIX`（`lib/librockchip_mpp.so` + 头；头缺失时回退 `third_party/mpp/inc`） |
| `RKVC_BUILD_BACKEND_SVT`                        | OFF                             | 需 `SVT_AV1_INSTALL_PREFIX`（`lib/libSvtAv1Enc.so` + `include/svt-av1/`）                    |
| `RKVC_BUILD_BACKEND_RGA`                        | OFF                             | 需 `RGA_INSTALL_PREFIX`（`lib/librga.so`；头缺失回退 `third_party/librga/include`）          |
| `RKVC_BUILD_BACKEND_FFMPEG` / `_RKNN` / `_MLVC` | OFF                             | 对应前缀见 `cmake/RkvcDependencies.cmake`                                                    |

### 2.3 依赖前缀准备

```bash
# MPP / SVT / RGA：用 rkvc-build 适配器装出前缀（含交叉场景），或使用
# 板卡/系统自带安装（例如 /usr/local 下的 rockchip_mpp 满足
# lib/librockchip_mpp.so + 头文件即可）。交叉时务必用目标架构的前缀，
# 混入宿主架构库会在运行期才暴露。
```

### 2.4 三种典型配置

**A. x86 开发验证（SVT 软编路径，无硬件依赖）**

```bash
cmake -B .build -G Ninja -DAIS_VIDEO_CODEC_RKVC=ON \
  -DAIS_RKVC_SOURCE_DIR=/path/to/rkvc \
  -DRKVC_BUILD_BACKEND_SVT=ON -DSVT_AV1_INSTALL_PREFIX=/path/to/svt-install \
  -DRKVC_BUILD_BACKEND_MPP=OFF -DRKVC_BUILD_BACKEND_RGA=OFF \
  -DRKVC_BUILD_BACKEND_FFMPEG=OFF -DRKVC_BUILD_BACKEND_RKNN=OFF \
  -DRKVC_BUILD_BACKEND_MLVC=OFF -DRKVC_BUILD_CLI=OFF \
  -DRKVC_BUILD_EXAMPLES=OFF -DRKVC_BUILD_TESTS=OFF
cmake --build .build
```

**B. aarch64 交叉（纯 C 目标适用）**

- 工具链文件：`cmake/toolchains/aarch64-linux-gnu.cmake` + sysroot；
- rkvc 本体纯 C，交叉无碍；**若宿主含 C++（如 ais SDK 适配层），宿主交叉
  工具链的 libstdc++ 必须与目标机 glibc 兼容**——高版本宿主交叉 GCC 的
  libstdc++ 常要求 GLIBC ≥ 2.36/2.38，低版本 sysroot 链接会引用
  `__isoc23_strtoul`/`arc4random` 等新符号而失败。此时优先方案 C。

**C. 目标板本地编译（含 C++ 宿主的推荐路径，RK3576 实测）**

板载 Ubuntu 22.04 + g++-11（libstdc++ 3.4.30 与系统运行库完全匹配）：

```bash
cmake -B .build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DAIS_VIDEO_CODEC_RKVC=ON -DAIS_RKVC_SOURCE_DIR=/data/sdk-test/rockchip-video-codec \
  -DRKVC_BUILD_BACKEND_MPP=ON -DMPP_INSTALL_PREFIX=<mpp前缀> \
  -DRKVC_BUILD_BACKEND_SVT=ON -DSVT_AV1_INSTALL_PREFIX=<svt前缀> \
  -DRKVC_BUILD_BACKEND_FFMPEG=OFF -DRKVC_BUILD_BACKEND_RKNN=OFF \
  -DRKVC_BUILD_BACKEND_MLVC=OFF -DRKVC_BUILD_CLI=OFF \
  -DRKVC_BUILD_EXAMPLES=OFF -DRKVC_BUILD_TESTS=OFF
cmake --build .build && ctest --test-dir .build
```

## 3. 运行时约定

### 3.1 后端 DSO 搜索顺序

`rkvc_context_create` 时按以下目录顺序收集 `*.so`（排序后逐个尝试，失败只记诊断）：

1. 包内目录：`<librkvc 映射所在目录>/rkvc/backends`（dladdr 定位，适配可重定位安装布局 `lib/rkvc/backends/`）；
2. `/usr/local/lib/rkvc/backends`；
3. `/usr/lib/rkvc/backends`；
4. 调用方经 `rkvc_context_options.paths.backend_dirs` 传入的可信目录
   （宿主应把用户参数透传到此，参考宿主的 `cfg.backend_dir`）

装载失败的最近一条诊断记录在 context 内部（`backend_diag`），排除问题时可 gdb
断点 `rkvc_registry_add_backend` 观察。

### 3.2 宿主符号导出要求

- 后端 DSO 以 `RTLD_NOW | RTLD_LOCAL` 装载，其未定义核心符号（`rkvc_node_emit`
  等）要求**装载时刻**能在宿主进程全局符号域解析；
- 常规做法：rkvc 链入**宿主共享库**（visibility default 保证符号进入 `.dynsym`
  GLOBAL/DEFAULT，实测导出 80+ 核心符号）；
- 静态 SDK 必须向最终可执行文件传播 `-Wl,--export-dynamic`；参考宿主已通过
  `target_link_options(... INTERFACE ...)` 实现。遗漏时 dlopen 会报
  `undefined symbol: rkvc_node_emit`。

### 3.3 DSO RUNPATH 与部署

后端 DSO 构建时把依赖前缀的 `lib/` 写入 RUNPATH（如 MPP 前缀
`.../target/mpp/lib`）。部署到目标机时三选一：

1. 前缀按配置期路径原样存在（构建机即目标机/板载编译场景天然满足）；
2. 目标机系统路径可解析依赖（如 `/usr/local/lib` 下的 `librockchip_mpp.so.1`）；
3. 运行时 `LD_LIBRARY_PATH` 指向依赖库目录。

### 3.4 backend_dir 传参

传给宿主/`rkvc_context_options` 的目录须为**绝对路径**；脚本内变量未展开
（如 `$PWD/rkvc` 字面量）会得到"planner 无候选"的误导性报错。

## 4. API 对接

### 4.1 流式会话 → rkvc 调用序列

| 宿主（参考实现） | rkvc 调用                                                                                                                                                                           | 说明                                                  |
| ---------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------- |
| `open(cfg)`      | `rkvc_context_options_init/create`（透传 backend_dirs/model_dirs）→ `rkvc_request_init`（operation/codec/policy/quality/端点/宽高）→ `rkvc_job_create`（带 diag）→ `rkvc_job_start` | open 失败时用 `rkvc_diag_fmt_text` 输出诊断链         |
| `send(frame)`    | 深拷贝载荷 → `rkvc_backend_frame_create`（挂释放回调，见 §5.5）→ `rkvc_job_push`                                                                                                    | push 为非阻塞 try 语义；AGAIN 处理见 §4.4             |
| `recv(&pkt)`     | flush 前 `rkvc_job_try_pull`（有限等待）；flush 后 `rkvc_job_pull`（阻塞排空）→ `rkvc_frame_get_desc` → 取 `data/size/flags`                                                        | AGAIN / EOF 判定见 §4.4                               |
| `flush()`        | `rkvc_job_push_eos`                                                                                                                                                                 | 之后 recv 排空至 EOF                                  |
| `close()`        | `rkvc_job_destroy` → `rkvc_context_destroy`                                                                                                                                         | 先销毁 job 再释放 context，防止帧引用越过核心生命周期 |
| `query_caps()`   | 按 backend_dir 创建临时 context → `rkvc_probe_device`                                                                                                                               | 见 §5.6                                               |

### 4.2 端点与格式

- 流式会话使用 `RKVC_ENDPOINT_FRAME_SINK` 双端点：输入格式写入
  `req.input.fmt`（如 NV12），输出 `req.output.fmt`（位流端写
  `RKVC_FRAME_FMT_BITSTREAM`），宽高写 `req.width/height`；
- FRAME_SINK 是**单节点图**（无 fileio source），`rkvc_graph_build` 在边协商后
  把端点声明的 fmt/宽高注入首/末节点端口（本仓 `lib/graph.c` 已含该注入段；
  早于该修复的检出会在 MPP 硬编 open 时报 FORMAT，见 §7）。FILE 端点路径由
  fileio source 声明格式经边协商传递，不受影响；
- MPP 编码接受 NV12 / YUV420P（HOST 域拷入或 DMABUF 导入）；解码输出为
  NV12 **DMABUF** 帧（HOST 回读见 §5.4）；
- 外部文件/网络码流先经 `ais_buffer_bitstream()` 深拷贝为带 PTS/DTS/flags 的
  SDK buffer，再交给 decoder send，禁止调用私有 `ais_buffer_create`。

### 4.3 状态码语义（宿主映射依据）

| rkvc 状态                              | 流式语义               | 参考宿主映射                                           |
| -------------------------------------- | ---------------------- | ------------------------------------------------------ |
| `OK`                                   | 成功                   | `AIS_OK`                                               |
| `AGAIN`                                | 队列满/空，稍后重试    | `AIS_ERR_AGAIN`（宿主 send 内部消化，recv 可对外暴露） |
| `EOF`                                  | 流结束（flush 排空后） | `AIS_ERR_EOF`                                          |
| `FORMAT` / `UNSUPPORTED` / `NOT_FOUND` | 格式约束 / 无可用候选  | `AIS_ERR_UNSUPPORTED`                                  |
| `IO` / `HW` / `CANCELED`               | 会话初始化或运行失败   | 按 mode 映射 `AIS_ERR_ENCODE` / `AIS_ERR_DECODE`       |
| `NOMEM` / `INVALID`                    | 资源 / 参数            | `AIS_ERR_NO_MEMORY` / `AIS_ERR_INVALID_ARG`            |

### 4.4 背压与拉取语义（宿主 recv 的正确姿势）

- `rkvc_job_push`：非阻塞。队列默认容量 4（`rkvc_graph_set_queue_capacity` 可调，
  须在 build 前），满返回 `AGAIN`；
- `rkvc_job_pull`：**阻塞**至有帧或 EOS；
- `rkvc_job_try_pull`：非阻塞，空且未 EOS 返回 `AGAIN`（与 try_push 对称，上游
  已发布，map/头文件一致由 `tools/check-exported-symbols.sh` 校验）

**死锁反模式**（gdb 实证）：调用方"send 数帧后进入 `while (recv==OK)` 排空"。
硬件编码器可能攒多帧才产包，阻塞 recv 永等输出，而后端在等新输入——三线程互等
（主线程 `rkvc_exec_pull` 内 `pthread_cond_wait`、worker `queue_pop`、MPP 线程
`mpp_thread_wait`）

**已实现模式**：flush 前，宿主 recv 使用 `try_pull` + 有界自旋（100×1ms），
无产出且未 EOF 时返回 `AIS_ERR_AGAIN`，让调用方继续 send 或稍后重试。send 遇
AGAIN 时先 `try_pull` 排空已产出帧到待取缓冲，再重试 push，对外保持同步发送。
flush 成功提交 EOS 后，worker 必然在 MPP 的 5 秒有限超时内到达尾包、EOS 或错误，
此时 recv 改用阻塞 pull，保证尾包完整排空且不对外产生伪 AGAIN。

`try_pull` 曾有一个独立的立即 EOF 缺陷：`rkvc_queue_try_pop()` 成功移除帧后未把
局部返回值从默认 `0` 改成 `1`，上层把该次成功弹帧误判为 EOS。修复后成功、EOS、
暂空分别严格返回 `1`、`0`、`RKVC_STATUS_AGAIN`，并由 queue/job 两层单测覆盖。

## 5. 已知约束与陷阱

### 5.1 FRAME_SINK 单节点图格式注入

见 §4.2。修复：`rkvc_graph_build` 边协商后注入 `req.input.fmt`（width/height
字段级合并，冲突报 NEGOTIATE）到首节点输入端口、`req.output.fmt` 到末节点输出
端口。状态：已修复（工作区，合入主线后更新此行）。错误特征见 §7。

### 5.2 MPP 拉取死锁与立即 EOF

两类问题均已修复并完成 RK3576 板测：

- MPP 正常 process 路径把输出超时设为 `MPP_TIMEOUT_NON_BLOCK`，避免编码器尚未
  产包时把 worker 永久卡在 `encode_get_packet`；输入沿用同步 task/frame 所有权
  语义，但设 5000ms 有限超时，硬件异常时可失败退出；
- flush 成功提交 EOS 后把输出超时切到 5000ms，持续排包直到 MPP EOS，既保留
  B 帧/GOP 尾包又避免永久等待；
- 宿主采用 §4.4 的 flush 前非阻塞、flush 后阻塞状态机；
- rkvc 修正 `rkvc_queue_try_pop()` 成功返回值，并确保队列中已有帧先于 cancel/error
  状态交付；process/flush 错误最终透传给 pull，不再表现成正常 EOF。

旧版 MPP 的空非阻塞输出可能以 `MPP_NOK` 表示而不是 `MPP_OK + NULL packet`；
process 路径将这两种形式都视为“当前无包”，flush 路径仍把它视为硬件错误。

### 5.3 配置初始化与质量参数

宿主配置必须由 `ais_video_config_init()` 初始化，结构中的 `struct_size` 用于拒绝
旧式 `memset` 零初始化。默认 `qp=-1`、`fps=30/1`、策略为 BALANCED；QP 只接受
`-1..51`，码率不得超过 `INT32_MAX`。rkvc 当前固定 30 fps，其他帧率由宿主在
open 前明确返回 UNSUPPORTED，不再静默忽略。

### 5.4 线性 DMABUF 解码输出

MPP 解码输出为 DMABUF 帧（含 FBC/瓦片布局需 DRM modifier 契约，后端刻意只暴露
线性帧）。Linux 宿主对 fd 做只读 `mmap`，按 `stride` / `ver_stride` 分平面复制，
产出紧凑 HOST NV12/YUV420P/RGB 缓冲；映射、边界或布局检查失败返回 DECODE，
绝不返回全零的伪成功帧。非线性 modifier 仍拒绝，后续应经 RGA 显式转换。

### 5.5 帧载荷生命周期

rkvc 帧只借用内存。宿主 send 深拷贝后经 `rkvc_backend_frame_create(&desc, free,
copy, &frame)` 挂释放回调：最后一个帧引用释放时副本自动 free。**禁止**会话级
`owned_` 指针数组驻留（长流内存线性增长，已在参考宿主移除）

### 5.6 caps 的硬件语义

`rkvc_probe_device` 的 `has_mpp_encoder/decoder` 仅统计 MPP 能力；SVT 等软件后端
不体现。宿主的 `ais_video_query_caps(backend_dir, ...)` 原样采用该语义；
`has_encoder` / `has_decoder` 明确表示 MPP 硬件能力，`has_rknn` 不再冒充解码能力。

### 5.7 MPP probe 条件

MPP 后端装载探测 = `access("/dev/mpp_service", R_OK|W_OK)`（旧节点名
`/dev/mpp-service` 兼容）+ `mpp_check_support_format` AVC 编/解任一支持。
RK3576 实测：DEC AVC/HEVC/AV1/VP9 均可，ENC 仅 AVC/HEVC（AV1/VP9 编码返回
不支持，属硬件能力，非缺陷）

## 6. 端到端验证

### 6.1 命令与预期输出

```bash
# backend_dir 必须绝对路径
./examples/ais_video_example /abs/path/to/.build/rkvc
```

```text
device: soc=rk3576-evb1-v10 mpp-enc=1 mpp-dec=1 npu=0
encoded: 5 frames -> 5 packets, 386 bytes
decoded: 5 NV12 frames (64x64)
```

### 6.2 实测基线（RK3576-evb1-v10，2026-09-03）

| 环境           | Ubuntu 22.04 / glibc 2.35 / RKNPU v0.9.8 / 板载 g++-11 11.4.0    |
| -------------- | ---------------------------------------------------------------- |
| 构建           | 板载增量编译 29 目标全过；宿主 SDK 单测 7/7                      |
| SVT AV1 软编   | 32×32×5 帧 → 174B；flush→EOF 语义正常                            |
| MPP H264 硬编  | 640×480×5 帧 → 5 包 1204B；500 帧 → 500 包 18237B（CBR）         |
| MPP H264 回环  | 64×64×5 帧 → 5 包 386B → 5 个 NV12 HOST 帧                       |
| 交错 send/recv | 50/500 帧均正常 EOF；flush 前 AGAIN、flush 后完整阻塞排空        |
| 长流内存       | 500 帧 VmRSS 增量约 0.9MiB（复跑 5200→6112KiB）；无 MPP 泄漏告警 |

### 6.3 板级回归矩阵

- [x] example：H.264 MPP 编码 + 真实逐包回环解码（backend_dir 绝对路径）
- [x] H264 硬编 500 帧 + RSS 前后对比（验证 §5.5 回调释放）
- [x] 交错 send/recv 压力（§4.4 模式，50/500 帧）
- [x] 解码逐帧尺寸/数量校验；线性 DMABUF 去 stride 回读为 HOST NV12
- [x] caps 输出（soc / mpp-enc / mpp-dec / npu）

## 7. 故障排查速查

| 错误特征                                                  | 根因                                                       | 处置                                                        |
| --------------------------------------------------------- | ---------------------------------------------------------- | ----------------------------------------------------------- |
| `planner(required stage has no candidate): not found`     | 后端 DSO 未装载（目录无 .so / 路径非绝对 / dlopen 失败）   | gdb 断 `rkvc_registry_add_backend` 看装载流；确认 §3.1/§3.4 |
| `undefined symbol: rkvc_node_emit`（dlopen）              | rkvc 核心符号未进宿主动态表（hidden 或静态宿主未导出）     | §2.1 可见性 + §3.2 `--export-dynamic`                       |
| `job_start failed: format (-8)`（MPP 硬编）               | 旧检出无 FRAME_SINK 格式注入，`mpp_enc_open` 读到 UNKNOWN  | §5.1 修复（graph.c 注入段）                                 |
| 三线程互等挂死（exec_pull / queue_pop / mpp_thread_wait） | 宿主 recv 阻塞 pull + "排空到没有为止"反模式               | §4.4 try_pull 有界自旋 / AGAIN 语义                         |
| worker 已 emit 包，但首次 `try_pull` 立即 EOF             | 旧版 `rkvc_queue_try_pop` 成功弹帧后仍返回 0，被误判成 EOS | 更新 rkvc executor；用 queue/job 非阻塞回归测试确认         |
| `caps enc=0` 但软件编码成功                               | caps 按契约只统计 MPP，软编后端不体现                      | §5.6（非故障）                                              |
| mpp 日志 `set rc fixqp` 而预期 CBR                        | 配置未通过 `ais_video_config_init` 初始化                  | §5.3                                                        |
| 编译报 `CLOCK_MONOTONIC` 未声明等                         | 宿主目录属性（严格 -std=c）污染 rkvc 子目录                | §2.1 属性隔离                                               |
| 链接失败引用 `__isoc23_strtoul` / `arc4random`            | 交叉 libstdc++ 与低版本 sysroot glibc 不兼容               | §2.4-B/C 换板载编译                                         |

## 8. 集成核对清单

1. [ ] 宿主 CMake：选项 + 子项目强制项 + 目录属性隔离（§2.1）
2. [ ] 依赖前缀：按需准备 MPP/SVT/RGA（§2.2/2.3）
3. [ ] 构建：全量通过；后端 DSO 产出；`readelf --dyn-syms` 确认宿主 so 导出
       `rkvc_node_emit` 等核心符号（§3.2）
4. [ ] 运行：backend_dir 绝对路径传参；DSO 依赖在目标机可解析（§3.3/3.4）
5. [ ] 适配层：send 深拷贝 + 释放回调（§5.5）；recv 分阶段拉取（§4.4）；
       线性 DMABUF 去 stride 回读（§5.4）；配置初始化（§5.3）
6. [ ] 回归：§6.3 矩阵全绿
7. [ ] 新增导出符号时同步 `include/rkvc/*.h` 与 `librkvc.map`
       （`tools/check-exported-symbols.sh` 校验）

## 9. 版本与变更记录

| 日期       | 变更                                                                                                               |
| ---------- | ------------------------------------------------------------------------------------------------------------------ |
| 2026-09-03 | 宿主公共 API 收口；补真实 H.264 编解码、线性 DMABUF 回读及静态安装包下游验证                                       |
| 2026-09-03 | RK3576 收尾：修复 try_pop 误报 EOF 与 MPP 有界排空；50/500 帧硬编、RSS 和无泄漏回归通过                            |
| 2026-09-03 | 重写为长期集成指南：并入 RK3576 实板结论（FRAME_SINK 注入修复、阻塞 pull 死锁模式、部署 RUNPATH 约定、故障速查表） |
| 2026-08    | 初版：x86 容器验证（SVT 软编 E2E）与最小修改提案（内嵌构建修复、try_pull 对称原语）——均已落地本仓与参考宿主        |
