# 测试

| 测试 | 覆盖 |
| --- | --- |
| test_graph_executor | 协商、回滚、背压、EOS、取消和候选回退 |
| test_job | job 生命周期、线程和流式 push/pull |
| test_media_pipeline | file source/sink 与 fake codec 端到端 |
| test_backend_loader | DSO ABI 握手、坏候选隔离和可信目录 |
| test_rkmodel | 容器边界、摘要和模型注册表 |
| test_model_trust | 可选 Ed25519 签名互操作 |
| test_rkvc_build_verify.py | ELF、RPATH、SONAME 和 glibc 门禁 |

~~~bash
cmake --preset tests
cmake --build --preset tests
ctest --preset tests --output-on-failure
~~~

真实 MPP 编解码仍必须在 Rockchip 板卡运行。QEMU 只验证加载、CLI 和无硬件
路径，不能替代 VPU 功能与性能回归。
