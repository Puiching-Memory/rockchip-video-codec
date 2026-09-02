# SPDX-License-Identifier: AGPL-3.0-or-later
# The core library is the only library implementation.

set(RKVC_SOURCES
    lib/api.c
    lib/frame.c
    lib/graph.c
    lib/executor.c
    lib/context.c
    lib/job.c
    lib/rkmodel.c
    lib/model_registry.c
    lib/backend_dso.c
    lib/builtin_backends.c
    lib/node_fileio.c
    lib/model_trust.c
)

function(rkvc_configure_library target)
    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/lib
            ${RKVC_SODIUM_INCLUDES}
    )
    target_compile_definitions(${target} PRIVATE
        ${RKVC_VERSION_COMPILE_DEFS}
        ${RKVC_MODEL_SIGN_DEFS})
    target_link_libraries(${target}
        PUBLIC Threads::Threads
        PRIVATE rkvc_instrumentation ${CMAKE_DL_LIBS} ${RKVC_SODIUM_LIBS})
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME rkvc
        POSITION_INDEPENDENT_CODE ON)
endfunction()

if(NOT RKVC_BUILD_SHARED AND NOT RKVC_BUILD_STATIC)
    message(FATAL_ERROR "At least one of RKVC_BUILD_SHARED/RKVC_BUILD_STATIC must be ON")
endif()

if(RKVC_BUILD_SHARED)
    add_library(rkvc_shared SHARED ${RKVC_SOURCES})
    rkvc_configure_library(rkvc_shared)
    target_link_options(rkvc_shared PRIVATE
        "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/librkvc.map"
        "-Wl,-Bsymbolic-functions")
    set_target_properties(rkvc_shared PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION 0)
    add_library(rkvc::shared ALIAS rkvc_shared)
endif()

if(RKVC_BUILD_STATIC)
    add_library(rkvc_static STATIC ${RKVC_SOURCES})
    rkvc_configure_library(rkvc_static)
    add_library(rkvc::static ALIAS rkvc_static)
endif()

if(RKVC_BUILD_SHARED)
    add_library(rkvc ALIAS rkvc_shared)
    add_library(rkvc::rkvc ALIAS rkvc_shared)
else()
    add_library(rkvc ALIAS rkvc_static)
    add_library(rkvc::rkvc ALIAS rkvc_static)
endif()

if(RKVC_BUILD_CLI)
    add_executable(rkvc_cli rkvc.c)
    set_target_properties(rkvc_cli PROPERTIES
        OUTPUT_NAME rkvc
        INSTALL_RPATH "$ORIGIN/../lib"
        BUILD_RPATH "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(rkvc_cli PRIVATE rkvc rkvc_instrumentation)
    target_include_directories(rkvc_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/lib)
endif()

if(RKVC_BUILD_EXAMPLES)
    foreach(example decode_file encode_file transcode stream_ports
                    roi_encode adaptive_bitrate live_capture net_loopback
                    live_transcode_ports upscale_ctx)
        add_executable(example_${example}
            examples/${example}.c examples/example_common.c)
        target_include_directories(example_${example} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/examples)
        target_link_libraries(example_${example} PRIVATE
            rkvc rkvc_instrumentation)
        set_target_properties(example_${example} PROPERTIES
            INSTALL_RPATH "$ORIGIN/../../../lib"
            BUILD_RPATH "${CMAKE_CURRENT_BINARY_DIR}")
    endforeach()
endif()

if(RKVC_BUILD_BACKEND_MPP)
    add_library(rkvc_backend_mpp MODULE backends/backend_mpp.c)
    target_include_directories(rkvc_backend_mpp PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${MPP_INCLUDE_DIR})
    target_link_directories(rkvc_backend_mpp PRIVATE ${MPP_LIB_DIR})
    target_link_libraries(rkvc_backend_mpp PRIVATE rockchip_mpp)
    set_target_properties(rkvc_backend_mpp PROPERTIES
        PREFIX ""
        OUTPUT_NAME rkvc_backend_mpp
        POSITION_INDEPENDENT_CODE ON
        BUILD_RPATH "${MPP_LIB_DIR}"
        INSTALL_RPATH "$ORIGIN/../..")
    target_link_options(rkvc_backend_mpp PRIVATE
        "-Wl,-soname,rkvc_backend_mpp.so")
endif()

if(RKVC_BUILD_BACKEND_RGA)
    add_library(rkvc_backend_rga MODULE backends/backend_rga.c)
    target_include_directories(rkvc_backend_rga PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${RGA_INCLUDE_DIR})
    target_link_directories(rkvc_backend_rga PRIVATE ${RGA_LIB_DIR})
    target_link_libraries(rkvc_backend_rga PRIVATE rga)
    set_target_properties(rkvc_backend_rga PROPERTIES
        PREFIX ""
        OUTPUT_NAME rkvc_backend_rga
        POSITION_INDEPENDENT_CODE ON
        BUILD_RPATH "${RGA_LIB_DIR}"
        INSTALL_RPATH "$ORIGIN/../..")
    target_link_options(rkvc_backend_rga PRIVATE
        "-Wl,-soname,rkvc_backend_rga.so")
endif()

if(RKVC_BUILD_BACKEND_RKNN)
    add_library(rkvc_backend_rknn MODULE backends/backend_rknn.c)
    target_include_directories(rkvc_backend_rknn PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${RKNN_INCLUDE_DIR})
    target_link_directories(rkvc_backend_rknn PRIVATE ${RKNN_LIB_DIR})
    target_link_libraries(rkvc_backend_rknn PRIVATE rknnrt m)
    set_target_properties(rkvc_backend_rknn PROPERTIES
        PREFIX ""
        OUTPUT_NAME rkvc_backend_rknn
        POSITION_INDEPENDENT_CODE ON
        BUILD_RPATH "${RKNN_LIB_DIR}"
        INSTALL_RPATH "$ORIGIN/../..")
    target_link_options(rkvc_backend_rknn PRIVATE
        "-Wl,-soname,rkvc_backend_rknn.so")
endif()

if(RKVC_BUILD_BACKEND_MLVC)
    add_library(rkvc_backend_mlvc MODULE
        backends/backend_mlvc.c
        backends/mlvc/rans.c
        backends/mlvc/mlvc_pixel.c
        backends/mlvc/pmf.c
        backends/mlvc/qppatch.c
        backends/mlvc/container.c)
    target_include_directories(rkvc_backend_mlvc PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/backends
        ${RKNN_INCLUDE_DIR})
    target_link_directories(rkvc_backend_mlvc PRIVATE ${RKNN_LIB_DIR})
    target_link_libraries(rkvc_backend_mlvc PRIVATE rknnrt m)
    set_target_properties(rkvc_backend_mlvc PROPERTIES
        PREFIX ""
        OUTPUT_NAME rkvc_backend_mlvc
        POSITION_INDEPENDENT_CODE ON
        BUILD_RPATH "${RKNN_LIB_DIR}"
        INSTALL_RPATH "$ORIGIN/../..")
    target_link_options(rkvc_backend_mlvc PRIVATE
        "-Wl,-soname,rkvc_backend_mlvc.so")
endif()
