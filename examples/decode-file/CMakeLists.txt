cmake_minimum_required(VERSION 3.21)
project(rkvc-decode-file LANGUAGES C CXX)

# Standalone FILE-decode sample (mirrors `rkvc decode`):
#   cmake -S . -B build -DRKVC_CORE_DIR=/path/to/core \
#     -DRKVC_CODEC_PLUGIN=/path/to/rkvc_h264h265.so
#   ./build/decode_file <plugin-dir> h264 <w> <h> nv12 in.bit out.nv12
set(RKVC_CORE_DIR "" CACHE PATH "rkvc core source dir")
if(NOT RKVC_CORE_DIR)
    message(FATAL_ERROR "RKVC_CORE_DIR is required")
endif()
add_subdirectory(${RKVC_CORE_DIR} rkvc-core-build)

add_executable(decode_file decode_file.c)
target_link_libraries(decode_file PRIVATE rkvc-core-static)
target_include_directories(decode_file PRIVATE
    ${RKVC_CORE_DIR}/include)
