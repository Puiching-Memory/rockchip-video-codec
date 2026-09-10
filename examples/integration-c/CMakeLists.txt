cmake_minimum_required(VERSION 3.21)
project(rkvc-integration-c LANGUAGES C CXX)

# Contract sample for embedding rkvc (mirrors the SDK adapter shape):
#   # 方式一：链接可移植包里的预编译 SDK（无需 core 源码）
#   cmake -S . -B build -Drkvc_DIR=/path/to/pkg/lib/cmake/rkvc
#   # 方式二：自带 core 源码（静态链，插件指纹需与自身编译器一致）
#   cmake -S . -B build -DRKVC_CORE_DIR=/path/to/core \
#     -DRKVC_CODEC_PLUGIN=/path/to/rkvc_av1.so
#   ./build/intc_encode <plugin.so> <width> <height> <frames>
set(RKVC_CORE_DIR "" CACHE PATH "rkvc core source dir（未提供预编译 SDK 时使用）")
find_package(rkvc CONFIG QUIET)
if(rkvc_FOUND)
    set(RKVC_LINK_TARGET rkvc::core)
elseif(RKVC_CORE_DIR)
    add_subdirectory(${RKVC_CORE_DIR} rkvc-core-build)
    set(RKVC_LINK_TARGET rkvc-core-static)
else()
    message(FATAL_ERROR
        "需要其中一种：-Drkvc_DIR=<pkg>/lib/cmake/rkvc 或 -DRKVC_CORE_DIR=<core 源码目录>")
endif()

add_executable(intc_encode main.c)
target_link_libraries(intc_encode PRIVATE ${RKVC_LINK_TARGET})
