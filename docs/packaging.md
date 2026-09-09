# 打包

本仓**没有打包器、不设 install 规则、不产安装树**。
旧 C 树的 `tools/rkvc-build`、`portable` 包、`RKVC_BUILD_BACKEND_*` 开关、
SBOM/provenance 流水线与 glibc 2.31 基线已随旧树删除，不再维护。

发布产物就是构建树：

~~~text
.build/release/rkvc            # CLI
.build/release/rkvc_*.so       # 所需 codec 插件
<模型目录>/*.rkmdl              # RKMDL1 模型（tools/mlvc、tools/sr 导出）
~~~

板端部署 = 复制这三样，保证插件与 CLI 同一次构建（工具链指纹握手会拒载
混版 `.so`），运行时用 `--backend-dir` / `--model-dir`（或逐个 `--model`）
指向它们。第三方运行时（MPP / `librknnrt.so` / SVT）由目标机系统路径或
随包复制的前缀目录提供，走常规动态链接解析。

aarch64 构建自动静态链接 libstdc++/libgcc；板端另跑
`tools/check-symbols.sh` 审计最终产物（GLIBC ≤ 2.34、NEEDED 禁动态
C++ 运行时）。注意该审计只在板端有意义：x86 构建产物天然动态链接
系统 libstdc++，CI 已移除这一步。
