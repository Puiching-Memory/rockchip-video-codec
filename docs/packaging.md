# 打包

唯一发布入口：

~~~bash
python3 tools/rkvc-build package --jobs 6
~~~

流水线为 pinned sysroot -> target dependencies -> CMake install ->
SBOM/licenses/provenance -> SHA256SUMS -> verify -> deterministic archive ->
QEMU smoke。

安装树而不是 Python 文件清单决定包内容。目标包只含统一 rkvc、唯一
librkvc.so.0、后端 DSO、公共头文件、模型目录和审计材料。

默认目标为 linux-aarch64-glibc231。验证器拒绝错误架构、绝对 RPATH、
缺失 SONAME/依赖、可写可执行段和高于 glibc 2.31 的符号需求。
