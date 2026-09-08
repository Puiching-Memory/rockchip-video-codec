# C++ 推倒重写计划（cpp-rewrite 分支）

> 状态：v3 · 已按 5 项修订意见更新（2026-09-08）· 执行中
> 分支：`cpp-rewrite`（自 `main@61c1ac3` 切出），完成板级回归验收前不合回 main。

## 目标与总体策略

将本项目**推倒重写**为现代 C++20 多工程体系，**不保留任何旧兼容性**：
旧的 39 符号 C ABI、C vtable 插件协议、`.rkmodel` v1 / `.mlvc` 线格式均自由
重新设计（配套打包/部署工具同步更新）。H.265（MPP 传统硬编解码）与 MLVC-S
（RKNN 神经编解码）**解耦为两个独立可编译工程**，各自输出独立静态/动态库，
头文件与依赖互不可见。上游 semantic-codec-sdk 通过**重新设计的通用 C ABI**
对接，同时该 C ABI 也作为开源项目的语言中立集成面（C/C++/Rust/Python 等均可
绑定）。

## 已确认决策

| 决策点 | 选择 | 说明 |
| --- | --- | --- |
| 语言标准 | **C++20** | 容器 g++ 13.3.0 原生/交叉（aarch64-linux-gnu-g++）均已验证支持 |
| 工具链基线 | **对齐 librknnrt** | glibc 符号上限 `GLIBC_2.17`（librknnrt 实测最大需求）；`-static-libstdc++ -static-libgcc` 消除 GLIBCXX 动态依赖；CI 符号审计兜底 |
| 兼容性 | **零旧兼容** | 不设兼容层/shim；线格式、插件协议、C ABI 全部重新设计；板上模型与流由工具链重新打包部署 |
| 工程解耦 | **H.265 / MLVC-S 独立工程** | core + codecs/h265 + codecs/mlvc 三工程，各自完整 CMake，独立可构建，独立产物 |
| 上游对接 | **semantic-codec-sdk + 开源通用性** | 对外提供重新设计的极简 C ABI（handle 式）作为唯一稳定集成面；SDK 适配层按新 ABI 重写（仓库外，~650 行）；原生 C++ API 供进程内/同工具链用户 |
| （承前）策略 | 推倒重写 · 内部异常 | 错误模型 `rkvc::Error`；dlopen 插件边界 try/catch 翻译；RTTI 开；插件 DSO 仅导出 query 入口 |

## librknnrt 依赖基线（实测，2026-09-08）

容器 `.build/portable/target/rknn/lib/librknnrt.so`（aarch64）：

- NEEDED：`libpthread.so.0` `libdl.so.2` `libstdc++.so.6` `libm.so.6` `libgcc_s.so.1` `libc.so.6`
- glibc 符号需求最大值：**`GLIBC_2.17`**
- GLIBCXX 需求最大值：`GLIBCXX_3.4.22`（GCC 5.2 时代）

推论与对策：

1. **能跑 librknnrt 的板必有 glibc ≥ 2.17 与旧版 libstdc++**——但旧 libstdc++
   远达不到 GCC 13 C++20 运行时（需 GLIBCXX ≥ 3.4.30），故 rkvc 全部产物
   **必须 `-static-libstdc++ -static-libgcc`**，不依赖板载 libstdc++ 版本。
2. glibc 保持动态链接，但符号面必须审计：`objdump -T` 全部 `GLIBC_*` 引用
   上限 ≤ 2.17（`tools/check-symbols.sh`，进 check target）。C++20 运行时
   线程/原子特性（jthread/semaphore/atomic）在静态 libstdc++ 内实现，
   不引入 > 2.17 的 glibc 符号，审计兜底。
3. 交叉构建仍用容器 GCC 13.3（无 sysroot 直连）；发布包 sysroot 基线
   （focal 2.31）仅约束编译期头，运行期以 2.17 审计红线为准。

## 目标工程布局

