# SPDX-License-Identifier: AGPL-3.0-or-later

if(NOT DEFINED GIT_EXECUTABLE OR NOT EXISTS "${GIT_EXECUTABLE}")
    message(FATAL_ERROR "GIT_EXECUTABLE is required")
endif()
if(NOT DEFINED RKVC_SOURCE_DIR)
    message(FATAL_ERROR "RKVC_SOURCE_DIR is required")
endif()

set(_ffmpeg "${RKVC_SOURCE_DIR}/third_party/ffmpeg-rockchip")
set(_patch_dir "${RKVC_SOURCE_DIR}/patches/ffmpeg-rockchip")
if(NOT EXISTS "${_ffmpeg}/.git")
    message(FATAL_ERROR "ffmpeg-rockchip submodule is not initialized")
endif()

file(GLOB _patches "${_patch_dir}/*.patch")
if(NOT _patches)
    message(FATAL_ERROR "no ffmpeg-rockchip patches found")
endif()
foreach(_patch IN LISTS _patches)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${_ffmpeg}" apply --check "${_patch}"
        RESULT_VARIABLE _forward
        OUTPUT_QUIET ERROR_QUIET)
    if(NOT _forward EQUAL 0)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${_ffmpeg}"
                    apply --reverse --check "${_patch}"
            RESULT_VARIABLE _reverse
            OUTPUT_QUIET ERROR_QUIET)
        if(NOT _reverse EQUAL 0)
            message(FATAL_ERROR
                "patch conflicts with pinned ffmpeg-rockchip: ${_patch}")
        endif()
    endif()
    message(STATUS "ffmpeg patch OK: ${_patch}")
endforeach()
