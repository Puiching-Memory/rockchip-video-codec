# SPDX-License-Identifier: AGPL-3.0-or-later
# RkvcTargets.cmake — 全部库/可执行 target 定义。
#
# 依赖变量来自 RkvcDependencies.cmake；安装规则在 RkvcInstall.cmake。

# ── 库目标公共配置 ───────────────────────────────────────────────────
# shared/static 公共配置（include、特性宏、版本/授权宏）；链接可见性各自处理
function(rkvc_configure_target tgt)
    target_include_directories(${tgt}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${AVCODEC_INCLUDE_DIRS}
            ${MPP_INCLUDE_DIR}
            ${MPP_OSAL_INCLUDE_DIR}
            ${RGA_INCLUDE_DIR}
            ${RKVC_RKNN_INCLUDES}
            ${CMAKE_CURRENT_SOURCE_DIR}/lib
            ${RKVC_SODIUM_INCLUDES}
    )
    if(RKVC_RKNN_ENABLED)
        target_compile_definitions(${tgt} PRIVATE RKVC_ENABLE_RKNN=1)
        if(RKVC_RKNN_ALLOW_DEBUG_LOG)
            target_compile_definitions(${tgt} PRIVATE RKVC_RKNN_ALLOW_DEBUG_LOG=1)
        endif()
    endif()
    if(RKVC_MLVC_ENABLED)
        target_compile_definitions(${tgt} PRIVATE RKVC_ENABLE_MLVC=1)
    endif()
    # 版本号由 project(VERSION) 注入，init.c 不再硬编码
    target_compile_definitions(${tgt} PRIVATE ${RKVC_VERSION_COMPILE_DEFS} ${RKVC_LICENSE_DEFS} ${RKVC_MODEL_CRYPT_DEFS})
endfunction()

if(RKVC_BUILD_SHARED)
    add_library(rkvc_shared SHARED ${RKVC_SOURCES})
    rkvc_configure_target(rkvc_shared)
    target_link_directories(rkvc_shared PRIVATE ${RKVC_DEP_LIB_DIRS})
    target_link_options(rkvc_shared PRIVATE
        ${RKVC_RPATH_LINK_OPTIONS}
        # 仅导出公共 API(rkvc_ 前缀),内部符号全部隐藏;并让库内对自身
        # 符号的调用绑定到本 DSO 定义(防止 LD_PRELOAD 劫持校验链)。
        "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/librkvc.map"
        "-Wl,-Bsymbolic-functions")
    target_link_libraries(rkvc_shared PRIVATE rkvc_instrumentation ${FFMPEG_LIBS} ${EXTRA_LIBS} ${RKVC_RKNN_LIBS} ${RKVC_SODIUM_LIBS})
    set_target_properties(rkvc_shared PROPERTIES
        OUTPUT_NAME rkvc
        VERSION     ${PROJECT_VERSION}
        SOVERSION   0
        POSITION_INDEPENDENT_CODE ON
    )
    # soname alias
    add_library(rkvc::shared ALIAS rkvc_shared)
endif()

if(RKVC_BUILD_STATIC)
    add_library(rkvc_static STATIC ${RKVC_SOURCES})
    rkvc_configure_target(rkvc_static)
    target_link_directories(rkvc_static PUBLIC ${RKVC_DEP_LIB_DIRS})
    target_link_options(rkvc_static INTERFACE ${RKVC_RPATH_LINK_OPTIONS})
    target_link_libraries(rkvc_static PUBLIC rkvc_instrumentation ${FFMPEG_LIBS} ${EXTRA_LIBS} ${RKVC_RKNN_LIBS} ${RKVC_SODIUM_LIBS})
    set_target_properties(rkvc_static PROPERTIES OUTPUT_NAME rkvc)
    add_library(rkvc::static ALIAS rkvc_static)
endif()

# 默认别名
if(RKVC_BUILD_SHARED)
    add_library(rkvc ALIAS rkvc_shared)
elseif(RKVC_BUILD_STATIC)
    add_library(rkvc ALIAS rkvc_static)
