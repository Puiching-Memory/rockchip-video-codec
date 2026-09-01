# 打包

唯一发布入口：

~~~bash
python3 tools/rkvc-build package --jobs 6
~~~

仓库不隐式使用宿主机 RKNN Runtime。需要把 RKNN 后端与运行库纳入包时，
显式传入已经完成再分发审计的 AArch64 SDK 前缀：

~~~bash
python3 tools/rkvc-build package --jobs 6 \
  --rknn-sdk /opt/rknn-runtime-aarch64 \
  --rknn-license /secure/legal/RKNN-RUNTIME-LICENSE.txt
~~~

编排器把许可证证据纳入 SDK 内容摘要，将头文件/运行库隔离装入 target
prefix，再构建 `rkvc_backend_rknn.so`；两个参数必须同时提供，缺少头文件、
`librknnrt.so` 或许可证证据时立即失败。

流水线为 pinned sysroot -> target dependencies -> CMake install ->
SBOM/licenses/provenance -> SHA256SUMS -> verify -> deterministic archive ->
QEMU smoke。

安装树而不是 Python 文件清单决定包内容。目标包只含统一 rkvc、唯一
librkvc.so.0、后端 DSO、公共头文件、模型目录和审计材料。

默认目标为 linux-aarch64-glibc231。验证器拒绝错误架构、绝对 RPATH、
缺失 SONAME/依赖、可写可执行段和高于 glibc 2.31 的符号需求。
