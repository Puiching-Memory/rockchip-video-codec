# SPDX-License-Identifier: AGPL-3.0-or-later
# RkvcDependencies.cmake — 定位所有外部依赖并组装特性相关的源文件清单。
#
# 只产生变量与生成文件，不创建库/可执行 target（见 RkvcTargets.cmake）。
# 交叉构建时所有路径必须落在 sysroot 或本仓 .build/deps 前缀内，
# 不允许从宿主 /usr 为目标链接。

# ── FFmpeg (ffmpeg-rockchip) ──────────────────────────────────────────
# 原生开发构建可直接消费子模块源码树中的静态库；交叉/portable 构建通过
# FFMPEG_PREFIX 指向隔离安装前缀，避免把 x86 与 AArch64 产物写进同一源码树。
set(FFMPEG_PREFIX "" CACHE PATH "ffmpeg-rockchip install prefix (cross/portable builds)")
if(FFMPEG_PREFIX)
    set(AVCODEC_INCLUDE_DIRS "${FFMPEG_PREFIX}/include")
    set(AVCODEC_LIB_DIRS "${FFMPEG_PREFIX}/lib")
else()
    set(AVCODEC_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/third_party/ffmpeg-rockchip")
    set(AVCODEC_LIB_DIRS
        "${CMAKE_SOURCE_DIR}/third_party/ffmpeg-rockchip/libavcodec"
        "${CMAKE_SOURCE_DIR}/third_party/ffmpeg-rockchip/libavformat"
        "${CMAKE_SOURCE_DIR}/third_party/ffmpeg-rockchip/libavutil"
        "${CMAKE_SOURCE_DIR}/third_party/ffmpeg-rockchip/libswscale"
    )
endif()

# ── MPP (rockchip mpp) ───────────────────────────────────────────────
# 始终使用 third_party/ 源码树构建的静态库
set(MPP_BUILD_DIR "${CMAKE_BINARY_DIR}/mpp_build" CACHE PATH "MPP build output directory")
set(MPP_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/.build/deps/mpp-install" CACHE PATH "MPP install prefix")
if(EXISTS "${MPP_INSTALL_PREFIX}/lib/librockchip_mpp.so")
    set(MPP_LIB_DIR "${MPP_INSTALL_PREFIX}/lib")
else()
    set(MPP_LIB_DIR "${MPP_BUILD_DIR}/mpp")
endif()
set(MPP_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/third_party/mpp/inc")
set(MPP_OSAL_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/third_party/mpp/osal/inc")

set(SVT_PREFIX "${CMAKE_SOURCE_DIR}/.build/deps/svt-av1-install" CACHE PATH "SVT-AV1 install prefix")

# librga (submodule 预编译库，经 scripts/install-librga.sh 安装)
set(RGA_PREFIX "${CMAKE_SOURCE_DIR}/.build/deps/librga-install" CACHE PATH "librga install prefix")
set(RGA_INCLUDE_DIR "${RGA_PREFIX}/include")
if(RKVC_BUILD_SHARED OR RKVC_BUILD_STATIC)
    if(NOT EXISTS "${RGA_PREFIX}/lib/librga.so")
        message(FATAL_ERROR
            "librga not found at ${RGA_PREFIX}/lib/librga.so\n"
            "  Run: git submodule update --init --depth 1 third_party/librga\n"
            "       ./scripts/install-librga.sh")
    endif()
endif()

# librknnrt（Rockchip 专有运行时，经 scripts/install-rknnrt.sh 下载到本仓前缀）
set(RKNN_PREFIX "${CMAKE_SOURCE_DIR}/.build/deps/rknn-install" CACHE PATH "RKNN runtime install prefix")

set(RKVC_DEP_LIB_DIRS
    ${AVCODEC_LIB_DIRS}
    ${MPP_LIB_DIR}
    ${SVT_PREFIX}/lib
    ${RGA_PREFIX}/lib
    ${RKNN_PREFIX}/lib
)

set(RKVC_RPATH_LINK_OPTIONS "")
foreach(RKVC_DEP_LIB_DIR IN LISTS RKVC_DEP_LIB_DIRS)
    list(APPEND RKVC_RPATH_LINK_OPTIONS "-Wl,-rpath-link,${RKVC_DEP_LIB_DIR}")
endforeach()

# ── 链接组 ───────────────────────────────────────────────────────────
# FFmpeg 静态库存在循环依赖，需要用 --start-group/--end-group 包裹。
# libswscale 用于解码器在硬件无法直接输出请求格式时的软件像素格式转换
# (例如 8-bit HEVC 流硬件只能下载为 NV12，需 sws_scale 转 YUV420P/NV16/P010)。
# SvtAv1Enc：FFmpeg libsvtav1 编码器静态链接引用 SVT-AV1 符号（svt_av1_enc_*），
# 必须在 avcodec 之后显式链接（静态 FFmpeg 不传递依赖）。
set(FFMPEG_LIBS
    -Wl,--start-group avcodec avformat avutil swscale -Wl,--end-group
    SvtAv1Enc
)
# 外部依赖：MPP、zlib、drm
set(EXTRA_LIBS
    -Wl,--no-as-needed
    rockchip_mpp
    -Wl,--as-needed
    z drm pthread m rga rt
)

# ── 特性源文件组装 ───────────────────────────────────────────────────
set(RKVC_SOURCES
    lib/init.c
    lib/platform.c
    lib/ffmpeg_util.c
    lib/utils.c
    lib/buffer_pool.c
    lib/router.c
    lib/pipeline.c
    lib/port.c
    lib/scheduler.c
    lib/session.c
    lib/session_scale.c
    lib/session_run.c
    lib/session_roi.c
    lib/session_reconfig.c
    lib/runtime.c
    lib/net.c
    lib/node_demux.c
    lib/node_mux.c
    lib/node_mpp_dec.c
    lib/node_mpp_enc.c
    lib/node_svt_enc.c
    lib/node_v4l2.c
    lib/node_rga.c
    lib/node_post_upscale.c
    lib/node_dma_to_host.c
    lib/rkvc_sr_phase.c
    lib/node_rkvc_sr.c
    lib/qppatch.c
)

if(RKVC_ENABLE_RKNN)
    # 可移植构建只认本仓前缀（./scripts/install-rknnrt.sh），避免链到系统 /usr 的 librknnrt。
    if(EXISTS "${RKNN_PREFIX}/lib/librknnrt.so" AND EXISTS "${RKNN_PREFIX}/include/rknn_api.h")
        set(RKNNRT_LIB "${RKNN_PREFIX}/lib/librknnrt.so")
        set(RKNN_INCLUDE_DIR "${RKNN_PREFIX}/include")
        message(STATUS "RKNN runtime: ${RKNNRT_LIB}")
        set(RKVC_RKNN_LIBS ${RKNNRT_LIB})
        set(RKVC_RKNN_INCLUDES ${RKNN_INCLUDE_DIR})
        set(RKVC_RKNN_ENABLED 1)
    else()
        message(WARNING
            "RKVC_ENABLE_RKNN=ON but librknnrt not found at ${RKNN_PREFIX}\n"
            "  Run: ./scripts/install-rknnrt.sh\n"
            "  Building without NPU SR / MLVC")
    endif()
endif()

# ── MLVC 神经视频编解码（RKNN NPU + 纯 C rANS）────────────────────
# 依赖 RKVC_ENABLE_RKNN（NPU 模型推理）。
# 熵编解码器为纯 C 实现（lib/rans.c），完整移植自 msrtc_rans，
# 不再依赖 third_party/mlvc 子模块或 C++17。
# 缺 librknnrt 时自动降级（与 RKVC_ENABLE_RKNN 一致）：有 NPU 环境自动启用，
# 无 NPU 环境（CI/release）构建不失败、跳过 MLVC。
if(RKVC_ENABLE_MLVC)
    if(NOT RKVC_RKNN_ENABLED)
        message(WARNING
            "RKVC_ENABLE_MLVC=ON but librknnrt not available (RKNN disabled)\n"
            "  Run: ./scripts/install-rknnrt.sh\n"
            "  Building without MLVC")
    else()
        set(RKVC_MLVC_SRC  lib/node_mlvc.c lib/rans.c lib/mlvc_pixel.c)
        set(RKVC_MLVC_ENABLED 1)
        message(STATUS "MLVC neural codec: enabled (pure C rANS)")
    endif()
endif()
if(RKVC_MLVC_ENABLED)
    list(APPEND RKVC_SOURCES ${RKVC_MLVC_SRC})
endif()

# ── 授权（1机1码，Ed25519） ────────────────────────────────────────
# 通过 third_party/libsodium 子模块提供 SHA-256 + Ed25519，无需系统 OpenSSL/mbedTLS。
# libsodium 使用 autotools 构建，沿用本项目对非 CMake 子模块（librga/SVT-AV1）的
# 「install 脚本 + 前缀」惯例：先运行 ./scripts/install-libsodium.sh。
# 单一选项 RKVC_ENABLE_LICENSE：开启即编译授权模块 + 运行时强制校验。
#
# 公钥来源（二选一）:
#   RKVC_LICENSE_PUBKEY_FILE=<path>  指向 32 字节公钥二进制文件（生产密钥，推荐）
#   未设置                           使用 lib/license_pubkey.c 演示公钥（仅开发）
if(RKVC_ENABLE_LICENSE)
    set(LIBSODIUM_PREFIX "${CMAKE_SOURCE_DIR}/.build/deps/libsodium-install"
        CACHE PATH "libsodium install prefix (see scripts/install-libsodium.sh)")
    if(NOT EXISTS "${LIBSODIUM_PREFIX}/lib/libsodium.a")
        message(FATAL_ERROR
            "libsodium not found at ${LIBSODIUM_PREFIX}/lib/libsodium.a\n"
            "  Run: git submodule update --init third_party/libsodium\n"
            "       ./scripts/install-libsodium.sh")
    endif()
    list(APPEND RKVC_SOURCES lib/license.c lib/license_machine.c)
    # libsodium 静态库依赖线程原语；旧 glibc(<2.34)/musl 上需显式链接 pthread，
    # 用 Threads::Threads 让 librkvc 与 rkvc_lic 都自包含地拿到线程库。
    find_package(Threads REQUIRED)
    set(RKVC_LICENSE_LIBS ${LIBSODIUM_PREFIX}/lib/libsodium.a Threads::Threads)
    set(RKVC_LICENSE_INCLUDES ${LIBSODIUM_PREFIX}/include)
    set(RKVC_LICENSE_DEFS RKVC_ENABLE_LICENSE=1)

    if(RKVC_LICENSE_PUBKEY_FILE AND EXISTS "${RKVC_LICENSE_PUBKEY_FILE}")
        # 从 32 字节公钥二进制生成 C 源文件（避免手动维护 license_pubkey.c）。
        # 为避免 32 字节明文公钥直接出现在 .rodata，这里生成 XOR 混淆后的数组，
        # 实际解密在 lib/license.c 运行时完成。
        file(READ "${RKVC_LICENSE_PUBKEY_FILE}" _pubkey_hex HEX)
        string(LENGTH "${_pubkey_hex}" _hex_len)
        math(EXPR _pubkey_bytes "${_hex_len} / 2")
        if(NOT _pubkey_bytes EQUAL 32)
            message(FATAL_ERROR
                "RKVC_LICENSE_PUBKEY_FILE: expected 32 bytes, got ${_pubkey_bytes}")
        endif()
        # 混淆参数须与 lib/license.c 中的解密逻辑一致：key_i = 0xA5 ^ (i * 7)
        set(_PK_K 165)  # 0xA5
        set(_PK_C1 7)
        set(_enc_arr "")
        set(_i 0)
        while(_i LESS 64)
            string(SUBSTRING "${_pubkey_hex}" ${_i} 2 _byte)
            math(EXPR _b "0x${_byte}")
            math(EXPR _idx "${_i} / 2")
            math(EXPR _ki "(${_PK_K} ^ (${_idx} * ${_PK_C1})) & 0xFF")
            math(EXPR _eb "(((${_b} ^ ${_ki}) + 0x100) & 0xFF)" OUTPUT_FORMAT HEXADECIMAL)
            string(APPEND _enc_arr "${_eb},")
            math(EXPR _next_i "${_i} + 2")
            math(EXPR _col "(${_next_i} / 2) % 12")
            if(_col EQUAL 0 AND _next_i LESS 64)
                string(APPEND _enc_arr "\n  ")
            endif()
            set(_i ${_next_i})
        endwhile()
        set(RKVC_LICENSE_PUBKEY_C "${CMAKE_BINARY_DIR}/generated/license_pubkey.c")
        file(WRITE "${RKVC_LICENSE_PUBKEY_C}"
            "/* 由 CMake 从 RKVC_LICENSE_PUBKEY_FILE 自动生成，勿手动编辑。 */\n"
            "#include <stdint.h>\n\n"
            "const uint8_t rkvc_license_pubkey_enc[32] = {\n  ${_enc_arr}\n};\n\n"
            "const unsigned rkvc_license_pubkey_len = 32;\n")
        list(APPEND RKVC_SOURCES "${RKVC_LICENSE_PUBKEY_C}")
        message(STATUS "License (1机1码): ON, pubkey=${RKVC_LICENSE_PUBKEY_FILE}")
    else()
        list(APPEND RKVC_SOURCES lib/license_pubkey.c)
        message(WARNING
            "RKVC_ENABLE_LICENSE=ON but RKVC_LICENSE_PUBKEY_FILE not set;\n"
            "  using DEMO public key from lib/license_pubkey.c — NOT for production")
    endif()
endif()

# ── 模型自研加密层（RKVC_ENABLE_MODEL_CRYPT） ────────────────────
# 模型体用 XSalsa20-Poly1305(data_key) 加密；data_key 经内嵌 master_key 密封在
# 每机一份的 model.key 中（含机器码），运行时校验后解密。格式见
# lib/model_crypt_layout.h；打包工具见 tools/rkvc_model_crypt.c。
#
# 主密钥来源（二选一）:
#   RKVC_MODEL_MASTERKEY_FILE=<path>  指向 32 字节主密钥二进制文件（生产，推荐）
#   未设置                             使用 lib/model_key.c 演示密钥（仅开发）
if(RKVC_ENABLE_MODEL_CRYPT)
    if(NOT DEFINED RKVC_LICENSE_LIBS)
        # LICENSE 未开启时独立准备 libsodium（同一安装前缀）
        set(LIBSODIUM_PREFIX "${CMAKE_SOURCE_DIR}/.build/deps/libsodium-install"
            CACHE PATH "libsodium install prefix (see scripts/install-libsodium.sh)")
        if(NOT EXISTS "${LIBSODIUM_PREFIX}/lib/libsodium.a")
            message(FATAL_ERROR
                "libsodium not found at ${LIBSODIUM_PREFIX}/lib/libsodium.a\n"
                "  Run: git submodule update --init third_party/libsodium\n"
                "       ./scripts/install-libsodium.sh")
        endif()
        find_package(Threads REQUIRED)
        set(RKVC_LICENSE_LIBS ${LIBSODIUM_PREFIX}/lib/libsodium.a Threads::Threads)
        set(RKVC_LICENSE_INCLUDES ${LIBSODIUM_PREFIX}/include)
    endif()
    list(APPEND RKVC_SOURCES lib/model_crypt.c)
    if(NOT RKVC_ENABLE_LICENSE)
        # 机器码指纹采集（与 1机1码 同一实现）
        list(APPEND RKVC_SOURCES lib/license_machine.c)
    endif()
    set(RKVC_MODEL_CRYPT_DEFS RKVC_ENABLE_MODEL_CRYPT=1)

    if(RKVC_MODEL_MASTERKEY_FILE AND EXISTS "${RKVC_MODEL_MASTERKEY_FILE}")
        # 从 32 字节主密钥二进制生成 XOR 混淆后的 C 源（与 license pubkey 同模式），
        # 运行时解密在 lib/model_crypt.c。
        file(READ "${RKVC_MODEL_MASTERKEY_FILE}" _mk_hex HEX)
        string(LENGTH "${_mk_hex}" _hex_len)
        math(EXPR _mk_bytes "${_hex_len} / 2")
        if(NOT _mk_bytes EQUAL 32)
            message(FATAL_ERROR
                "RKVC_MODEL_MASTERKEY_FILE: expected 32 bytes, got ${_mk_bytes}")
        endif()
        # 混淆参数须与 lib/model_crypt.c 中的解密逻辑一致：key_i = 0xA5 ^ (i * 7)
        set(_MK_K 165)  # 0xA5
        set(_MK_C1 7)
        set(_mk_enc_arr "")
        set(_i 0)
        while(_i LESS 64)
            string(SUBSTRING "${_mk_hex}" ${_i} 2 _byte)
            math(EXPR _b "0x${_byte}")
            math(EXPR _idx "${_i} / 2")
            math(EXPR _ki "(${_MK_K} ^ (${_idx} * ${_MK_C1})) & 0xFF")
            math(EXPR _eb "(((${_b} ^ ${_ki}) + 0x100) & 0xFF)" OUTPUT_FORMAT HEXADECIMAL)
            string(APPEND _mk_enc_arr "${_eb},")
            math(EXPR _next_i "${_i} + 2")
            math(EXPR _col "(${_next_i} / 2) % 12")
            if(_col EQUAL 0 AND _next_i LESS 64)
                string(APPEND _mk_enc_arr "\n  ")
            endif()
            set(_i ${_next_i})
        endwhile()
        set(RKVC_MODEL_KEY_C "${CMAKE_BINARY_DIR}/generated/model_key.c")
        file(WRITE "${RKVC_MODEL_KEY_C}"
            "/* 由 CMake 从 RKVC_MODEL_MASTERKEY_FILE 自动生成，勿手动编辑。 */\n"
            "#include <stdint.h>\n\n"
            "const uint8_t model_masterkey_obfuscated[32] = {\n  ${_mk_enc_arr}\n};\n")
        list(APPEND RKVC_SOURCES "${RKVC_MODEL_KEY_C}")
        message(STATUS "Model crypt: ON, masterkey=${RKVC_MODEL_MASTERKEY_FILE}")
    else()
        list(APPEND RKVC_SOURCES lib/model_key.c)
        message(WARNING
            "RKVC_ENABLE_MODEL_CRYPT=ON but RKVC_MODEL_MASTERKEY_FILE not set;\n"
            "  using DEMO master key from lib/model_key.c — NOT for production")
    endif()
endif()

# ── .rkmodel 模型签名验证（新引擎；Ed25519 trust root）───────────────
# 与 RKVC_ENABLE_MODEL_CRYPT（旧引擎模型体加密）正交：本块只管 0.4 容器签名。
# trust root：RKVC_TRUST_PUBKEY_HEX 显式给定；否则 dev 模式读
# scripts/trust/dev-root.pub，缺失时用 rkmodel.py keygen 生成临时开发根
# （仅本机构建有效，不入库）。prod 模式（RKVC_TRUST_PRODUCTION=ON）必须显式
# 提供公钥，否则配置失败。
if(RKVC_ENABLE_MODEL_SIGN)
    if(NOT RKVC_BUILD_NEW_ENGINE)
        message(FATAL_ERROR "RKVC_ENABLE_MODEL_SIGN requires RKVC_BUILD_NEW_ENGINE=ON")
    endif()
    if(NOT DEFINED RKVC_LICENSE_LIBS)
        set(LIBSODIUM_PREFIX "${CMAKE_SOURCE_DIR}/.build/deps/libsodium-install"
            CACHE PATH "libsodium install prefix (see scripts/install-libsodium.sh)")
        if(NOT EXISTS "${LIBSODIUM_PREFIX}/lib/libsodium.a")
            message(FATAL_ERROR
                "libsodium not found at ${LIBSODIUM_PREFIX}/lib/libsodium.a\n"
                "  Run: git submodule update --init third_party/libsodium\n"
                "       ./scripts/install-libsodium.sh")
        endif()
        find_package(Threads REQUIRED)
        set(RKVC_LICENSE_LIBS ${LIBSODIUM_PREFIX}/lib/libsodium.a Threads::Threads)
        set(RKVC_LICENSE_INCLUDES ${LIBSODIUM_PREFIX}/include)
    endif()

    set(_trust_pub "${RKVC_TRUST_PUBKEY_HEX}")
    if(NOT _trust_pub AND NOT RKVC_TRUST_PRODUCTION AND
       EXISTS "${CMAKE_SOURCE_DIR}/scripts/trust/dev-root.pub")
        file(STRINGS "${CMAKE_SOURCE_DIR}/scripts/trust/dev-root.pub" _pub_lines)
        foreach(_ln IN LISTS _pub_lines)
            if(NOT _ln MATCHES "^#")
                set(_trust_pub "${_ln}")
            endif()
        endforeach()
    endif()
    if(NOT _trust_pub)
        if(RKVC_TRUST_PRODUCTION)
            message(FATAL_ERROR
                "RKVC_TRUST_PRODUCTION=ON requires -DRKVC_TRUST_PUBKEY_HEX=<64 hex>\n"
                "  (prod trust root 由发布流程离线生成，不入库)")
        endif()
        # dev 临时根：配置期生成（需要主机 python3 + libsodium 运行库）
        find_program(PYTHON3_EXECUTABLE python3 REQUIRED)
        set(_trust_dir "${CMAKE_BINARY_DIR}/trust")
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E env "PYTHONPATH=${CMAKE_SOURCE_DIR}/tools"
                ${PYTHON3_EXECUTABLE} -m rkvc_build.rkmodel keygen
                "${_trust_dir}/dev-root"
            RESULT_VARIABLE _keygen_rc
            OUTPUT_VARIABLE _keygen_out
            ERROR_QUIET)
        if(NOT _keygen_rc EQUAL 0)
            message(FATAL_ERROR
                "dev trust root 生成失败（需要主机 libsodium 运行库 libsodium23）")
        endif()
        file(STRINGS "${_trust_dir}/dev-root.pub" _pub_lines)
        foreach(_ln IN LISTS _pub_lines)
            if(NOT _ln MATCHES "^#")
                set(_trust_pub "${_ln}")
            endif()
        endforeach()
        message(STATUS "Model sign: ephemeral dev trust root at ${_trust_dir}")
    endif()
    string(LENGTH "${_trust_pub}" _pub_len)
    if(NOT _pub_len EQUAL 64 OR NOT _trust_pub MATCHES "^[0-9a-fA-F]+$")
        message(FATAL_ERROR "trust pubkey 必须是 64 字符 hex：${_trust_pub}")
    endif()
    string(TOLOWER "${_trust_pub}" RKVC_TRUST_PUBKEY_HEX_EFFECTIVE)
    set(RKVC_MODEL_SIGN_LIBS ${RKVC_LICENSE_LIBS})
    set(RKVC_MODEL_SIGN_INCLUDES ${RKVC_LICENSE_INCLUDES})
    set(RKVC_MODEL_SIGN_DEFS
        RKVC_ENABLE_MODEL_SIGN=1
        RKVC_TRUST_PUBKEY_HEX="${RKVC_TRUST_PUBKEY_HEX_EFFECTIVE}"
        RKVC_TRUST_PRODUCTION=$<BOOL:${RKVC_TRUST_PRODUCTION}>)
    message(STATUS "Model sign: ON (production=${RKVC_TRUST_PRODUCTION})")
endif()
