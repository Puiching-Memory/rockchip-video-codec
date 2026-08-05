cmake_minimum_required(VERSION 3.16)
project(rkvc
    VERSION 0.2.8
    DESCRIPTION "RK3588 multi-codec pipeline library"
    LANGUAGES C
)

# ── 版本号（全项目唯一来源：project(VERSION)）──────────────────────
# 源码通过下面的编译定义消费版本号，任何 .c/.h 中不得再硬编码版本。
# RKVC_VERSION_NUM: major<<16 | minor<<8 | patch，与 rkvc_version_number() 对齐。
math(EXPR RKVC_VERSION_NUM
    "${PROJECT_VERSION_MAJOR} * 65536 + ${PROJECT_VERSION_MINOR} * 256 + ${PROJECT_VERSION_PATCH}")
set(RKVC_VERSION_COMPILE_DEFS
    RKVC_VERSION_STR="${PROJECT_VERSION}"
    RKVC_VERSION_NUM=${RKVC_VERSION_NUM}
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── 选项 ──────────────────────────────────────────────────────────────
option(RKVC_BUILD_SHARED   "Build shared library"      ON)
option(RKVC_BUILD_STATIC   "Build static library"      ON)
option(RKVC_BUILD_EXAMPLES "Build example programs"    ON)
option(RKVC_BUILD_GUI_EXAMPLES "Build optional SDL2 GUI examples" ON)
option(RKVC_BUILD_CLI      "Build CLI programs (cli/)" ON)
option(RKVC_BUILD_TESTS    "Build unit tests (CMocka)" OFF)
option(RKVC_ENABLE_ASAN    "Enable AddressSanitizer"   OFF)
option(RKVC_ENABLE_UBSAN   "Enable UndefinedBehaviorSanitizer" OFF)
option(RKVC_ENABLE_COVERAGE "Enable gcov/llvm-cov instrumentation" OFF)
option(RKVC_ENABLE_FAULT_INJECTION "Enable deterministic test fault injection hooks" OFF)
option(RKVC_BUILD_DOCS     "Build Doxygen API docs"    OFF)
option(RKVC_ENABLE_RKNN    "Enable RKNN NPU super-resolution" ON)
option(RKVC_ENABLE_LICENSE "Build 1-machine-1-code license support and enforce it in rkvc_init() (libsodium Ed25519)" OFF)

# ── FFmpeg (ffmpeg-rockchip) ──────────────────────────────────────────
# 始终使用 third_party/ 源码树构建的静态库
set(AVCODEC_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/third_party/ffmpeg-rockchip")
set(AVCODEC_LIB_DIRS
    "${CMAKE_SOURCE_DIR}/third_party/ffmpeg-rockchip/libavcodec"
    "${CMAKE_SOURCE_DIR}/third_party/ffmpeg-rockchip/libavformat"
    "${CMAKE_SOURCE_DIR}/third_party/ffmpeg-rockchip/libavutil"
    "${CMAKE_SOURCE_DIR}/third_party/ffmpeg-rockchip/libswscale"
)

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

set(SVT_PREFIX "${CMAKE_SOURCE_DIR}/.build/deps/svt-av1-install" CACHE PATH "SVT-AV1 install prefix")

# librga (submodule 预编译库，经 scripts/install-librga.sh 安装)
set(RGA_PREFIX "${CMAKE_SOURCE_DIR}/.build/deps/librga-install" CACHE PATH "librga install prefix")
set(RGA_INCLUDE_DIR "${RGA_PREFIX}/include")
if(NOT EXISTS "${RGA_PREFIX}/lib/librga.so")
    message(FATAL_ERROR
        "librga not found at ${RGA_PREFIX}/lib/librga.so\n"
        "  Run: git submodule update --init --depth 1 third_party/librga\n"
        "       ./scripts/install-librga.sh")
endif()

set(RKVC_DEP_LIB_DIRS
    ${AVCODEC_LIB_DIRS}
    ${MPP_LIB_DIR}
    ${SVT_PREFIX}/lib
    ${RGA_PREFIX}/lib
)

set(RKVC_RPATH_LINK_OPTIONS "")
foreach(RKVC_DEP_LIB_DIR IN LISTS RKVC_DEP_LIB_DIRS)
    list(APPEND RKVC_RPATH_LINK_OPTIONS "-Wl,-rpath-link,${RKVC_DEP_LIB_DIR}")
endforeach()

# ── 源文件 ─────────────────────────────────────────────────────────────
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
set(RKVC_SOURCES
    lib/init.c
    lib/ffmpeg_util.c
    lib/utils.c
    lib/buffer_pool.c
    lib/router.c
    lib/pipeline.c
    lib/port.c
    lib/scheduler.c
    lib/session.c
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
    lib/rkvc_sr_neon.c
    lib/node_rkvc_sr.c
)

if(RKVC_ENABLE_RKNN)
    find_library(RKNNRT_LIB rknnrt
        PATHS /usr/lib/aarch64-linux-gnu /usr/lib /lib)
    find_path(RKNN_INCLUDE_DIR rknn_api.h
        PATHS /usr/include /usr/local/include)
    if(RKNNRT_LIB AND RKNN_INCLUDE_DIR)
        message(STATUS "RKNN runtime: ${RKNNRT_LIB}")
        set(RKVC_RKNN_LIBS ${RKNNRT_LIB})
        set(RKVC_RKNN_INCLUDES ${RKNN_INCLUDE_DIR})
        set(RKVC_RKNN_ENABLED 1)
    else()
        message(WARNING "RKVC_ENABLE_RKNN=ON but librknnrt not found; building without NPU SR")
    endif()
endif()

add_library(rkvc_instrumentation INTERFACE)

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

if(RKVC_ENABLE_ASAN OR RKVC_ENABLE_UBSAN OR RKVC_ENABLE_COVERAGE)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "Sanitizers and coverage require GCC or Clang")
    endif()
endif()

if(RKVC_ENABLE_ASAN)
    target_compile_options(rkvc_instrumentation INTERFACE
        -fsanitize=address
        -fno-omit-frame-pointer
    )
    target_link_options(rkvc_instrumentation INTERFACE
        -fsanitize=address
        -fno-omit-frame-pointer
    )
endif()

if(RKVC_ENABLE_UBSAN)
    target_compile_options(rkvc_instrumentation INTERFACE
        -fsanitize=undefined
        -fno-omit-frame-pointer
    )
    target_link_options(rkvc_instrumentation INTERFACE
        -fsanitize=undefined
        -fno-omit-frame-pointer
    )
endif()

if(RKVC_ENABLE_COVERAGE)
    target_compile_options(rkvc_instrumentation INTERFACE
        -O0
        -g
        --coverage
    )
    target_link_options(rkvc_instrumentation INTERFACE
        --coverage
    )
endif()

if(RKVC_ENABLE_FAULT_INJECTION)
    target_compile_definitions(rkvc_instrumentation INTERFACE
        RKVC_ENABLE_FAULT_INJECTION=1
    )
endif()

# ── Release 加固 / 反编译增强 ──────────────────────────────────────
# 原则：不改动源代码，仅在 Release 构建且未开启调试/测试/检测工具时启用。
# 因此不影响日常二次开发（Debug / tests / asan / coverage 构建保持可调试）。
#
# 启用项：
#   -O3 -flto                  链接时优化，让反编译后的控制流/函数边界更难读
#   -fvisibility=hidden        默认隐藏所有符号（配合 librkvc.map 精确导出 API）
#   -fmerge-all-constants      合并重复常量，增加字符串定位难度
#   -fno-unwind-tables          移除异常展开表，减少元数据
#   -fno-asynchronous-unwind-tables  同上
#   -Wl,-z,relro,-z,now        FULL RELRO，防止 GOT 覆写
#   -Wl,-z,noexecstack          不可执行栈
#   -pie / -Wl,--strip-all     可执行文件位置无关 + 剥离符号
option(RKVC_ENABLE_RELEASE_HARDENING
    "Enable release hardening: LTO, full RELRO, PIE, strip, and symbol hiding"
    ON)

set(_RKVC_DO_HARDEN OFF)
if(RKVC_ENABLE_RELEASE_HARDENING AND
   CMAKE_BUILD_TYPE STREQUAL "Release" AND
   NOT RKVC_ENABLE_ASAN AND
   NOT RKVC_ENABLE_UBSAN AND
   NOT RKVC_ENABLE_COVERAGE AND
   NOT RKVC_ENABLE_FAULT_INJECTION)
    set(_RKVC_DO_HARDEN ON)
endif()

if(_RKVC_DO_HARDEN)
    message(STATUS "Release hardening: ON")
    add_compile_options(
        -O3
        -fmerge-all-constants
        -fno-unwind-tables
        -fno-asynchronous-unwind-tables
    )
    add_link_options(
        -Wl,-O2
        -Wl,-z,relro,-z,now
        -Wl,-z,noexecstack
    )
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(CMAKE_EXE_LINKER_FLAGS
        "${CMAKE_EXE_LINKER_FLAGS} -pie -Wl,--strip-all")
    set(CMAKE_SHARED_LINKER_FLAGS
        "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--strip-all")
else()
    message(STATUS "Release hardening: OFF")
endif()

# ── 库目标 ─────────────────────────────────────────────────────────────
if(RKVC_BUILD_SHARED)
    add_library(rkvc_shared SHARED ${RKVC_SOURCES})
    target_include_directories(rkvc_shared
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${AVCODEC_INCLUDE_DIRS}
            ${MPP_INCLUDE_DIR}
            ${RGA_INCLUDE_DIR}
            ${RKVC_RKNN_INCLUDES}
            ${CMAKE_CURRENT_SOURCE_DIR}/lib
    )
    target_link_directories(rkvc_shared PRIVATE ${RKVC_DEP_LIB_DIRS})
    target_link_options(rkvc_shared PRIVATE
        ${RKVC_RPATH_LINK_OPTIONS}
        # 仅导出公共 API(rkvc_ 前缀),内部符号全部隐藏;并让库内对自身
        # 符号的调用绑定到本 DSO 定义(防止 LD_PRELOAD 劫持校验链)。
        "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/librkvc.map"
        "-Wl,-Bsymbolic-functions")
    target_link_libraries(rkvc_shared PRIVATE rkvc_instrumentation ${FFMPEG_LIBS} ${EXTRA_LIBS} ${RKVC_RKNN_LIBS} ${RKVC_LICENSE_LIBS})
    if(RKVC_RKNN_ENABLED)
        target_compile_definitions(rkvc_shared PRIVATE RKVC_ENABLE_RKNN=1)
    endif()
    set_target_properties(rkvc_shared PROPERTIES
        OUTPUT_NAME rkvc
        VERSION     ${PROJECT_VERSION}
        SOVERSION   0
        POSITION_INDEPENDENT_CODE ON
    )
    # 版本号由 project(VERSION) 注入，init.c 不再硬编码
    target_compile_definitions(rkvc_shared PRIVATE ${RKVC_VERSION_COMPILE_DEFS} ${RKVC_LICENSE_DEFS})
    target_include_directories(rkvc_shared PRIVATE ${RKVC_LICENSE_INCLUDES})
    # soname alias
    add_library(rkvc::shared ALIAS rkvc_shared)
endif()

if(RKVC_BUILD_STATIC)
    add_library(rkvc_static STATIC ${RKVC_SOURCES})
    target_include_directories(rkvc_static
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${AVCODEC_INCLUDE_DIRS}
            ${MPP_INCLUDE_DIR}
            ${RGA_INCLUDE_DIR}
            ${RKVC_RKNN_INCLUDES}
            ${CMAKE_CURRENT_SOURCE_DIR}/lib
    )
    target_link_directories(rkvc_static PUBLIC ${RKVC_DEP_LIB_DIRS})
    target_link_options(rkvc_static INTERFACE ${RKVC_RPATH_LINK_OPTIONS})
    target_link_libraries(rkvc_static PUBLIC rkvc_instrumentation ${FFMPEG_LIBS} ${EXTRA_LIBS} ${RKVC_RKNN_LIBS} ${RKVC_LICENSE_LIBS})
    if(RKVC_RKNN_ENABLED)
        target_compile_definitions(rkvc_static PRIVATE RKVC_ENABLE_RKNN=1)
    endif()
    set_target_properties(rkvc_static PROPERTIES OUTPUT_NAME rkvc)
    # 版本号由 project(VERSION) 注入，init.c 不再硬编码
    target_compile_definitions(rkvc_static PRIVATE ${RKVC_VERSION_COMPILE_DEFS} ${RKVC_LICENSE_DEFS})
    target_include_directories(rkvc_static PRIVATE ${RKVC_LICENSE_INCLUDES})
    add_library(rkvc::static ALIAS rkvc_static)
endif()

# 默认别名
if(RKVC_BUILD_SHARED)
    add_library(rkvc ALIAS rkvc_shared)
elseif(RKVC_BUILD_STATIC)
    add_library(rkvc ALIAS rkvc_static)
endif()

# ── 示例程序 ──────────────────────────────────────────────────────────
if(RKVC_BUILD_EXAMPLES)
    set(EXAMPLE_TARGETS "")

    foreach(EXAMPLE encode_file decode_file decode_formats stream_device_pair transcode latency_test live_capture net_loopback adaptive_bitrate adaptive_bitrate_file)
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

    if(TARGET example_stream_device_pair)
        target_link_libraries(example_stream_device_pair PRIVATE pthread)
    endif()
    if(TARGET example_net_loopback)
        target_link_libraries(example_net_loopback PRIVATE pthread)
    endif()
    if(TARGET example_adaptive_bitrate)
        target_link_libraries(example_adaptive_bitrate PRIVATE pthread)
    endif()
    if(TARGET example_adaptive_bitrate_file)
        target_link_libraries(example_adaptive_bitrate_file PRIVATE pthread)
    endif()

    if(RKVC_BUILD_GUI_EXAMPLES)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(SDL2 QUIET sdl2)
        endif()

        if(SDL2_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/visual_compare.c")
            add_executable(example_visual_compare examples/visual_compare.c)
            target_link_libraries(example_visual_compare PRIVATE rkvc m ${SDL2_LIBRARIES})
            target_include_directories(example_visual_compare
                PRIVATE
                    ${AVCODEC_INCLUDE_DIRS}
                    ${SDL2_INCLUDE_DIRS}
            )
            target_link_directories(example_visual_compare
                PRIVATE
                    ${RKVC_DEP_LIB_DIRS}
                    ${SDL2_LIBRARY_DIRS}
            )
            target_compile_options(example_visual_compare PRIVATE ${SDL2_CFLAGS_OTHER})
            target_link_options(example_visual_compare PRIVATE ${RKVC_RPATH_LINK_OPTIONS})
            set_target_properties(example_visual_compare PROPERTIES
                BUILD_RPATH "${CMAKE_CURRENT_BINARY_DIR};${RKVC_DEP_LIB_DIRS}"
            )
            list(APPEND EXAMPLE_TARGETS example_visual_compare)
        else()
            message(STATUS "SDL2 not found, skipping example_visual_compare")
        endif()
    endif()
endif()

# ── CLI (cli/ → 安装到 bin/) ──────────────────────────────────────────
if(RKVC_BUILD_CLI)
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
            add_executable(${CLI_NAME} ${CLI_SRC})
            target_link_libraries(${CLI_NAME} PRIVATE rkvc)
            target_include_directories(${CLI_NAME}
                PRIVATE ${AVCODEC_INCLUDE_DIRS})
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
        COMMAND ${CMAKE_SOURCE_DIR}/bench/run_rd_benchmark.sh
        DEPENDS ${RKVC_CLI_TARGETS}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Run RD benchmark (requires SRC_VIDEO or pass video path to script)"
        VERBATIM
    )
endif()

# ── 授权工具（rkvc_lic：签发/管理，独立于 librkvc） ─────────────────
# rkvc_lic_client 为分发到客户机的裁剪版：仅 machine-id/verify，
# 不含 genkey/issue/inspect，避免把私钥签发能力随包分发给终端客户。
if(RKVC_ENABLE_LICENSE)
    add_executable(rkvc_lic tools/rkvc_lic.c lib/license_machine.c)
    target_include_directories(rkvc_lic PRIVATE
            ${RKVC_LICENSE_INCLUDES} ${CMAKE_CURRENT_SOURCE_DIR}/lib)
    target_link_libraries(rkvc_lic PRIVATE ${RKVC_LICENSE_LIBS})

    add_executable(rkvc_lic_client tools/rkvc_lic.c lib/license_machine.c)
    target_compile_definitions(rkvc_lic_client PRIVATE RKVC_LIC_MACHINE_ONLY=1)
    target_include_directories(rkvc_lic_client PRIVATE
            ${RKVC_LICENSE_INCLUDES} ${CMAKE_CURRENT_SOURCE_DIR}/lib)
    target_link_libraries(rkvc_lic_client PRIVATE ${RKVC_LICENSE_LIBS})
endif()

# ── 安装 ──────────────────────────────────────────────────────────────
include(GNUInstallDirs)

install(DIRECTORY include/rkvc
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

if(RKVC_BUILD_SHARED)
    install(TARGETS rkvc_shared
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif()

if(RKVC_BUILD_STATIC)
    install(TARGETS rkvc_static
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif()

# 安装 CLI 到 bin/
if(RKVC_CLI_TARGETS)
    install(TARGETS ${RKVC_CLI_TARGETS}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

# pkg-config 文件
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/rkvc.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/rkvc.pc
    @ONLY
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/rkvc.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)

# 许可证文本
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/rkvc
)

# CMake config
include(CMakePackageConfigHelpers)
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/rkvc-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/rkvc-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rkvc
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/rkvc-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/rkvc-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/rkvc-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rkvc
)

# ── 单元测试 (CMocka) ────────────────────────────────────────────────
if(RKVC_BUILD_TESTS)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(CMOCKA REQUIRED cmocka)
    enable_testing()

    set(RKVC_TEST_SOURCES
        tests/test_types.c
        tests/test_router.c
        tests/test_buffer.c
        tests/test_contracts.c
        tests/test_internal.c
        tests/test_hardware.c
        tests/test_scale.c
        tests/test_post_upscale.c
        tests/test_roi_runtime.c
        tests/test_reconfig.c
        tests/test_v4l2.c
        tests/test_net.c
    )

    if(RKVC_ENABLE_FAULT_INJECTION)
        list(APPEND RKVC_TEST_SOURCES tests/test_permissions.c)
        list(APPEND RKVC_TEST_SOURCES tests/test_fault_injection.c)
    endif()

    set(RKVC_TEST_TARGETS "")
    string(REPLACE ";" ":" RKVC_TEST_LIBRARY_PATH "${RKVC_DEP_LIB_DIRS}")
    set(RKVC_TEST_ENVIRONMENT
        "LD_LIBRARY_PATH=${RKVC_TEST_LIBRARY_PATH}:$ENV{LD_LIBRARY_PATH}"
        "RKVC_SOURCE_ROOT=${CMAKE_SOURCE_DIR}"
    )
    if(RKVC_ENABLE_ASAN OR RKVC_ENABLE_UBSAN)
        list(APPEND RKVC_TEST_ENVIRONMENT
            "ASAN_OPTIONS=detect_leaks=0:halt_on_error=1:abort_on_error=1"
            "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1"
        )
    endif()

    foreach(TEST_SRC ${RKVC_TEST_SOURCES})
        get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)
        add_executable(${TEST_NAME} ${TEST_SRC})
        target_link_libraries(${TEST_NAME} PRIVATE rkvc_static rkvc_instrumentation ${CMOCKA_LIBRARIES})
        target_include_directories(${TEST_NAME}
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/lib
                ${CMOCKA_INCLUDE_DIRS}
                ${AVCODEC_INCLUDE_DIRS}
        )
        target_link_directories(${TEST_NAME}
            PRIVATE
                ${RKVC_DEP_LIB_DIRS}
                ${CMOCKA_LIBRARY_DIRS}
        )
        target_link_options(${TEST_NAME}
            PRIVATE ${RKVC_RPATH_LINK_OPTIONS}
        )
        if(TEST_NAME STREQUAL "test_hardware")
            target_sources(${TEST_NAME} PRIVATE tests/test_fixtures.c)
            # 拆成独立 CTest 用例，避免单次进程内连续压榨 MPP/RGA 驱动。
        else()
            add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
            set_tests_properties(${TEST_NAME} PROPERTIES
                LABELS unit
                ENVIRONMENT "${RKVC_TEST_ENVIRONMENT}"
            )
            if(TEST_NAME STREQUAL "test_scale")
                set_tests_properties(${TEST_NAME} PROPERTIES
                    LABELS "hardware;integration"
                    RUN_SERIAL TRUE
                    SKIP_RETURN_CODE 77
                    ENVIRONMENT "${RKVC_TEST_ENVIRONMENT}"
                )
            endif()
        endif()
        list(APPEND RKVC_TEST_TARGETS ${TEST_NAME})
    endforeach()

    set(RKVC_HW_TEST_CASES
        test_session_transcode_h264
        test_session_transcode_balanced
        test_session_transcode_quality
        test_session_transcode_offline
        test_session_decode_nv12
        test_session_encode_h264
        test_session_av1_storage
        test_session_encode_decode_upscale_3x
        test_session_encode_decode_upscale_3x_ai_sr
    )
    foreach(hw_case ${RKVC_HW_TEST_CASES})
        add_test(NAME ${hw_case} COMMAND test_hardware ${hw_case})
        set_tests_properties(${hw_case} PROPERTIES
            LABELS "hardware;integration"
            RUN_SERIAL TRUE
            SKIP_RETURN_CODE 77
            ENVIRONMENT "${RKVC_TEST_ENVIRONMENT}"
        )
    endforeach()

    set(RKVC_CHECK_DEPENDS ${RKVC_TEST_TARGETS})
    if(RKVC_BUILD_CLI)
        list(APPEND RKVC_CHECK_DEPENDS ${RKVC_CLI_TARGETS})
    endif()

    add_custom_target(check
        COMMAND ${CMAKE_CTEST_COMMAND} -j1 --output-on-failure
        DEPENDS ${RKVC_CHECK_DEPENDS}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Building and running rkvc unit tests"
        VERBATIM
    )

    if(RKVC_BUILD_CLI)
        add_test(NAME test_cli_args
            COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli_args.sh
                    ${CMAKE_CURRENT_BINARY_DIR}
        )
        set_tests_properties(test_cli_args PROPERTIES
            LABELS unit
            ENVIRONMENT "${RKVC_TEST_ENVIRONMENT}"
        )

        add_test(NAME test_bench_permission_failure
            COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_bench_permission_failure.sh
                    ${CMAKE_CURRENT_BINARY_DIR}
        )
        set_tests_properties(test_bench_permission_failure PROPERTIES
            LABELS unit
            SKIP_RETURN_CODE 77
            ENVIRONMENT "${RKVC_TEST_ENVIRONMENT}"
        )
    endif()
endif()

# ── Doxygen API 文档 ─────────────────────────────────────────────────
if(RKVC_BUILD_DOCS)
    find_package(Doxygen REQUIRED)

    set(DOXYGEN_IN  ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in)
    set(DOXYGEN_OUT ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile)

    configure_file(${DOXYGEN_IN} ${DOXYGEN_OUT} @ONLY)

    add_custom_target(docs
        COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYGEN_OUT}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Generating API documentation with Doxygen"
        VERBATIM
    )
endif()

# ── CPack 打包 ───────────────────────────────────────────────────────
set(CPACK_PACKAGE_NAME "rkvc")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "RK3588 multi-codec pipeline library (rkvc v2)")
set(CPACK_PACKAGE_CONTACT "rkvc@example.com")
set(CPACK_PACKAGE_VENDOR "rkvc")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")

# DEB 包配置
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libcmocka-dev")
set(CPACK_DEBIAN_PACKAGE_SECTION "libs")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "arm64")
set(CPACK_DEBIAN_FILE_NAME "rkvc_${PROJECT_VERSION}_arm64.deb")

# TGZ 配置
set(CPACK_GENERATOR "TGZ;DEB")

# 输出目录
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/packages")

include(CPack)
