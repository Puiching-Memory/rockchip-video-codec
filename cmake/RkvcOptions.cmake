# SPDX-License-Identifier: AGPL-3.0-or-later
# RkvcOptions.cmake — 所有构建选项、版本注入、检测/加固开关。
#
# 本模块只声明"要不要构建什么"，不定位依赖（RkvcDependencies）、
# 不创建最终产物（RkvcTargets）。

# ── 版本号（全项目唯一来源：project(VERSION)）──────────────────────
# 源码通过下面的编译定义消费版本号，任何 .c/.h 中不得再硬编码版本。
# RKVC_VERSION_NUM: major<<16 | minor<<8 | patch，与 rkvc_version_number() 对齐。
math(EXPR RKVC_VERSION_NUM
    "${PROJECT_VERSION_MAJOR} * 65536 + ${PROJECT_VERSION_MINOR} * 256 + ${PROJECT_VERSION_PATCH}")
set(RKVC_VERSION_COMPILE_DEFS
    RKVC_VERSION_STR="${PROJECT_VERSION}"
    RKVC_VERSION_NUM=${RKVC_VERSION_NUM}
)

# ── 选项 ──────────────────────────────────────────────────────────────
option(RKVC_BUILD_SHARED   "Build shared library"      ON)
option(RKVC_BUILD_STATIC   "Build static library"      ON)
option(RKVC_BUILD_EXAMPLES "Build example programs"    ON)
option(RKVC_BUILD_CLI      "Build CLI programs (cli/)" ON)
option(RKVC_BUILD_TESTS    "Build unit tests (CMocka)" OFF)
option(RKVC_ENABLE_ASAN    "Enable AddressSanitizer"   OFF)
option(RKVC_ENABLE_UBSAN   "Enable UndefinedBehaviorSanitizer" OFF)
option(RKVC_ENABLE_COVERAGE "Enable gcov/llvm-cov instrumentation" OFF)
option(RKVC_ENABLE_FAULT_INJECTION "Enable deterministic test fault injection hooks" OFF)
option(RKVC_BUILD_DOCS     "Build Doxygen API docs"    OFF)
option(RKVC_ENABLE_RKNN    "Enable RKNN NPU super-resolution" ON)
option(RKVC_ENABLE_MLVC    "Enable MLVC neural video codec (RKNN NPU + pure-C rANS)" ON)
option(RKVC_RKNN_ALLOW_DEBUG_LOG "Respect RKNN_LOG_LEVEL / RKVC_DEBUG_RKNN_LOG at runtime for internal diagnostics (leaks model topology to end users; keep OFF for distribution)" OFF)
option(RKVC_ENABLE_LICENSE "Build 1-machine-1-code license support and enforce it in rkvc_init() (libsodium Ed25519)" OFF)
option(RKVC_ENABLE_MODEL_CRYPT "Encrypt RKNN models with rkvc's own layer (XSalsa20-Poly1305 + per-machine model.key, libsodium)" OFF)
option(RKVC_BUILD_NEW_ENGINE
    "Build the 0.4 graph-kernel engine (context/request/job/frame/diagnostic) as the rkvc_core target alongside the legacy rkvc_shared. Default OFF so the existing pipeline keeps building unchanged."
    OFF)
option(RKVC_BUILD_BACKEND_MPP
    "Build the FFmpeg-rockchip MPP decoder backend DSO (requires libavcodec/libavutil/libdrm)"
    OFF)
option(RKVC_ENABLE_MODEL_SIGN
    "Enable .rkmodel Ed25519 signature verification (libsodium; dev/prod trust root)"
    OFF)
option(RKVC_TRUST_PRODUCTION
    "Pin production trust root (unsigned/development-signed models count as untrusted)"
    OFF)
set(RKVC_TRUST_PUBKEY_HEX "" CACHE STRING
    "Ed25519 trust-root public key (64 hex chars). Empty + non-production: use scripts/trust/dev-root.pub or generate an ephemeral dev root at configure time.")

# ── 检测/插桩 ────────────────────────────────────────────────────────
add_library(rkvc_instrumentation INTERFACE)

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
#   -O3                        高优化等级，让反编译后的控制流/函数边界更难读
#   - version script allowlist 仅导出公共头声明的 API
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
