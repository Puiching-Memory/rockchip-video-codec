# 测试

项目测试按实现语言分目录，避免测试源码、脚本入口和 Python 工具测试混放：

| 类型   | 目录             | 执行方式                    |
| ------ | ---------------- | --------------------------- |
| C      | `tests/c/`       | CMake 构建，CTest 执行      |
| Python | `tests/python/`  | `unittest` 自动发现         |
| Bash   | `tests/bash/`    | 逐个执行 `test_*.sh`        |

## C 测试

| 测试                | 覆盖                                          |
| ------------------- | --------------------------------------------- |
| test_graph_executor | 协商、回滚、背压、EOS、取消和候选回退         |
| test_job            | job 生命周期、线程和流式 push/pull            |
| test_media_pipeline | file source/sink 与 fake codec 端到端         |
| test_frame_metadata | ROI/编码热控校验、元数据深拷贝与所有权        |
| test_api_contract   | 初始化器、状态码、结构体前缀兼容与 ABI 主版本 |
| test_backend_loader | DSO ABI 握手、坏候选隔离和可信目录            |
| test_rkmodel        | 容器边界、摘要和模型注册表                    |
| test_model_trust    | 可选 Ed25519 签名互操作                       |
| test_backend_rknn   | fake Runtime 下模型绑定、推理与 NV12 3× 输出  |

~~~bash
cmake --preset tests
cmake --build .build/tests
ctest --test-dir .build/tests -L c --output-on-failure
~~~

C 测试均带有 `c` CTest 标签。完整的 `check` 目标还会运行补丁检查、CLI
冒烟测试，并编译全部公共 API 示例：

~~~bash
cmake --build --preset tests
~~~

`test_ffmpeg_patches` 只运行 `git apply --check`，不会弄脏子模块。

## Python 测试

| 测试                        | 覆盖                                      |
| --------------------------- | ----------------------------------------- |
| test_mlvc_export.py         | MLVC PMF/QP patch、导出 CLI 与 ONNX 重写  |
| test_sr_export.py           | 超分模型导出、校准数据和 bundle 校验      |
| test_rkvc_build_verify.py   | ELF、RPATH、SONAME、依赖与 glibc 门禁     |
| test_benchmark.py           | 性能基准配置、命令生成、统计与阈值        |

~~~bash
python3 -m unittest discover -s tests/python -p 'test_*.py' -v
~~~

ONNX、NumPy 等可选依赖缺失时，对应用例会明确标记为 skip。ELF 校验器测试只在
Linux 运行，其余平台同样标记为 skip。

## Bash 测试

Bash 目录提供使用项目 Python 环境执行 MLVC 和超分导出测试的入口。脚本优先
使用 `.venv/bin/python`，不存在时回退到 `python3`，并且不依赖调用者的当前
工作目录。

~~~bash
for test_script in tests/bash/test_*.sh; do
    bash "$test_script"
done
~~~

真实 MPP 编解码仍必须在 Rockchip 板卡运行。QEMU 只验证加载、CLI 和无硬件
路径，不能替代 VPU 功能与性能回归。

## 性能基准

CTest 使用 fake 媒体 DSO 冒烟验证 `rkvc bench` 的真实 Request/Job 执行路径；
性能门禁仍不混入普通单元测试。板卡上可用内建 `rkvc bench OP` 做单项采样，
或使用 `tools/bench/benchmark.py` 对固定媒体集做矩阵采样；完整配置格式、输出指标
及性能门槛见 [性能基准说明](../tools/bench/README.md)。
