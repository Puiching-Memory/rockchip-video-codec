# 测试

## 测试哲学

- 公共 API 契约优先：非法输入、边界值、错误码和资源释放先于 happy path（对照 [api.md](api.md)）。
- 异常路径必须可重复：OOM、I/O 错误、NULL 参数和无硬件环境均有确定性测试。
- 同一批测试在多种构建配置下重复运行：Debug、ASan/UBSan、coverage。
- 缺陷修复必须沉淀为回归测试。

## 当前测试矩阵

| 层级 | 目标 | 说明 |
|------|------|------|
| 类型与默认 | `tests/test_types.c` | 版本、pipeline 默认值、init 幂等 |
| Codec Router | `tests/test_router.c` | policy → H.264/HEVC/AV1 路由 |
| Buffer | `tests/test_buffer.c` | 视频/码流分配、引用计数 |
| 公共契约 | `tests/test_contracts.c` | caps、端口名、模板 |
| 内部一致性 | `tests/test_internal.c` | FFmpeg 错误映射、像素格式、端口队列 |
| 后处理上采样 | `tests/test_post_upscale.c` | 算法名、pipeline 默认值 |
| 权限门控 | `tests/test_permissions.c` | fake `/dev` 权限回归（fault injection preset） |
| 异常注入 | `tests/test_fault_injection.c` | 确定性 OOM 模拟 |
| 硬件集成 | `tests/test_hardware.c` | **默认跳过**；`RKVC_RUN_HARDWARE_TESTS=1` 时执行 |
| RGA 缩放 | `tests/test_scale.c` | 参数/布局始终运行；RGA 用例需硬件标志 |
| RGA 推广门禁 | `scripts/test-rga.sh` | 1080p↔360p、padding 源、post_upscale、soak；需 `/dev/rga` |
| CLI 脚本 | `tests/test_cli_args.sh` | CLI 参数错误（`full-tests` preset） |
| 可移植包 | `scripts/test-portable.sh` | 包完整性、RPATH、三策略 bench、后处理上采样、pkg-config（99 项） |
| 动态分析 | `asan` preset | ASan + UBSan |
| 覆盖率 | `coverage` preset | gcov instrumentation |
| 严格门禁 | `scripts/test-strict.sh` | 顺序执行 tests / asan / coverage |

`RKVC_ENABLE_FAULT_INJECTION` 默认关闭，仅在 `tests` preset 中启用。

### CTest 目标统计

| preset | CTest 目标数 | 说明 |
|--------|-------------|------|
| `tests` | 16 | 9 个单元测试 + 7 个硬件子用例 |
| `full-tests` | 18 | 上述 + `test_cli_args` + `test_bench_permission_failure` |

硬件测试拆为 7 个独立 CTest 用例（含三策略转码与 3× 上采样），未设置 `RKVC_RUN_HARDWARE_TESTS=1` 时 **exit 77（Skipped）**；设置后夹具自生成，无需 `tests/fixtures/` 内嵌文件。

## 执行命令

构建脚本与 CMake Presets 默认将编译并行度限制为 **4**。

```bash
# 基线单元测试
cmake --preset tests
cmake --build --preset tests
ctest --preset tests -j1 --output-on-failure

# 单元测试 + CLI 工具脚本
cmake --preset full-tests
cmake --build --preset full-tests
ctest --preset full-tests --output-on-failure

# RK3588 硬件集成
export RKVC_RUN_HARDWARE_TESTS=1
ctest --test-dir build-tests -j1 -R 'test_session_' --output-on-failure

# RGA 缩放推广门禁（1080p↔360p + soak）
./scripts/test-rga.sh
# 加长 soak：RKVC_RGA_SOAK_FRAMES=1000 ./scripts/test-rga.sh

# Sanitizer / 覆盖率 / 严格模式
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
cmake --preset coverage && cmake --build --preset coverage && ctest --preset coverage
./scripts/test-strict.sh
```

覆盖率门禁（需 gcovr）：

```bash
RKVC_COVERAGE_MIN_LINE=80 RKVC_COVERAGE_MIN_BRANCH=70 ./scripts/test-strict.sh
```

无 RKMPP 设备节点环境可使用 `60/50` 作为基础门禁。

## Valgrind

默认包含无硬件依赖的 `test_*`。第三方 MPP/FFmpeg 噪声通过 `scripts/mpp.supp` 屏蔽。CI 环境设置 `RKVC_VALGRIND_HARDWARE=0` 跳过硬件测试。

## 交付前最低要求

- `./scripts/test-strict.sh` 全部通过
- 可移植包通过 `./scripts/test-portable.sh <package-dir>`（99 项）
- RK3588 实机完成固定样本编码、解码、转码与长时间 soak test
- 新缺陷附带回归测试

## 发布清单

- 源码：`git status --short` 只含本次变更，子模块版本已锁定
- 构建：Debug、Release、ASan、coverage 均成功
- 单元：`test_types` ~ `test_post_upscale` 全部通过
- 硬件：记录 SoC、内核、设备权限、FFmpeg/MPP/SVT 版本、样本 SHA256
- 包：SDK/可移植包完整性、RPATH、动态库依赖验证
