# C++ 推倒重写计划（cpp-rewrite 分支）

> 状态：v4 · 2026-09-08 · 执行中
> 分支：`cpp-rewrite`（自 `main@61c1ac3` 切出），完成板级回归验收前不合回 main。

## 目标与总体策略

将本项目**推倒重写**为现代 C++20 多工程体系，**不保留任何旧兼容性**：旧的
39 符号 C ABI、C vtable 插件协议、`.rkmodel` v1 / `.mlvc` 线格式均自由
重新设计（配套工具同步更新）。编解码能力按家族解耦为独立工程——
H.264/H.265（MPP 传统硬编解码）、MLVC-S（RKNN 神经编解码）、AV1（SVT-AV1
软编）、SR 超分（RKNN 模型增强）——各自独立 CMake、独立静态/动态库、
头文件与依赖互不可见。错误处理为**无异常模型**（Status + `Result<T>`，
`-fno-exceptions` 硬约束）。上游 semantic-codec-sdk 通过重新设计的通用
C ABI 对接；该 C ABI 同时是开源项目的语言中立集成面。

## 已确认决策（v4）

| 决策点 | 选择 | 说明 |
| --- | --- | --- |
| 语言标准 | **C++20** | 容器 g++ 13.3.0 原生/交叉均已验证 |
| 工具链基线 | **对齐 librknnrt** | glibc 符号上限 `GLIBC_2.17`；`-static-libstdc++ -static-libgcc`；符号审计兜底 |
| 兼容性 | **零旧兼容** | 不设兼容层；线格式、插件协议、C ABI 全部重新设计；板上模型与流用工具重打包 |
| 工程解耦 | **四 codec 工程** | core + codecs/h264h265 + codecs/mlvc + codecs/av1 + codecs/sr，独立可构建 |
| 错误模型 | **无异常** | `Status` + `Result<T>`；`-fno-exceptions -fno-rtti`（自有源硬约束，与 MPP 同款）；dlopen 边界零翻译成本 |
| 上游对接 | **SDK + 开源通用性** | 新极简 handle 式 C ABI 为唯一稳定面；SDK 适配层按新 ABI 重写；原生 C++ API 供进程内用户 |
| 格式版本策略 | **无版本号演进** | 线格式（rkmodel/mlvc 容器）不设 v1/v2 标记；格式与代码同仓同版本发布，不做任何兼容窗口 |

## 无异常错误模型（v4 修订）

