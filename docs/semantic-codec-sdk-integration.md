# semantic-codec-sdk 集成验证与最小最优修改提案

> 目标：验证 `semantic-codec-sdk-master`（ais SDK）能否通过内嵌 `rockchip-video-codec`（rkvc）
> 完成**编译 + 运行**；基于实际构建/运行结果给出 SDK 与 rkvc 两侧的最小最优修改提案。

## 1. 验证结论（试过了才知道）

### 1.1 环境

| 项                | 值                                                                                   |
| ----------------- | ------------------------------------------------------------------------------------ |
| 构建机            | Ubuntu 24.04 x86_64（dev container `rkvc_dev`），gcc 13.3 / cmake 3.28 / ninja       |
| rkvc              | 0.4.0，纯 C17，本机源码 `HEAD 130cc04`                                               |
| SDK               | v0.1.0，C11 + C++17，`libais_semantic_codec.so`                                      |
| 编码后端          | SVT-AV1 **x86 软编**（容器内仅 aarch64 库，已交叉编译原生 x86 版 `libSvtAv1Enc.so`） |
| MPP / RKNN / MLVC | 未启用（x86 + 本容器无相关后端）→ 解码路径不可验证，属预期                           |

### 1.2 结果

- **编译**：✅ `cmake --build .build-sdk` 全量通过，`ais_video_example` 链接成功；
  `rkvc_backend_svt.so` 作为后端 DSO 正常产出。
- **运行**：✅ `ais_video_example` 端到端成功：

```
video.standard encoder opened          # SDK open → rkvc context+job+start
Svt[info]: SVT-AV1 Encoder Lib v4.2.0  # 后端 DSO 被规划器选中的证据
...
video encode ok: 5 frames -> 174 packet bytes   # 5×NV12 32×32 → 174B AV1 码流
video decode skipped (not supported here: unsupported)  # x86 无解码候选，符合预期
```

进度中遇到的“`planner(required stage has no candidate): not found`”在修复可见性后消除；
**前提是同时修复下述两个真实缺陷**，否则会出现“编译通过但运行时挂死/找不到后端”。

---

## 2. 实际踩到的两个根因

### 根因 1：rkvc 作为子目录内嵌时不可构建/不可装载

1. **`CMAKE_SOURCE_DIR` 硬编码**：`cmake/RkvcDependencies.cmake` 中 12 处使用 `CMAKE_SOURCE_DIR`，
   作为 `add_subdirectory` 内嵌时会指向 SDK 根目录 → 依赖路径全部错位。→ 改为 `CMAKE_CURRENT_SOURCE_DIR`。
2. **符号可见性被父工程污染**：SDK 全局 `CMAKE_C_VISIBILITY_PRESET hidden` 会传播进 rkvc 子目录，
   导致：
   - 静态库 `rkvc_static` 的核心符号（如 `rkvc_node_emit`）被隐藏 → 后端 DSO `dlopen`
     时 `undefined symbol: rkvc_node_emit`；
   - `rkvc_backend_query` 未导出 → 规划器认为没有任何后端可提供所需 stage
     （表现即 `planner(required stage has no candidate)`）。
   → 在 rkvc 顶层 `CMakeLists.txt` 显式 `set(CMAKE_C_VISIBILITY_PRESET default)`，
     并**全量重建**（仅重建 DSO 不够，隐藏编译产物仍留在 `rkvc_static.a` 里）。

### 根因 2：流式 API 不对称导致背压死锁（真实挂死，gdb 定位）

- `rkvc_job_push` 是**非阻塞** try 语义（队列满返回 `RKVC_STATUS_AGAIN`）。
- `rkvc_job_pull` 是**阻塞**语义（空时 `pthread_cond_wait`，只有 EOS 才返回 EOF）；
  **没有非阻塞 pull**。
- 后果：默认队列容量 4 时，第 5 帧 `send` 返回 AGAIN；调用方按常规做法
  “排空到没有为止”（`while (recv == OK) ;`）会阻塞——因为剩余帧与 EOS 都还没推入，
  输出不会再增长 → 永久挂死。gdb 证实：主线程卡在 `rkvc_job_pull` 内 `pthread_cond_wait`，
  SVT 全部工作线程空闲。
- 修复：为 rkvc 增加非阻塞 `rkvc_job_try_pull`（与 `try_push` 对称），
  SDK 在 `send` 遇 AGAIN 时用它非阻塞排空输出（转入待取缓冲），使 SDK 的
  `ais_video_send` 呈现常规**阻塞发送**语义。示例代码无需改动即通过。

---

## 3. 最小最优修改提案

### 3.A rkvc 侧（本体）