endif()

# ── 0.4 新引擎（rkvc_core）───────────────────────────────────────
# 独立于旧 rkvc_shared 构建，作为可移植的公共 ABI + 图内核核心。它只依赖
# libc 与 pthread，不含 FFmpeg/MPP/RGA/RKNN 类型，所以可在无硬件容器独立
# 编译验证，也能在交叉工具链上作为稳定 rkvc 核心与 rkvc_backend_* 协作。
# 注意: 新引擎与旧引擎都导出 rkvc_version()，因此二者不能链接进同一二进制；
# 迁移期由 rkvc_core 独立承载新 ABI(见 docs/0.4.0-refactor-plan.md P1/P2)。
# P6 删除旧库后，rkvc_core 将更名为 rkvc（librkvc.so.0）。
if(RKVC_BUILD_NEW_ENGINE)
    set(RKVC_CORE_SOURCES
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
        lib/model_trust.c
    )
    add_library(rkvc_core ${RKVC_CORE_SOURCES})
    target_include_directories(rkvc_core
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/lib
    )
    target_compile_definitions(rkvc_core PRIVATE ${RKVC_VERSION_COMPILE_DEFS})
    find_package(Threads REQUIRED)
    # .rkmodel 载荷摘要（SHA-256）是核心契约，rkvc_core 始终链接 libsodium
    target_include_directories(rkvc_core PRIVATE ${RKVC_SODIUM_INCLUDES})
    target_link_libraries(rkvc_core PUBLIC Threads::Threads
                                   PRIVATE ${CMAKE_DL_LIBS} ${RKVC_SODIUM_LIBS})
    if(RKVC_ENABLE_MODEL_SIGN)
        target_compile_definitions(rkvc_core PRIVATE ${RKVC_MODEL_SIGN_DEFS})
    endif()
    if(NOT RKVC_BUILD_SHARED AND NOT RKVC_BUILD_STATIC)
        set(_RKVC_CORE_OUTPUT_NAME rkvc)
        set(_RKVC_CORE_VERSION_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/librkvc-0.4.map")
    else()
        set(_RKVC_CORE_OUTPUT_NAME rkvc_core)
        set(_RKVC_CORE_VERSION_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/librkvc.map")
    endif()
    set_target_properties(rkvc_core PROPERTIES
        OUTPUT_NAME ${_RKVC_CORE_OUTPUT_NAME}
        POSITION_INDEPENDENT_CODE ON
    )
    if(BUILD_SHARED_LIBS)
        set_target_properties(rkvc_core PROPERTIES
            VERSION   ${PROJECT_VERSION}
            SOVERSION 0
        )
        # 导出符号仅限公共 ABI（rkvc_ 前缀白名单）
        if(EXISTS "${_RKVC_CORE_VERSION_SCRIPT}")
            target_link_options(rkvc_core PRIVATE
                "-Wl,--version-script=${_RKVC_CORE_VERSION_SCRIPT}"
                "-Wl,-Bsymbolic-functions")
        endif()
    endif()
    add_library(rkvc::core ALIAS rkvc_core)

    # 单一 CLI（0.4 形态）：只链接新引擎，包含 inspect/version；
    # 媒体子命令由后续后端迁移逐步点亮。
    add_executable(rkvc_cli cli/rkvc.c)
    set_target_properties(rkvc_cli PROPERTIES
        OUTPUT_NAME rkvc
        INSTALL_RPATH "$ORIGIN/../lib"
        BUILD_RPATH   "${CMAKE_CURRENT_BINARY_DIR}"
    )
    target_link_libraries(rkvc_cli PRIVATE rkvc::core rkvc_instrumentation)
    target_include_directories(rkvc_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/lib)
endif()

# ── 0.4 MPP decoder backend DSO ─────────────────────────────────────
if(RKVC_BUILD_BACKEND_MPP)
    if(NOT RKVC_BUILD_NEW_ENGINE)
        message(FATAL_ERROR "RKVC_BUILD_BACKEND_MPP requires RKVC_BUILD_NEW_ENGINE=ON")
    endif()
    if(NOT EXISTS "${MPP_INSTALL_PREFIX}/lib/librockchip_mpp.so")
        message(FATAL_ERROR
            "Bundled MPP not found at ${MPP_INSTALL_PREFIX}; build the project-owned dependency first")
    endif()
    add_library(rkvc_backend_mpp MODULE backends/mpp/backend_mpp.c)
    target_include_directories(rkvc_backend_mpp PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${MPP_INCLUDE_DIR})
    target_link_directories(rkvc_backend_mpp PRIVATE ${MPP_LIB_DIR})
    target_link_libraries(rkvc_backend_mpp PRIVATE
        rockchip_mpp)
    set_target_properties(rkvc_backend_mpp PROPERTIES
        PREFIX ""
        OUTPUT_NAME rkvc_backend_mpp
        POSITION_INDEPENDENT_CODE ON
        BUILD_RPATH "${MPP_LIB_DIR}"
        INSTALL_RPATH "$ORIGIN/../..")
endif()

# ── 示例程序 ──────────────────────────────────────────────────────────
if(RKVC_BUILD_EXAMPLES AND (RKVC_BUILD_SHARED OR RKVC_BUILD_STATIC))
    set(EXAMPLE_TARGETS "")

    foreach(EXAMPLE encode_file decode_file transcode live_capture live_transcode_ports adaptive_bitrate roi_encode stream_ports upscale_ctx net_loopback)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/${EXAMPLE}.c")
            add_executable(example_${EXAMPLE} examples/${EXAMPLE}.c)
            target_link_libraries(example_${EXAMPLE} PRIVATE rkvc)
            target_include_directories(example_${EXAMPLE}
                PRIVATE ${AVCODEC_INCLUDE_DIRS})
            target_link_directories(example_${EXAMPLE}
                PRIVATE ${RKVC_DEP_LIB_DIRS})
            target_link_options(example_${EXAMPLE}
                PRIVATE ${RKVC_RPATH_LINK_OPTIONS})
            set_target_properties(example_${EXAMPLE} PROPERTIES
                BUILD_RPATH "${CMAKE_CURRENT_BINARY_DIR};${RKVC_DEP_LIB_DIRS}"
            )
            list(APPEND EXAMPLE_TARGETS example_${EXAMPLE})
        endif()
    endforeach()

    if(TARGET example_net_loopback)
        target_link_libraries(example_net_loopback PRIVATE pthread)
    endif()
    if(TARGET example_adaptive_bitrate)
        target_link_libraries(example_adaptive_bitrate PRIVATE pthread)
    endif()
    if(TARGET example_stream_ports)
        target_link_libraries(example_stream_ports PRIVATE pthread)
    endif()
    if(TARGET example_live_transcode_ports)
        # 直接使用静态 FFmpeg(libavformat/libavcodec .a)，依赖不传递：
        # avcodec 的 libsvtav1 编码器需 SvtAv1Enc、av1_rkmpp 解码器需
        # rockchip_mpp/drm 等，与库自身链接集保持一致。
        target_link_libraries(example_live_transcode_ports
            PRIVATE ${FFMPEG_LIBS} ${EXTRA_LIBS})
    endif()
endif()

# ── 旧版 CLI (cli/ → 安装到 bin/) ──────────────────────────────────
# 0.4 迁移期保留；P4 单一 rkvc CLI 覆盖全部功能后删除（见 P6）。
if(RKVC_BUILD_CLI AND (RKVC_BUILD_SHARED OR RKVC_BUILD_STATIC))
    set(RKVC_CLI_SOURCES
        cli/rkvc_bench.c
        cli/rkvc_encode.c
        cli/rkvc_decode.c
        cli/rkvc_transcode.c
        cli/rkvc_yuv_upscale.c
        cli/rkvc_session_upscale.c
        cli/rkvc_info.c
    )

    set(RKVC_CLI_TARGETS "")
    foreach(CLI_SRC ${RKVC_CLI_SOURCES})
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${CLI_SRC}")
            get_filename_component(CLI_NAME ${CLI_SRC} NAME_WE)
            set(CLI_EXTRA "")
            if(CLI_NAME STREQUAL "rkvc_encode" OR
               CLI_NAME STREQUAL "rkvc_transcode" OR
               CLI_NAME STREQUAL "rkvc_bench")
                set(CLI_EXTRA cli/cli_parse.c)
            endif()
            add_executable(${CLI_NAME} ${CLI_SRC} ${CLI_EXTRA})
            target_link_libraries(${CLI_NAME} PRIVATE rkvc)
            target_include_directories(${CLI_NAME}
                PRIVATE
                    ${AVCODEC_INCLUDE_DIRS}
                    ${CMAKE_CURRENT_SOURCE_DIR}/cli)
            target_link_directories(${CLI_NAME}
                PRIVATE ${RKVC_DEP_LIB_DIRS})
            target_link_options(${CLI_NAME}
                PRIVATE ${RKVC_RPATH_LINK_OPTIONS})
            # RPATH: 安装后从 bin/ 找 ../lib/ 中的 .so
            set_target_properties(${CLI_NAME} PROPERTIES
                INSTALL_RPATH "$ORIGIN/../lib"
                BUILD_RPATH   "${CMAKE_CURRENT_BINARY_DIR};${RKVC_DEP_LIB_DIRS}"
            )
            list(APPEND RKVC_CLI_TARGETS ${CLI_NAME})
        endif()
    endforeach()

    add_custom_target(bench-rd
        COMMAND ${CMAKE_SOURCE_DIR}/tools/bench/run_rd_benchmark.sh
        DEPENDS ${RKVC_CLI_TARGETS}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Run RD benchmark (requires SRC_VIDEO or pass video path to script)"
        VERBATIM
    )
endif()

# ── 授权/模型工具（独立于 librkvc，不随可移植包分发） ───────────────
if(RKVC_ENABLE_LICENSE)
    # rkvc_lic_client 为分发到客户机的裁剪版：仅 machine-id/verify，
    # 不含 genkey/issue/inspect，避免把私钥签发能力随包分发给终端客户。
    add_executable(rkvc_lic tools/rkvc_lic.c lib/license_machine.c)
    target_include_directories(rkvc_lic PRIVATE
            ${RKVC_SODIUM_INCLUDES} ${CMAKE_CURRENT_SOURCE_DIR}/lib)
    target_link_libraries(rkvc_lic PRIVATE ${RKVC_SODIUM_LIBS})

    add_executable(rkvc_lic_client tools/rkvc_lic.c lib/license_machine.c)
    target_compile_definitions(rkvc_lic_client PRIVATE RKVC_LIC_MACHINE_ONLY=1)
    target_include_directories(rkvc_lic_client PRIVATE
            ${RKVC_SODIUM_INCLUDES} ${CMAKE_CURRENT_SOURCE_DIR}/lib)
    target_link_libraries(rkvc_lic_client PRIVATE ${RKVC_SODIUM_LIBS})
endif()

if(RKVC_ENABLE_MODEL_CRYPT)
    # 打包方加密/签发工具（独立于 librkvc，不随可移植包分发）
    add_executable(rkvc_model_crypt tools/rkvc_model_crypt.c lib/license_machine.c)
    target_include_directories(rkvc_model_crypt PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/lib
        ${RKVC_SODIUM_INCLUDES})
    target_link_libraries(rkvc_model_crypt PRIVATE ${RKVC_SODIUM_LIBS})

    # 客户侧只读机器码采集器：不包含 genkey/issue/decrypt 等发码能力。
    add_executable(rkvc_model_id tools/rkvc_model_id.c lib/license_machine.c)
    target_include_directories(rkvc_model_id PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/lib
        ${RKVC_SODIUM_INCLUDES})
    target_link_libraries(rkvc_model_id PRIVATE ${RKVC_SODIUM_LIBS})
endif()
