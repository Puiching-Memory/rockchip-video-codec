cmake_minimum_required(VERSION 3.21)
project(rkvc-integration-c LANGUAGES C CXX)

# Contract sample for embedding rkvc (mirrors the SDK adapter shape):
#   cmake -S . -B build -DRKVC_CORE_DIR=/path/to/core \
#     -DRKVC_CODEC_PLUGIN=/path/to/rkvc_av1.so
#   ./build/intc_encode <plugin.so> <width> <height> <frames>
set(RKVC_CORE_DIR "" CACHE PATH "rkvc core source dir")
if(NOT RKVC_CORE_DIR)
    message(FATAL_ERROR "RKVC_CORE_DIR is required")
endif()
add_subdirectory(${RKVC_CORE_DIR} rkvc-core-build)

add_executable(intc_encode main.c)
target_link_libraries(intc_encode PRIVATE rkvc-core-static)
target_include_directories(intc_encode PRIVATE
    ${RKVC_CORE_DIR}/include)
