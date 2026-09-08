# C++ 推倒重写计划（cpp-rewrite 分支）

> 状态：已评审定稿（2026-09-08）· 执行中
> 分支：`cpp-rewrite`（自 `main@61c1ac3` 切出），完成板级回归验收前不合回 main。

## 目标与总体策略

在 `cpp-rewrite` 分支上将本项目**推倒重写**为现代 C++20 项目：全新目录结构
（`src/core`、`src/backends`、`src/cli`、`include/rkvc/*.hpp`），后端插件协议
**C++ 化**（C vtable → 纯虚接口 + HostServices 注入），内部错误处理采用**异常**
（dlopen 插件边界两侧 try/catch 翻译为 status + diag）。旧 C 树在 golden 基线
固化后整体删除，`main` 分支永久保留作参照与回退路径。

不变量：算法层（rANS / 容器 / PMF / QPTAB / QPPatch / ratectl / pixel 内核）与
盘上线格式（`.rkmodel` v1、`.mlvc` 容器）保持**字节级兼容**——板上已部署模型与
流媒体素材不重新生成，行为由 Phase 0 固化的 golden fixtures 逐字节对拍锁死。

## 已确认决策（2026-09-08）

| 决策点 | 选择 | 影响 |
|---|---|---|
| 重写策略 | **推倒重写** | 旧 cmocka 测试不复用；安全网换成 golden fixtures 对拍 + 测试语义移植 |
| ABI | **允许演进** | backend.h C vtable → C++ 纯虚接口；新旧插件不互通；semantic-codec-sdk 的 C 兼容层降为可选 Phase 6 |
| 范围 | **含 CLI + examples** | rkvc.c 与 11 个示例全部重写，行为（子命令/JSON/退出码）golden 对拍兼容 |
| 风格 | **现代 C++，内部用异常** | `rkvc::Error{status, stage, cause 链}` 异常体系为核心错误模型；插件边界翻译；RTTI 开；插件 DSO 仅导出 query 入口 |

## 关键设计

- **两段式 API**：原生 C++ API（`namespace rkvc`，异常/RAII）为第一公民；
  C veneer（39 符号兼容层）为可选 Phase 6，仅当 SDK 需要时做。
- **新插件协议**：`extern "C" const rkvc::plugin::Descriptor* rkvc_plugin_query(uint32_t host_abi)`
  纯虚接口（IBackend / INodeFactory / INode / IPort）+ Descriptor 握手时注入
  `HostServices*`（核心工厂函数经接口指针交付，根治 DSO 反向符号解析问题——
  旧 rkvc_cli 需 ENABLE_EXPORTS 的坑随之消除）。
- **异常纪律**：插件虚方法可抛；host 每次调用点 try/catch 翻译为 Error + diag；
  host→插件析构/noexcept 约定写入插件 ABI 文档。
- **测试框架换 doctest**（单头 vendored，`tests/doctest.h`）；cmocka 测试语义
  全部移植为 C++ 套件。
- **部署**：交叉构建 `-static-libstdc++`（toolchain 已有先例）；161/214 板
  rootfs 按"无 libstdc++"假设处理，Phase 5 `ldd` 验证。

## 阶段与步骤

### Phase 0：golden 基线 + 工具链（删除旧代码的前置门禁）
1. 容器验证 `g++` / `aarch64-linux-gnu-g++`（13.3）与 `-std=c++20`；板上确认 libstdc++ 存在性（161/214）。
2. **从 main 构建 C 实现并固化 golden fixtures 到 `tests/golden/`**：
   fake-runtime MLVC 70 帧编码流 + 解码 NV12、rANS roundtrip 向量、
   pmf/qptab/qppatch 样本解析、ratectl 32 帧逐帧 trace（对齐官方 Python）、
   CLI inspect/version/license JSON、`.rkmodel` 样本解析结果、
   fixture_media_backend bench 输出。
3. 新 CMakeLists（`LANGUAGES CXX`、C++20、presets 重定义 debug/tests/asan/coverage/portable）
   + vendored doctest + check target。
