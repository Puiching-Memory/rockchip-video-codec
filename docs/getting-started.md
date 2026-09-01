# 快速开始

要求 Linux、CMake 3.21+、C17 编译器、Ninja、pthread/dl 和 autotools。

~~~bash
git submodule update --init third_party/libsodium
./scripts/install-libsodium.sh
cmake --preset default
cmake --build --preset default
~~~

默认产物位于 .build/release：librkvc.so、librkvc.a 和 rkvc。

~~~bash
.build/release/rkvc version --json
.build/release/rkvc inspect device --json
.build/release/rkvc inspect models --json
~~~

MPP DSO 需要一个目标架构匹配的 MPP 安装前缀，并在配置时设置
RKVC_BUILD_BACKEND_MPP=ON 与 MPP_INSTALL_PREFIX。
