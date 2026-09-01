# SPDX-License-Identifier: AGPL-3.0-or-later
# Build options and instrumentation for the codebase.

math(EXPR RKVC_VERSION_NUM
    "${PROJECT_VERSION_MAJOR} * 65536 + ${PROJECT_VERSION_MINOR} * 256 + ${PROJECT_VERSION_PATCH}")
set(RKVC_VERSION_COMPILE_DEFS
    RKVC_VERSION_STR="${PROJECT_VERSION}"
    RKVC_VERSION_NUM=${RKVC_VERSION_NUM}
)

option(RKVC_BUILD_SHARED "Build librkvc shared library" ON)
option(RKVC_BUILD_STATIC "Build librkvc static library" ON)
option(RKVC_BUILD_CLI "Build the unified rkvc CLI" ON)
option(RKVC_BUILD_EXAMPLES "Build public API examples" ON)
option(RKVC_BUILD_TESTS "Build unit tests (CMocka)" OFF)
option(RKVC_BUILD_DOCS "Build Doxygen API docs" OFF)
option(RKVC_BUILD_BACKEND_MPP
    "Build the Rockchip MPP backend DSO (requires MPP_INSTALL_PREFIX)" OFF)
option(RKVC_BUILD_BACKEND_RGA
    "Build the Rockchip RGA backend DSO (requires RGA_INSTALL_PREFIX)" OFF)
option(RKVC_BUILD_BACKEND_RKNN
    "Build the Rockchip RKNN backend DSO (requires RKNN_INSTALL_PREFIX)" OFF)
option(RKVC_ENABLE_MODEL_SIGN
    "Enable .rkmodel Ed25519 signature verification" OFF)
option(RKVC_TRUST_PRODUCTION
    "Require an explicitly pinned production model trust root" OFF)
set(RKVC_TRUST_PUBKEY_HEX "" CACHE STRING
    "Ed25519 trust-root public key (64 hex chars)")

option(RKVC_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(RKVC_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(RKVC_ENABLE_COVERAGE "Enable gcov/llvm-cov instrumentation" OFF)
option(RKVC_ENABLE_FAULT_INJECTION
    "Enable deterministic fault injection hooks" OFF)
option(RKVC_ENABLE_RELEASE_HARDENING
    "Enable release hardening: LTO, full RELRO, PIE, strip, and symbol hiding" ON)

add_library(rkvc_instrumentation INTERFACE)

if(RKVC_ENABLE_ASAN OR RKVC_ENABLE_UBSAN OR RKVC_ENABLE_COVERAGE)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "Sanitizers and coverage require GCC or Clang")
    endif()
endif()

if(RKVC_ENABLE_ASAN)
    target_compile_options(rkvc_instrumentation INTERFACE
        -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(rkvc_instrumentation INTERFACE
        -fsanitize=address -fno-omit-frame-pointer)
endif()

if(RKVC_ENABLE_UBSAN)
    target_compile_options(rkvc_instrumentation INTERFACE
        -fsanitize=undefined -fno-omit-frame-pointer)
    target_link_options(rkvc_instrumentation INTERFACE
        -fsanitize=undefined -fno-omit-frame-pointer)
endif()

if(RKVC_ENABLE_COVERAGE)
    target_compile_options(rkvc_instrumentation INTERFACE -O0 -g --coverage)
    target_link_options(rkvc_instrumentation INTERFACE --coverage)
endif()

if(RKVC_ENABLE_FAULT_INJECTION)
    target_compile_definitions(rkvc_instrumentation INTERFACE
        RKVC_ENABLE_FAULT_INJECTION=1)
endif()

set(_RKVC_DO_HARDEN OFF)
if(RKVC_ENABLE_RELEASE_HARDENING AND
   CMAKE_BUILD_TYPE STREQUAL "Release" AND
   NOT RKVC_ENABLE_ASAN AND NOT RKVC_ENABLE_UBSAN AND
   NOT RKVC_ENABLE_COVERAGE AND NOT RKVC_ENABLE_FAULT_INJECTION)
    set(_RKVC_DO_HARDEN ON)
endif()

if(_RKVC_DO_HARDEN)
    message(STATUS "Release hardening: ON")
    add_compile_options(-O3 -fmerge-all-constants
        -fno-unwind-tables -fno-asynchronous-unwind-tables)
    add_link_options(-Wl,-O2 -Wl,-z,relro,-z,now -Wl,-z,noexecstack)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -pie -Wl,--strip-all")
    set(CMAKE_SHARED_LINKER_FLAGS
        "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--strip-all")
else()
    message(STATUS "Release hardening: OFF")
endif()