| 文件                           | 变更                                                                                      | 必要性                    | 影响面                                                          |
| ------------------------------ | ----------------------------------------------------------------------------------------- | ------------------------- | --------------------------------------------------------------- |
| `cmake/RkvcDependencies.cmake` | 12 处 `CMAKE_SOURCE_DIR` → `CMAKE_CURRENT_SOURCE_DIR`                                     | 必需（内嵌）              | 仅构建语义，独立构建行为不变                                    |
| `CMakeLists.txt`               | 在 `include(RkvcTargets)` 前 `set(CMAKE_C_VISIBILITY_PRESET default)`                     | 必需（内嵌 + DSO 可见性） | 独立构建默认本就是 default，行为不变                            |
| `lib/graph_internal.h`         | 声明 `rkvc_queue_try_pop()`、`rkvc_exec_try_pull()`                                       | 必需（对称原语）          | 内部头，无 ABI 影响                                             |
| `lib/executor.c`               | 实现 `rkvc_queue_try_pop()`（空且未 EOS→AGAIN）、`rkvc_exec_try_pull()`（AGAIN/EOF 映射） | 必需                      | 纯新增，不影响既有路径                                          |
| `lib/job.c`                    | 实现 `rkvc_job_try_pull()`（镜像 `rkvc_job_pull`，走 `try_pull`）                         | 必需                      | 纯新增                                                          |
| `include/rkvc/job.h`           | 公开声明 + 文档 `rkvc_job_try_pull`                                                       | 必需                      | 新增公共 API（ABI 增量，不破坏）                                |
| `librkvc.map`                  | 增加 `rkvc_job_try_pull;`                                                                 | 必需                      | 保持 map/头文件一致（`tools/check-exported-symbols.sh` 会校验） |

> 提案要点：不加任何“为 SDK 定制”的接口，只补齐流式 I/O 早已被“try_push / 阻塞 pull”
> 半掩盖的缺口——**对称的非阻塞 pull**。这也是上游通用的最小原语。

### 3.B SDK 侧（`semantic-codec-sdk-master`）

按**流式重设计**落地（非适配器堆叠），文件清单与要点：

| 文件                                                                          | 变更                                                                                                                                                                                                                         |
| ----------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `CMakeLists.txt`                                                              | 新增 `option(AIS_VIDEO_CODEC_RKVC)`、`AIS_RKVC_SOURCE_DIR`；内嵌 rkvc 时强制 `RKVC_BUILD_SHARED=OFF RKVC_BUILD_STATIC=ON` 及 CLI/EXAMPLES/TESTS 关闭                                                                         |
| `codec/video/CMakeLists.txt`                                                  | `AIS_VIDEO_CODEC_RKVC` 时 `target_link_libraries(ais_semantic_codec PRIVATE rkvc_static rkvc_instrumentation)`                                                                                                               |
| `include/ais/ais_status.h`、`core/ais_status.c`                               | 新增 `AIS_ERR_AGAIN=11`、`AIS_ERR_EOF=12` 及字符串                                                                                                                                                                           |
| `include/ais/ais_buffer.h`、`core/ais_buffer_internal.h`、`core/ais_buffer.c` | 位流包信息 `ais_packet_info_t`、`AIS_PACKET_FLAG_KEYFRAME`、`ais_buffer_packet_info()` / `ais_buffer_set_packet_info()`                                                                                                      |
| `include/ais/video_codec.h`                                                   | **核心**：流式接口 `ais_video_open/close/send/recv/flush/caps` + `ais_video_config_t`（id/mode/family/policy/宽高/格式/帧率/码率/qp/model/backend_dir/model_dir）+ `ais_video_caps_t`（soc/enc/dec/npu）                     |
| `codec/video/video_codec.cpp`                                                 | C 封装层：`struct ais_video_codec` → 委托 `ais::video::Codec`                                                                                                                                                                |
| `codec/video/runtime/video_runtime.hpp/.cpp`                                  | 单一 `Codec` 类：`open()`（context+request+job+start，含诊断文本）、`send()`（深拷贝→wrap→push，**AGAIN 时非阻塞排空重试**）、`recv()`（阻塞 pull + `pending_` 缓冲），`flush()`（push_eos），`query_caps()`（probe_device） |
| `examples/video_encode_decode.c`                                              | 流式用法示例：open → send×N → flush → recv 至 EOF → 可选回环 decode                                                                                                                                                          |

### 3.C 验收清单

```bash
# 容器内（x86 软编路径）
cd /root/workspace/semantic-codec-sdk-master
cmake --build .build-sdk -j8
LD_LIBRARY_PATH=.build-sdk:/root/svt-native/lib \
  ./.build-sdk/examples/ais_video_example .build-sdk/rkvc
# 预期：video encode ok: 5 frames -> N packet bytes；无挂死、无 not found
```

---

## 4. 未决项 / 后续建议

1. **设备能力探测**：`ais_video_caps_t` 目前仅统计 MPP/RKNN，SVT 等软件后端不体现
   （示例输出 `enc=0` 但编码可用）。建议后续让 `rkvc_probe_device` 或 SDK 侧叠加
   已装载后端能力。
2. **SDK 单测**：`tests/test_video_codec.c` 仍是旧 `ais_video_init/encode/decode` API，
   本次以 `AIS_BUILD_TESTS=OFF` 构建；建议随流式 API 一并改写（本次未启用，避免扩大改动面）。
3. **硬编/MLVC 路径**：x86 容器无法覆盖 MPP/RKNN；提案中的接口与 rkvc 侧改动均为通用，
   在 RK3588 等目标板需再跑一次端到端验证（含解码）。
4. **建议同步**：`rkvc_job_try_pull` 建议随 0.4.x 补丁发布，并补一条 executor 单测
   （`tests/c/test_graph_executor.c` 已有 queue 用例，可扩展 try_pop/try_pull）。
