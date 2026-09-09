cmake_minimum_required(VERSION 3.21)
project(rkvc VERSION 0.5.0 LANGUAGES C CXX)

# Thin aggregation over the decoupled C++20 projects. Each subproject also
# configures standalone (codec projects fall back to add_subdirectory(core)).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(RKVC_BUILD_CLI "Build the rkvc CLI" ON)
option(RKVC_BUILD_CODECS "Build the codec plugins" ON)

add_subdirectory(core)

if(RKVC_BUILD_CODECS)
    add_subdirectory(codecs/h264h265)
    add_subdirectory(codecs/mlvc)
    add_subdirectory(codecs/av1)
    add_subdirectory(codecs/sr)
endif()

if(RKVC_BUILD_CLI)
    add_subdirectory(cli)
endif()

include(CTest)