```
rockchip-video-codec/            # 单仓库，多独立工程
├── core/                         # 工程 1：rkvc-core（公共基础，独立可构建）
│   ├── CMakeLists.txt            #   project(rkvc-core)，产物 librkvc-core.{a,so}
│   ├── include/rkvc/             #   *.hpp（原生 C++ API）+ rkvc.h（C ABI）
│   └── src/                      #   frame/graph/executor/context/job/planner/rkmodel
├── codecs/
│   ├── h265/                     # 工程 2：rkvc-h265（独立可构建）
│   │   ├── CMakeLists.txt        #   project(rkvc-h265)，产物 librkvc-h265.{a,so}
│   │   └── src/                  #   MPP H.264/H.265 硬编解码节点
│   └── mlvc/                     # 工程 3：rkvc-mlvc（独立可构建）
│       ├── CMakeLists.txt        #   project(rkvc-mlvc)，产物 librkvc-mlvc.{a,so}
│       └── src/                  #   rknn 运行时封装 + mlvc 算法库 + 编解码节点
├── cli/                          # rkvc CLI（链接 core，运行时发现 codec 插件）
├── examples/                     # C++ 示例 + C ABI 集成示例
├── tests/                        # doctest 全套（各工程自带单测）+ 集成测试
├── tools/                        # rkmodel 打包/板级部署/审计脚本（同步更新）
└── CMakeLists.txt                # 顶层聚合（可选构建全树；子工程不依赖它存在）
```

解耦规则：

- **codec 工程只依赖 rkvc-core 的公开头 + 库**（`find_package(rkvc-core)` 优先，
  回退 `add_subdirectory`），相互之间零依赖：h265 不见 rknn 头，mlvc 不见
  mpp 头，编译期即隔离。
- **依赖传递隔离**：librkvc-h265.so 只 NEEDED librkvc-core + librockchip_mpp；
  librkvc-mlvc.so 只 NEEDED librkvc-core + librknnrt。
- codec 工程既可作为 DSO 插件被 CLI/宿主运行时发现，也可被静态链接直嵌。
- ffmpeg demux/mux、SVT-AV1 属可选第三方向：默认不建，进入解耦体系前单独
  评估（避免为不存在的需求过度分拆）。

## 双集成面设计

1. **C ABI（第一稳定面，开源通用）**：`core/include/rkvc/rkvc.h`，handle 式
   极简接口——context/session(handle)/frame/枚举与常量/`rkvc_status_str`/
   probe，语义覆盖 semantic-codec-sdk 当前消费的全部能力（上下文+流式
   push/pull/EOF/背压 AGAIN、帧 wrap/描述、设备探测、诊断），函数命名与
   参数形态按新设计自由定稿，**不与旧 39 符号兼容**。结构体演化采用
   size/version 首字段模式。C ABI 内部即异常边界（catch → status+diag）。
2. **原生 C++ API（第二面，进程内）**：`core/include/rkvc/*.hpp`，
   `namespace rkvc`，异常/RAII/`std::string_view`。同工具链用户与本项目
   CLI/examples/测试直接使用。无跨编译器稳定性承诺。

semantic-codec-sdk 对接：其 `codec/video/runtime/video_runtime.cpp`（~419 行）
按新 C ABI 重写（仓库外任务，函数语义一一映射，工作量小）；rkvc 仓提供
`examples/integration-c/` 演示 add_subdirectory + C ABI 完整用法作为对接
模板。SDK 侧 CMake target 期望（静态库 + 头目录）在集成示例中给出契约。

## 阶段与步骤

### Phase 0：基线与审计设施
1. ✅ 工具链验证：g++/aarch64-linux-gnu-g++ 13.3.0，C++20 原生+交叉通过。
2. ✅ librknnrt 依赖基线实测（GLIBC_2.17 / GLIBCXX_3.4.22，见上节）。
3. `tools/check-symbols.sh`：审计任意 ELF 的 GLIBC 符号上限、禁止动态
   libstdc++/libgcc_s 依赖；接入 check。
4. **算法语义参照确认**（零旧兼容下的裁判基准）：ratectl 对官方
   `rate_controller.py` 逐帧对拍（容器 mlvc venv）；rANS roundtrip 自洽
   向量 + 官方 msrtc_rans 行为参照；pixel 内核 NEON/标量双路径互拍；
   mlvc 编解码以官方 microsoft/mlvc 参照实现为语义基准。
5. 旧 C 树行为参照留档（可选、仅排障辅助）：ratectl trace、fake-runtime
   mlvc E2E 输出快照 → `tests/reference/`。

