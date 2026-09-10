cmake_minimum_required(VERSION 3.21)
project(rkvc-upscale-file LANGUAGES C CXX)

# Standalone fixed-3x upscale sample (mirrors `rkvc upscale`):
#   cmake -S . -B build -DRKVC_CORE_DIR=/path/to/core \
#     -DRKVC_MODEL=/path/to/phase-rlfn-bench.rkmodel
#   ./build/upscale_file <plugin-dir> <w> <h> nv12 <model-id> in.nv12 out.nv12 <model.rkmodel>
set(RKVC_CORE_DIR "" CACHE PATH "rkvc core source dir")
if(NOT RKVC_CORE_DIR)
    message(FATAL_ERROR "RKVC_CORE_DIR is required")
endif()
add_subdirectory(${RKVC_CORE_DIR} rkvc-core-build)

add_executable(upscale_file upscale_file.c)
target_link_libraries(upscale_file PRIVATE rkvc-core-static)
target_include_directories(upscale_file PRIVATE
    ${RKVC_CORE_DIR}/include)
