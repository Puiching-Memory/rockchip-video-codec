# 测试

三类测试，互不混放：

| 类型     | 位置                                                              | 框架与执行                                                              |
| -------- | ----------------------------------------------------------------- | ----------------------------------------------------------------------- |
| C++ 单元 | 各工程 `tests/`（doctest 2.4.11，`DOCTEST_CONFIG_NO_EXCEPTIONS`） | 随工程构建，顶层 CTest 聚合                                             |
| Python   | `tests/python/`                                                   | `python3 -m unittest discover -s tests/python -p 'test_*.py'`           |
| Bash     | `tests/bash/test_*.sh`                                            | 逐个 `bash` 执行（MLVC / SR 导出链路入口，优先 `.venv` 回退 `python3`） |

## C++ 测试

| 工程     | 测试                                                               | 覆盖                                                            |
| -------- | ------------------------------------------------------------------ | --------------------------------------------------------------- |
| core     | status_result、spec_frame、rkmodel、queue、plugin、pipeline、c_abi | Status/Result/Diag、帧规约、RKMDL1、队列、插件握手、管线、C ABI |
| mlvc     | tables、ratectl、rans、pixel、mlvc_codec                           | 算法表、码控、熵编码、像素打包、编解码往返                      |
| av1      | av1                                                                | SVT 软编逻辑                                                    |
| sr       | sr_post                                                            | 超分后处理（非方形 `core_w`/`core_h` 拆分）                     |
| h264h265 | mpp_logic、mpp_plugin                                              | MPP 参数逻辑、插件装载                                          |
| cli      | cli                                                                | 长选项解析（含 `--fps/--json`，无 `--low-delay`）               |

~~~bash
cmake --preset tests && cmake --build --preset tests
ctest --test-dir .build/tests --output-on-failure
~~~

`tests` 预设多开 `RKVC_CORE_BUILD_TESTS=ON`（core 测试默认关闭，其余工程
测试开关默认全开）。8/8（codec + cli）与 15/15（含 core）是合入前必须全绿
的两档。x86 只覆盖纯软路径；MPP / NPU 用例在板端跑同一条 `ctest`。

## Python 测试

| 测试                                    | 覆盖                                                                                              |
| --------------------------------------- | ------------------------------------------------------------------------------------------------- |
| test_rd.py                              | RD 核算（pooled MSE、GOP 审计、resume 校验；直读 `tools/bench/rd.uvg.json` 模板，模板失配即失败） |
| test_benchmark.py                       | `benchmark.py` 配置与命令生成（新 CLI 长选项、无 `--low-delay`）                                  |
| test_rkmdl1.py                          | RKMDL1 容器结构                                                                                   |
| test_mlvc_export.py / test_sr_export.py | MLVC / SR 导出、ONNX 重写、bundle 校验（缺 ONNX/NumPy 时 skip）                                   |

## 板端回归

MPP / NPU 硬件路径必须在 Rockchip 板卡验证：同一 `ctest` 全量 +
`tools/bench/` 的 RD 与性能采样（见 [bench 说明](../tools/bench/README.md)）。
`tools/bench/results/` 下的历史 `rd.json` 是旧协议归档，只作文档对照，
不作回归基线。

~~~bash
for test_script in tests/bash/test_*.sh; do
    bash "$test_script"
done
~~~

真实 MPP / NPU 硬件路径仍必须在 Rockchip 板卡运行（同一 `ctest` 全量 +
`tools/bench/` 采样）。x86 只验证加载、CLI 与无硬件纯软路径。