### Phase 1：core 工程（core/）
6. CMake 骨架：`project(rkvc-core LANGUAGES CXX)`、C++20、debug/tests/asan/
   coverage/portable presets、vendored doctest、check target。
7. 错误模型：`rkvc::Status`（保留 AGAIN/EOF 流控语义值）、`rkvc::Error`
   异常（status/stage/cause 链）、diag text/JSON。
8. Frame（shared_ptr + 后端资源钩子、host/DMABUF）、spec 协商类型。
9. rkmodel 容器读取器——**格式自由重设计**（v2 起步，读写对称，
   `tools/rkvc_build/rkmodel.py` 同步）+ 单测。
10. Context/Registry/DeviceProbe、Planner 候选回退、Graph、Executor
    （`std::jthread` + stop_token、有界队列、EOS/cancel）、Job。

### Phase 2：插件 ABI 与装载器（core 内定稿）
11. 新插件协议：`extern "C" const rkvc_plugin_descriptor* rkvc_plugin_query(uint32_t host_abi)`
    + C++ 纯虚接口 + **HostServices 注入**（核心工厂经接口指针交付，根治
    DSO 反向符号解析）。异常纪律：插件方法可抛、host 调用点统一翻译；
    析构 noexcept。坏 ABI/坏插件淘汰测试。
12. fileio 内建（core 自带，非插件）；恒等编解码测试插件。

### Phase 3：两个 codec 独立工程
13. `codecs/h265/`：MPP H.264/H.265 节点，独立 CMake 可单独 configure/build；
    依赖仅 rkvc-core + librockchip_mpp；x86 编译级验证 + 板上功能回归。
14. `codecs/mlvc/`：
    - 算法层：rans → container → pmf/qptab/qppatch → ratectl（官方 Python
      逐帧 MATCH）→ pixel（`_Float16`+NEON，双路径互拍）；`.mlvc` 容器格式
      随新设计定稿，读写与工具同步。
    - rknn 运行时封装（fake runtime 以 C++ 重写，含旧头 extern "C" 修复）
      + encoder/decoder 节点 + fake E2E。
    - 依赖仅 rkvc-core + librknnrt；独立构建验证。
15. 解耦验证：`codecs/h265` 构建树无 rknn 头可见、`codecs/mlvc` 构建树无
    mpp 头可见（include 隔离测试）；两库 NEEDED 集合审计。

### Phase 4：CLI + examples + 集成模板
16. `cli/`：C++ 重写，运行时发现 codecs 产物（DSO）或全静态链接。
17. `examples/` 11 个语义等价重写（C++ API）；`examples/integration-c/`
    C ABI 集成模板（semantic-codec-sdk 对接样板）。

### Phase 5：交叉构建 + 板级回归 + 打包
18. 交叉：`-static-libstdc++ -static-libgcc` + `tools/check-symbols.sh`
    全产物审计（GLIBC ≤ 2.17、无动态 libstdc++）；161/214 `ldd` 复核。
19. 模型与素材重打包（新 rkmodel/.mlvc 格式）：`rkmodel.py` 产新包 →
    板上重新部署（dynq/qprun 套件随新格式更新）。
20. 板级新基线（基于新格式重建）：往返一致性、GOP/LTR/CBR 行为、bench
    dynq fps 对 v2 main 的性能对照（±5% 目标，超差归因）。
21. rkvc-build 发布管线适配多库布局 + 新符号审计；docs / CHANGELOG。

### Phase 6：semantic-codec-sdk 对接（仓库外）
22. SDK `codec/video/runtime/video_runtime.cpp` 按新 C ABI 重写（~650 行
    范围，含 CMake target 对接）；以 `examples/integration-c/` 为契约样板。

## 验证标准

- 每阶段：doctest check 全绿 + `tools/check-symbols.sh` 通过（从产出第一个
  ELF 起）。
- Phase 1：rkmodel v2 读写 roundtrip；ratectl 参照对拍（Phase 3 完成时）
  逐帧 MATCH。
- Phase 2：插件握手 / 坏 ABI 淘汰 / HostServices 注入路径。
- Phase 3：两 codec 工程**独立 configure+build+test 成功**；include 隔离
  测试；NEEDED 依赖集合符合解耦规则。