4. **删除旧 C 树**（lib/ backends/ rkvc.c examples/*.c tests/c/，单 commit）。

### Phase 1：核心类型与错误模型（include/rkvc/*.hpp + src/core）
5. Status 枚举保留 AGAIN/EOF 流控语义；异常体系 `rkvc::Error`；diag text/JSON 格式化兼容。
6. Frame：`shared_ptr` 所有权 + 后端资源钩子（deleter）、host/DMABUF 双域、spec 协商类型。
7. rkmodel v1 读取器（字节兼容，golden 对拍）+ 单测。

### Phase 2：运行时核心 + 新插件 ABI
8. Context/Registry/DeviceProbe、Planner 候选回退、Graph、Executor
   （`std::jthread` / 有界队列 / EOS / cancel + stop_token）、Job。
9. 插件 ABI 定稿（Descriptor/HostServices/版本握手）+ dlopen 扫描装载器
   （坏 ABI 淘汰测试移植）。
10. fileio 首插件 + media_pipeline 等价测试（恒等编解码 fixture 插件移植）；
    移植 graph_executor/job/model_binding/api 契约测试语义。

### Phase 3：算法库与硬件后端（每模块 golden 逐字节对拍）
11. mlvc 算法：rans → container → pmf/qptab/qppatch → ratectl（官方 Python
    逐帧 MATCH）→ pixel（`_Float16` + NEON，全灰 NV12 逐位）。
12. rknn 后端 + fake runtime（C++ 移植）+ tensor 契约；rga/mpp（x86 编译级
    验证）；mlvc encoder/decoder 节点 E2E 对拍 goldens；svt/ffmpeg 视容器
    third_party 依赖可用性（否则留桩并注明）。

### Phase 4：CLI + examples
13. `src/cli/` C++ 重写：子命令/JSON/退出码与 golden 逐字节兼容。
14. examples 11 个全量重写（验证新 C++ API 易用性）。

### Phase 5：交叉构建 + 板级回归 + 打包
15. toolchain C++（`-static-libstdc++`）+ board-build.sh 适配；`ldd` 验证
    librkvc.so 无 libstdc++ 动态依赖。
16. 161 板 A/B/C/D 四基线 + bench dynq fps（±5%）；214 同。
17. rkvc-build（Python 编排）路径适配 + check-exported-symbols 重写/替代
    （新 ABI 审计）；docs / CHANGELOG 更新。

### Phase 6（可选）：C veneer 兼容层
18. 39 符号 C ABI 包装（异常 → status + diag 翻译），仅当 semantic-codec-sdk
    需要时启动。

## 验证标准

- Phase 0 后每阶段：新 check（doctest 全套）绿 + 相关 golden 逐字节 MATCH。
- Phase 2：插件 ABI 握手 / 坏 ABI 淘汰（移植 test_backend_loader 语义）。
- Phase 3：ratectl 官方 Python 逐帧 MATCH；mlvc 流/NV12 与 Phase 0 goldens 逐字节一致。
- Phase 4：CLI 输出与 goldens 逐字节一致。
- Phase 5：板级四基线全过 + bench 无回归 + `ldd` 无 libstdc++。

## 风险与缓解

1. **Phase 0 是唯一安全网窗口**——golden 固化前删除旧树 = 算法行为失去裁判
   基准，删除动作硬性排在基线固化之后。
2. **新旧插件不互通**：板上已部署旧 .so 布局与本分支产物不能混用；main 是
   回退路径（板上已部署二进制与 main 对应）。
3. **异常 × dlopen**：host 每个插件调用点 try/catch；插件析构/noexcept 纪律
   在 Phase 2 ABI 定稿时文档化。
4. **插件 C++ ABI 稳定性**：全树同工具链构建 + 版本握手兜底；文档注明
   "插件与核心同版本发布"。
5. fake_rknn 随后端一并移植为 C++（旧头无 extern "C" 守卫，反正要动）；
   bench/rd.py 等 Python 工具只改调用路径不改逻辑。

## 迁移映射（旧 → 新）

| 旧（C） | 新（C++20） |
|---|---|
| `lib/{api,frame,graph,executor,context,job,model_registry,backend_dso,builtin_backends,node_fileio,rkmodel}.c` | `src/core/`（含 rkmodel 读取器） |
| `include/rkvc/*.h`（39 导出符号 C ABI） | `include/rkvc/*.hpp` 原生 API；（可选）`include/rkvc/*.h` C veneer |
| `backend.h` C vtable + `rkvc_backend_query` | `rkvc/plugin.hpp` 纯虚接口 + `rkvc_plugin_query` + HostServices |
| `backends/backend_{mpp,rga,rknn,mlvc,svt,ffmpeg}.c` | `src/backends/`（mlvc 按 model/rung/encoder/decoder/registry 拆分） |
| `backends/mlvc/{rans,container,pmf,qptab,qppatch,ratectl,mlvc_pixel}.c` | `src/mlvc/`（算法库，字节兼容） |
| `rkvc.c` CLI | `src/cli/` |
| `examples/*.c` | `examples/*.cpp` |
| `tests/c/*.c`（cmocka） | `tests/`（doctest，语义移植 + golden 对拍） |
| `tools/{rga_probe,rknn_attr,mlvc/rknn_split_bench}.c`（CMake 外游离） | 不迁移（保持 C、按需手动编译） |

旧树规模参照：lib 11 文件 4.2k 行、backends 8 文件 5.9k 行 + mlvc 库 7 文件、
CLI 827 行、examples 11 文件，共约 15.7k 行 / 60 文件。
