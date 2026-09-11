cmake_minimum_required(VERSION 3.21)
project(rkvc-ais-video-host LANGUAGES C CXX)

# 集成样板：把 semantic-codec-sdk 的视频适配层装配成 ais_semantic_codec。
# SDK 上游只有挂在父目标上的片段（core/、codec/video/），视频顶层归集成者，
# 本文件就是那层的最小写法，按可移植包里的预编译 SDK 配：
#   cmake -S examples/ais-video-host -B build \
#     -DAIS_SDK_ROOT=/path/to/semantic-codec-sdk \
#     -Drkvc_DIR=<pkg>/lib/cmake/rkvc
# 两个参数都可省略：SDK 按内嵌/同级目录探测，rkvc 走 CMAKE_PREFIX_PATH。
# 产物 libais_semantic_codec.so + ais_video_demo，板端跑：
#   AIS_VIDEO_FAMILY=h264 AIS_VIDEO_FRAMES=5 ./ais_video_demo
# 用 RKVC_CORE_DIR 内嵌 core 时宿主与插件要同一编译器；预编译 SDK 无此约束。

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(AIS_SDK_ROOT "" CACHE PATH "semantic-codec-sdk 根目录（含 include/ais/ais.h）")
set(RKVC_CORE_DIR "" CACHE PATH "rkvc core 源码目录；没有预编译 SDK 时内嵌编译")
option(AIS_VIDEO_CODEC_RKVC "视频编解码走 rkvc 后端" ON)

# 本示例自己的参数写错就报错；纯自动探测失败则跳过——打包会对 examples/*
# 逐个 configure，缺 SDK 时必须安静跳过，而不是中断打包。
set(_ais_explicit FALSE)
if(AIS_SDK_ROOT OR RKVC_CORE_DIR)
    set(_ais_explicit TRUE)
endif()

if(NOT AIS_SDK_ROOT AND DEFINED ENV{AIS_SDK_ROOT})
    set(AIS_SDK_ROOT "$ENV{AIS_SDK_ROOT}")
endif()
if(NOT AIS_SDK_ROOT)
    foreach(_cand "${CMAKE_CURRENT_SOURCE_DIR}/../../semantic-codec-sdk"
                  "${CMAKE_CURRENT_SOURCE_DIR}/../../../semantic-codec-sdk")
        if(EXISTS "${_cand}/include/ais/ais.h")
            set(AIS_SDK_ROOT "${_cand}")
            break()
        endif()
    endforeach()
endif()
if(AIS_SDK_ROOT)
    get_filename_component(AIS_SDK_ROOT "${AIS_SDK_ROOT}" ABSOLUTE)
endif()

if(NOT AIS_SDK_ROOT)
    if(_ais_explicit)
        message(FATAL_ERROR "AIS_SDK_ROOT 无效：需指向 semantic-codec-sdk 根目录")
    endif()
    message(STATUS "跳过 ais-video-host：未找到 semantic-codec-sdk（用 -DAIS_SDK_ROOT=... 指定）")
    return()
endif()

if(AIS_VIDEO_CODEC_RKVC)
    find_package(rkvc CONFIG QUIET)
    if(NOT rkvc_FOUND AND RKVC_CORE_DIR)
        # 内嵌用法：core 源码同样提供 rkvc::core，片段里的链接名不用改。
        add_subdirectory("${RKVC_CORE_DIR}" rkvc-core-build)
    elseif(NOT rkvc_FOUND)
        if(_ais_explicit)
            message(FATAL_ERROR
                "未找到 rkvc：用 -Drkvc_DIR=<pkg>/lib/cmake/rkvc 或 -DRKVC_CORE_DIR=<rkvc 源码>/core")
        endif()
        message(STATUS "跳过 ais-video-host：未找到 rkvc（用 -Drkvc_DIR=... 指定）")
        return()
    endif()
endif()

add_library(ais_semantic_codec SHARED)
target_include_directories(ais_semantic_codec
    PUBLIC "${AIS_SDK_ROOT}/include"
    PRIVATE "${AIS_SDK_ROOT}")

add_subdirectory("${AIS_SDK_ROOT}/core" ais-core)
add_subdirectory("${AIS_SDK_ROOT}/codec/video" ais-video)

# 自带的最小可执行：编码 N 帧 NV12 并打印包数/字节数，编出来直接拿板端跑。
add_executable(ais_video_demo "${CMAKE_CURRENT_SOURCE_DIR}/demo.c")
target_link_libraries(ais_video_demo PRIVATE ais_semantic_codec)