- Phase 4：CLI 行为等价（子命令/JSON/退出码与 v2 对照，差异白名单化）；
  integration-c 模板编译运行。
- Phase 5：符号审计全过 + 板级四类行为基线（新格式）+ 性能对照 ±5%。

## 风险与缓解

1. **零旧兼容的部署代价**：板上模型/流全部重打包。缓解：Phase 5 专属步骤、
   工具先行（rkmodel.py 与读取器同步交付）、main 分支即回退路径。
2. **GLIBC_2.17 红线意外突破**（C++ 运行时悄悄引入新符号）：审计脚本进
   check，任何 ELF 产出即检；突破时定位到具体符号改用等价旧 API。
3. **静态 libstdc++ 体积**（每 DSO 各带一份）：codec DSO 共 3 个库各自静态
   链接会增加体积；可接受（板端存储充裕），必要时评估 -ffunction-sections
   + gc-sections。
4. **异常 × dlopen**：host 每调用点 try/catch；插件析构 noexcept 纪律写入
   插件 ABI 头注释；坏插件（抛穿边界）在装载期探针调用中淘汰。
5. **插件 C++ ABI 跨 DSO 稳定性**：同一镜像内全树同工具链构建；版本握手
   兜底；文档声明"插件与核心同版本同工具链发布"。
6. **SDK 对接节奏**：rkvc 先行交付 C ABI + 集成模板；SDK 侧重写在其仓库
   独立排期，不阻塞本分支主体。

## 迁移映射（旧 → 新）

| 旧（C 单体） | 新（C++20 多工程） |
| --- | --- |
| `lib/*.c`（11 文件核心） | `core/src/` → `librkvc-core.{a,so}` |
| `include/rkvc/*.h`（39 符号 C ABI） | `core/include/rkvc/rkvc.h`（重新设计 C ABI）+ `*.hpp` 原生 API |
| `backend.h` C vtable 协议 | core 内新插件 ABI（纯虚 + HostServices） |
| `backends/backend_mpp.c` | `codecs/h265/` → `librkvc-h265.{a,so}` |
| `backends/{backend_rknn,backend_mlvc}.c` + `backends/mlvc/*` | `codecs/mlvc/` → `librkvc-mlvc.{a,so}` |
| `backends/{backend_ffmpeg,backend_svt}.c` | 可选第三方向，解耦前单独评估（默认不建） |
| `rkvc.c` | `cli/` |
| `examples/*.c` | `examples/*.cpp` + `examples/integration-c/` |
| `tests/c/*.c`（cmocka） | `tests/`（doctest，语义移植；参照=官方实现与新格式） |
| `.rkmodel` v1 / `.mlvc` 线格式 | 自由重设计（v2 起步），工具链同步 |
| `tools/{rga_probe,rknn_attr,mlvc/rknn_split_bench}.c` | 不迁移（保持 C、按需手动编译） |

旧树规模参照：约 15.7k 行 / 60 文件（lib 4.2k、backends 5.9k + mlvc 算法库、
CLI 827、examples 11 文件）。

## 附：semantic-codec-sdk 现对接面（重写参照，2026-09-08 实测）

- 消费方式：外层工程 `add_subdirectory(rkvc)` + `target_link_libraries(... rkvc_static rkvc_instrumentation)`，仅 include `rkvc/api.h`。
- 实际调用（C）：context `{options_init,create,destroy}` + `rkvc_probe_device`；
  job `{create,start,destroy,push,try_pull,pull,push_eos}`；frame
  `{wrap,release,get_desc,desc_init}`；diag `{fmt_text,release}` +
  `rkvc_status_str`；枚举 STATUS/CODEC/POLICY/OPERATION/FRAME_FMT/MEM_DOMAIN/
  ENDPOINT/TS_UNKNOWN。
- 行为契约：push 非阻塞（AGAIN 背压）、pull 阻塞 / try_pull 非阻塞、
  frame wrap 零拷贝借用（SDK 深拷贝载荷）、先 destroy job 再释放载荷。
- SDK 对外自身为纯 C ABI（`include/ais/*.h`），视频适配层 C++17
  （`codec/video/runtime/video_runtime.cpp` ~419 行为全部 rkvc 调用点）。
