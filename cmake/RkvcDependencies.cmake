# SPDX-License-Identifier: AGPL-3.0-or-later
# Dependencies used by the core library and its backend DSOs.
#
# All paths below are relative to the rkvc source tree (CMAKE_CURRENT_SOURCE_DIR),
# so rkvc can also be added as a subdirectory of another CMake project.

find_package(Threads REQUIRED)

if(RKVC_BUILD_BACKEND_MPP)
    set(MPP_INSTALL_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/.build/deps/mpp-install"
        CACHE PATH "MPP install prefix")
    if(NOT EXISTS "${MPP_INSTALL_PREFIX}/lib/librockchip_mpp.so")
        message(FATAL_ERROR
            "MPP backend requested but librockchip_mpp.so was not found at "
            "${MPP_INSTALL_PREFIX}/lib")
    endif()
    set(MPP_LIB_DIR "${MPP_INSTALL_PREFIX}/lib")
    if(EXISTS "${MPP_INSTALL_PREFIX}/include/rockchip/mpp_api.h")
        set(MPP_INCLUDE_DIR "${MPP_INSTALL_PREFIX}/include/rockchip")
    else()
        set(MPP_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mpp/inc")
    endif()
endif()

if(RKVC_BUILD_BACKEND_RGA)
    set(RGA_INSTALL_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/.build/deps/rga-install"
        CACHE PATH "RGA SDK prefix (lib/ + optional include/)")
    if(NOT EXISTS "${RGA_INSTALL_PREFIX}/lib/librga.so")
        message(FATAL_ERROR
            "RGA backend requested but librga.so was not found at "
            "${RGA_INSTALL_PREFIX}/lib")
    endif()
    set(RGA_LIB_DIR "${RGA_INSTALL_PREFIX}/lib")
    # 优先使用 SDK 前缀内的头；缺失时回退到只读子模块头（版本一致由
    # rkvc-build 适配器保证：SDK 前缀正是从该子模块装出的）。
    if(EXISTS "${RGA_INSTALL_PREFIX}/include/im2d.h")
        set(RGA_INCLUDE_DIR "${RGA_INSTALL_PREFIX}/include")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/librga/include/im2d.h")
        set(RGA_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/librga/include")
    else()
        message(FATAL_ERROR
            "RGA headers not found in ${RGA_INSTALL_PREFIX}/include or "
            "third_party/librga/include")
    endif()
endif()

if(RKVC_BUILD_BACKEND_RKNN OR RKVC_BUILD_BACKEND_MLVC)
    set(RKNN_INSTALL_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/.build/deps/rknn-install"
        CACHE PATH "RKNN Runtime SDK prefix (include/ + lib/)")
    if(NOT EXISTS "${RKNN_INSTALL_PREFIX}/lib/librknnrt.so")
        message(FATAL_ERROR
            "RKNN backend requested but librknnrt.so was not found at "
            "${RKNN_INSTALL_PREFIX}/lib")
    endif()
    set(RKNN_LIB_DIR "${RKNN_INSTALL_PREFIX}/lib")
    if(EXISTS "${RKNN_INSTALL_PREFIX}/include/rknn_api.h")
        set(RKNN_INCLUDE_DIR "${RKNN_INSTALL_PREFIX}/include")
    elseif(EXISTS "${RKNN_INSTALL_PREFIX}/include/rknn/rknn_api.h")
        set(RKNN_INCLUDE_DIR "${RKNN_INSTALL_PREFIX}/include/rknn")
    else()
        message(FATAL_ERROR
            "RKNN backend requested but rknn_api.h was not found below "
            "${RKNN_INSTALL_PREFIX}/include")
    endif()
endif()

if(RKVC_BUILD_BACKEND_SVT)
    set(SVT_AV1_INSTALL_PREFIX
        "${CMAKE_CURRENT_SOURCE_DIR}/.build/deps/svt-av1-install"
        CACHE PATH "SVT-AV1 install prefix (lib/ + include/svt-av1/)")
    if(NOT EXISTS "${SVT_AV1_INSTALL_PREFIX}/lib/libSvtAv1Enc.so")
        message(FATAL_ERROR
            "SVT backend requested but libSvtAv1Enc.so was not found at "
            "${SVT_AV1_INSTALL_PREFIX}/lib\n"
            "  Build/install SVT-AV1 from third_party/SVT-AV1 into "
            "${SVT_AV1_INSTALL_PREFIX} first.")
    endif()
    set(SVT_AV1_LIB_DIR "${SVT_AV1_INSTALL_PREFIX}/lib")
    if(EXISTS "${SVT_AV1_INSTALL_PREFIX}/include/svt-av1/EbSvtAv1Enc.h")
        set(SVT_AV1_INCLUDE_DIR
            "${SVT_AV1_INSTALL_PREFIX}/include/svt-av1")
    else()
        message(FATAL_ERROR
            "SVT backend requested but EbSvtAv1Enc.h was not found under "
            "${SVT_AV1_INSTALL_PREFIX}/include/svt-av1")
    endif()
endif()

# FFmpeg 容器后端链接源码树内的共享库：需先在
# third_party/ffmpeg-rockchip 中 configure && make（--enable-shared，
# 默认 libavcodec/libavformat/libavutil 三个子目录产出 .so）。
if(RKVC_BUILD_BACKEND_FFMPEG)
    set(FFMPEG_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg-rockchip")
    if(NOT EXISTS "${FFMPEG_SRC_DIR}/libavcodec/libavcodec.so" OR
       NOT EXISTS "${FFMPEG_SRC_DIR}/libavformat/libavformat.so" OR
       NOT EXISTS "${FFMPEG_SRC_DIR}/libavutil/libavutil.so")
        message(FATAL_ERROR
            "FFmpeg backend requested but ffmpeg-rockchip shared libs were "
            "not found in ${FFMPEG_SRC_DIR}\n"
            "  Run the ffmpeg-rockchip configure/make step first.")
    endif()
    set(FFMPEG_INCLUDE_DIRS
        "${FFMPEG_SRC_DIR}"
        "${FFMPEG_SRC_DIR}/libavcodec"
        "${FFMPEG_SRC_DIR}/libavformat"
        "${FFMPEG_SRC_DIR}/libavutil")
    set(FFMPEG_LIB_DIRS
        "${FFMPEG_SRC_DIR}/libavcodec"
        "${FFMPEG_SRC_DIR}/libavformat"
        "${FFMPEG_SRC_DIR}/libavutil")
endif()