同生态知名库实测证据：MPP 全局 `-fno-exceptions -fno-rtti`
（[CMakeLists.txt:160](../third_party/mpp/CMakeLists.txt#L160)）；librga、
librknnrt、FFmpeg、SVT-AV1 全部返回码错误处理，源码零 throw/catch。跟随
生态：

- **`rkvc::Status`**：负值枚举（含 AGAIN/EOF 流控语义），跨全部 API 面。
- **`rkvc::Result<T> = Status | T`**：expected 风格 sum 类型（自实现，
  ~200 行；C++20 无 std::expected）。失败上下文经 `Diag` 链携带
  （stage/subject/reason，入链深拷贝）。
- **编译期**：自有源 `-fno-exceptions -fno-rtti`（第三方库不受约束，
  只约束 `rkvc::*` target 的源）；RAII 全覆盖资源生命周期；OOM 走
  `Status::NOMEM` 返回路径（new 无异常模式下失败即 abort，热路径外的
  分配统一走受检包装）。
- **插件边界**：虚方法全部返回 Status，异常零穿越，边界零翻译代码。
- **doctest** 官方支持 `DOCTEST_CONFIG_NO_EXCEPTIONS`，与 -fno-exceptions
  测试编译完全兼容（已核实官方配置文档）。

## 第三方库选型（GitHub 调研，2026-09-08）

原则：**最小依赖面**。每个候选必须过三关：无异常可用、aarch64 交叉无障碍、
glibc 2.17 / 静态 libstdc++ 体系兼容。结论：**核心体系仅引入 1 个非头文件
依赖（doctest，且仅测试用）**，其余全部自实现或已 vendored。

| 用途 | 选型 | 依据 |
| --- | --- | --- |
| 单元测试 | **doctest**（vendored 单头，6.9k★） | 官方 `DOCTEST_CONFIG_NO_EXCEPTIONS` 支持已核实；编译最快；单头 vendored 零构建集成 |
| `Result<T>` | **自实现**（~200 行） | tl::expected（1.9k★）依赖异常路径表达错误侧；自实现直接绑定 `Status + Diag`，无异常语义更贴切 |
| JSON 输出（CLI/diag） | **手写极简 writer**（~150 行） | nlohmann/json（50k★）生态最成熟但 throw 语义 + 重；CLI 仅需序列化不需解析 |
| 格式化 | **std::format**（libstdc++ 13 已完整支持） | fmt 库（25k★）不再需要——GCC 13 的 C++20 format 实现已完整 |
| 日志 | 不引入（spdlite 等） | 现有 diag 链 + stderr 即日志面 |
| 构建元信息 | CMake 内建（不再用 version script） | 新导出面 = C ABI 头声明集合，审计脚本对照 |
| rANS / 熵编码 | **自实现**（语义对齐官方 msrtc_rans） | 无成熟小依赖可引；已有 C 实现作逻辑参照 |
| RKNN/MPP/RGA | 已 vendored/third_party | 不变 |

明确排除：Boost（体积/依赖面）、abseil（异常默认开）、fmt（GCC13 不需要）、
nlohmann/json（throw 语义，仅序列化场景太重）。

## 目标工程布局

```
rockchip-video-codec/            # 单仓库，多独立工程
├── core/                         # 工程 0：rkvc-core
│   ├── CMakeLists.txt            #   project(rkvc-core)，librkvc-core.{a,so}
│   ├── include/rkvc/             #   *.hpp 原生 API + rkvc.h（C ABI）+ plugin ABI
│   └── src/                      #   status/result/diag/frame/spec/graph/executor/
│   │                             #   context/job/planner/rkmodel
├── codecs/
│   ├── h264h265/                 # 工程 1：rkvc-h264h265（MPP 传统硬编解码）
│   │   ├── CMakeLists.txt        #   依赖仅 rkvc-core + librockchip_mpp
│   │   └── src/
│   ├── mlvc/                     # 工程 2：rkvc-mlvc（RKNN 神经编解码）
│   │   ├── CSCDELists.txt        #   依赖仅 rkvc-core + librknnrt
│   │   └── src/                  #   rknn 封装 + 算法库 + 编解码节点
│   ├── av1/                      # 工程 3：rkvc-av1（SVT-AV1 软编码）
│   │   ├── CMakeLists.txt        #   依赖仅 rkvc-core + libSvtAv1Enc
│   │   └── src/
│   └── sr/                       # 工程 4：rkvc-sr（RKNN SR 超分）
│       ├── CMakeLists.txt        #   依赖仅 rkvc-core + librknnrt
│       └── src/                  #   Phase-RLFN 模型宿主（自旧 backend_rknn SR 路径迁移）
├── cli/                          # rkvc CLI（链接 core，运行时发现 codec DSO 或静态全链）
├── examples/                     # C++ 示例 + examples/integration-c/
├── tests/                        # doctest 单测（各工程自带）+ 集成测试
├── tools/                        # rkmodel 打包/部署/审计脚本（同步重写）
└── CMakeLists.txt                # 顶层聚合（可选；子工程不依赖它存在）
```

解耦规则：

- codec 工程只依赖 `rkvc-core` 公开头 + 库（find_package 优先，回退
  add_subdirectory），codec 之间零依赖、头文件互不可见（编译期隔离）。
- 依赖传递隔离（NEEDED 审计）：h264h265 → core + mpp；mlvc → core + rknnrt；
  av1 → core + SvtAv1Enc；sr → core + rknnrt。mlvc 与 sr 共享 rknn 基础
  封装→ 上提到 core 的 `rkvc::npu` 支持层（仅头文件可见性受控，库仍合一）。
- codec 工程可被 CLI 运行时发现（DSO）或静态直嵌。
- RGA 缩放/格式转换属通用 2D 加速，进 core（`rkvc::rga` 支持层）——所有
  codec 均可能用（mlvc 上采样、sr 前处理等），不属于任何单一 codec。

## 双集成面设计

1. **C ABI（第一稳定面，开源通用）**：`core/include/rkvc/rkvc.h`，handle
   式极简接口（context/session/frame/枚举/`rkvc_status_str`/probe），语义
   覆盖 SDK 现消费全部能力；结构体 size/version 首字段演化模式。C ABI
   实现 100% 无异常（内部本就无异常，无翻译层）。
2. **原生 C++ API（第二面，进程内）**：`core/include/rkvc/*.hpp`，
   `namespace rkvc`，`Result<T>`/RAII/`string_view`。无跨编译器承诺。

semantic-codec-sdk 对接：SDK `codec/video/runtime/video_runtime.cpp`
（~419 行）按新 C ABI 重写（仓库外）；`examples/integration-c/` 为对接
样板（add_subdirectory + C ABI 完整用法 + target 契约）。

## 阶段与步骤

### Phase 0：基线与审计设施
1. ✅ 工具链验证（g++ 13.3.0 C++20 原生+交叉）。
2. ✅ librknnrt 依赖基线实测（GLIBC_2.17 / GLIBCXX_3.4.22）。
3. `tools/check-symbols.sh`（GLIBC 符号上限 + 禁动态 libstdc++/libgcc_s）。
4. 算法语义参照确认：ratectl ↔ 官方 rate_controller.py 逐帧对拍（容器
   mlvc venv）；rANS 自洽 + msrtc_rans 行为参照；pixel NEON/标量互拍；
   mlvc 语义以官方 microsoft/mlvc 为基准。
5. 旧 C 树行为参照留档（可选）：ratectl trace、fake-runtime mlvc E2E
   快照 → `tests/reference/`。

### Phase 1：core 工程
6. CMake 骨架（project(rkvc-core LANGUAGES CXX)、C++20、presets、vendored
   doctest、check target）。
7. **无异常错误模型落地**：Status/Result<T>/Diag + `-fno-exceptions
   -fno-rtti` 编译验证 + doctest NO_EXCEPTIONS 配置。
8. Frame（shared_ptr + 后端钩子、host/DMABUF）、spec 协商类型。
9. rkmodel 容器（格式随代码演进，无版本号；读写对称；rkmodel.py 同步）。
10. Context/Registry/Probe、Planner、Graph、Executor（jthread/stop_token/
    有界队列/EOS/cancel）、Job。

### Phase 2：插件 ABI 与装载器
11. 插件协议：`extern "C" const rkvc_plugin_descriptor* rkvc_plugin_query(uint32_t host_abi)`
    + 纯虚接口 + HostServices 注入；全虚方法返回 Status，无异常穿越；
    坏 ABI/坏插件淘汰测试。
12. fileio 内建；恒等编解码测试插件。

### Phase 3：codec 独立工程（四个）
13. `codecs/h264h265/`：MPP H.264/H.265 节点（自 backend_mpp.c 迁移）；
    x86 编译级验证 + 板上功能回归。
14. `codecs/mlvc/`：算法层（rans → container → pmf/qptab/qppatch →
    ratectl 官方对拍 → pixel `_Float16`+NEON 互拍）+ rknn 封装（fake
    runtime C++ 重写）+ encoder/decoder 节点 + fake E2E。
15. `codecs/av1/`：SVT-AV1 编码节点（自 backend_svt.c 迁移；EbSvtAv1Enc
    纯 C API 已验证 extern "C"）；容器内 third_party/SVT-AV1 可构建时全链
    验证，否则 x86 编译级 + 板上待验。
16. `codecs/sr/`：SR 超分节点（自旧 backend_rknn.c 的 Phase-RLFN 路径
    迁移；后处理热路径语义保持，性能对照 ±5%）。
17. 解耦验证：四工程独立 configure+build+test；include 隔离测试（h264h265
    不见 rknn/svt 头、mlvc 不见 mpp 头……）；NEEDED 集合审计。

### Phase 4：CLI + examples + 集成模板
18. `cli/`：C++ 重写；codec 家族运行时发现或静态链接。
19. `examples/` 语义等价重写；`examples/integration-c/` C ABI 样板。

### Phase 5：交叉构建 + 板级回归 + 打包
20. 交叉：`-static-libstdc++ -static-libgcc` + 符号审计全产物；161/214
    `ldd` 复核。
21. 模型重打包 + 板上重新部署（rkmodel.py 新格式；dynq/qprun 套件更新）。
22. 板级新基线：h264h265 往返/GOP/LTR/CBR；mlvc E2E；av1 编码（板上可用
    则）；sr 超分质量与性能；bench 对照 v2 main ±5% 超差归因。
23. rkvc-build 适配多库布局 + 符号审计；docs / CHANGELOG。

### Phase 6：semantic-codec-sdk 对接（仓库外）
24. SDK 适配层按新 C ABI 重写；以 integration-c 样板为契约。

## 验证标准

- 每阶段：doctest check 全绿 + 符号审计过（首个 ELF 起）。
- Phase 1：Result/Status/Diag 无异常编译套件；rkmodel 读写 roundtrip。
- Phase 2：插件握手/坏 ABI 淘汰/HostServices 注入。
- Phase 3：四 codec 工程独立构建+测试；include 隔离；NEEDED 审计。
- Phase 4：CLI 行为等价（差异白名单）；integration-c 编译运行。
- Phase  无异常编译断言：全树无 -fexceptions；测试二进制运行期零
  terminate 调用（ASan/UBSan 全绿）。

## 风险与缓解

1. **零旧兼容的部署代价**：工具先行（rkmodel.py 与读取器同步交付）、
   main 分支回退。
2. **GLIBC_2.17 红线突破**：符号审计进 check；突破即改等价旧 API。
3. **无异常 × OOM**：无异常模式下 operator new 失败即 terminate——热路径
   外分配统一走受检包装（`rkvc::checked_alloc`），热路径预分配。
4. **静态 libstdc++ 体积**：可接受；必要时 -ffunction-sections + gc-sections。
4. **插件 C++ ABI 跨 DSO**：同工具链同版本发布；版本握手兜底。
5. **SDK 对接节奏**：rkvc 先行交付 C ABI + 样板；SDK 侧独立排期。
6. **av1/sr 工程在无硬件/依赖场景的可构建性**：CMake 依赖缺失时工程
   优雅降级为不参与构建（message(STATUS) 不 FATAL），保持"独立可编译"
   承诺在任意环境成立。

## 迁移映射（旧 → 新）

| 旧（C 单体） | 新（C++20 多工程） |
| --- | --- |
| `lib/*.c`（11 文件） | `core/src/` → `librkvc-core.{a,so}` |
| `include/rkvc/*.h`（39 符号 C ABI） | `core/include/rkvc/rkvc.h` 新设计 + `*.hpp` |
| `backend.h` C vtable | core 新插件 ABI（纯虚 + HostServices，全 Status 返回） |
| `backends/backend_mpp.c` | `codecs/h264h265/` → `librkvc-h264h265.{a,so}` |
| `backends/{backend_rknn,backend_mlvc}.c` + `backends/mlvc/*` | `codecs/mlvc/`（编码解码）+ `codecs/sr/`（SR 路径） |
| `backends/backend_svt.c` | `codecs/av1/` → `librkvc-av1.{a,so}` |
| `backends/backend_ffmpeg.c` | 不迁移（demux 需求并入 CLI/宿主侧或 core 可选层，后续评估） |
| `rkvc.c` | `cli/` |
| `examples/*.c` | `examples/*.cpp` + `integration-c/` |
| `tests/c/*.c`（cmocka） | `tests/` doctest（NO_EXCEPTIONS 配置） |
| `.rkmodel`/`.mlvc` 格式 | 重设计，**无版本号**（格式与代码同版本发布） |
| `tools/` C 小工具 | 不迁移 |
| `tools/check-exported-symbols.sh` | `tools/check-symbols.sh`（GLIBC + 依赖审计替代） |

旧树规模参照：约 15.7k 行 / 60 文件。

## 附：semantic-codec-sdk 现对接面（重写参照，2026-09-08 实测）

- 消费方式：外层工程 `add_subdirectory(rkvc)` + `target_link_libraries(... rkvc_static rkvc_instrumentation)`，仅 include `rkvc/api.h`。
- 实际调用（C）：context `{options_init,create,destroy}` + `rkvc_probe_device`；
  job `{create,start,destroy,push,try_pull,pull,push_eos}`；frame
  `{wrap,release,get_desc,desc_init}`；diag `{fmt_text,release}` +
  `rkvc_status_str`；枚举 STATUS/CODEC/POLICY/OPERATION/FRAME_FMT/MEM_DOMAIN/
  ENDPOINT/TS_UNKNOWN。
- 行为契约：push 非阻塞（AGAIN 背压）、pull 阻塞 / try_pull 非阻塞、
  frame wrap 零拷贝借用（SDK 深拷贝载荷）、先 destroy job 再释放载荷。
- SDK 对外自身为纯 C ABI（`include/asis/*.h`），视频适配层 C++17
  （`codec/video/runtime/video_runtime.cpp` ~419 行为全部 rkvc 调用点）。
