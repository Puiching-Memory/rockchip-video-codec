cmake_minimum_required(VERSION 3.21)
project(rkvc
    VERSION 0.4.0
    DESCRIPTION "Rockchip multi-SoC multi-codec pipeline library"
    LANGUAGES C
)

# 0.4 起为不兼容版本：公共 ABI、CLI 与包布局均按 docs/0.4.0-refactor-plan.md
# 重新定义，不提供 0.3.x 兼容层。版本号唯一来源是 project(VERSION)。

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# 根文件只保留项目装配；选项/依赖/target/安装细节见 cmake/ 模块。
include(RkvcOptions)
include(RkvcDependencies)
include(RkvcTargets)
include(RkvcInstall)

# ── 单元测试 (CMocka) ────────────────────────────────────────────────
if(RKVC_BUILD_TESTS)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(CMOCKA REQUIRED cmocka)
    enable_testing()

    # 旧引擎测试依赖 rkvc_static；核心库独立构建（无旧库）时只跑新引擎回归。
    if(NOT TARGET rkvc_static)
        set(RKVC_LEGACY_TESTS OFF)
    else()
        set(RKVC_LEGACY_TESTS ON)
    endif()

    set(RKVC_TEST_SOURCES
        tests/test_types.c
        tests/test_router.c
        tests/test_buffer.c
        tests/test_contracts.c
        tests/test_internal.c
        tests/test_hardware.c
        tests/test_scale.c
        tests/test_post_upscale.c
        tests/test_sr_phase.c
        tests/test_roi_runtime.c
        tests/test_reconfig.c
        tests/test_v4l2.c
        tests/test_net.c
        tests/test_qppatch.c
        tests/test_cli_parse.c
    )

    if(RKVC_MLVC_ENABLED)
        # rANS 熵编解码与 MLVC 像素内核回归（逐位等价 + round-trip）
        list(APPEND RKVC_TEST_SOURCES
            tests/test_rans.c
            tests/test_mlvc_pixel.c
        )
        # Python 工具（tools/mlvc/pmf.py、qppatch.py）↔ C 消费方二进制契约
        # fixture 由 tests/gen_mlvc_fixtures.py 生成（见其 --check 模式）
        list(APPEND RKVC_TEST_SOURCES tests/test_mlvc_contract.c)
    endif()

    if(RKVC_ENABLE_MODEL_CRYPT)
        list(APPEND RKVC_TEST_SOURCES tests/test_model_crypt.c)
    endif()

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

    if(RKVC_LEGACY_TESTS)
    foreach(TEST_SRC ${RKVC_TEST_SOURCES})
        get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)
        if(TEST_NAME STREQUAL "test_cli_parse")
            add_executable(${TEST_NAME} ${TEST_SRC} cli/cli_parse.c)
        else()
            add_executable(${TEST_NAME} ${TEST_SRC})
        endif()
        target_link_libraries(${TEST_NAME} PRIVATE rkvc_static rkvc_instrumentation ${CMOCKA_LIBRARIES})
        target_include_directories(${TEST_NAME}
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/lib
                ${CMAKE_CURRENT_SOURCE_DIR}/cli
                ${CMOCKA_INCLUDE_DIRS}
                ${AVCODEC_INCLUDE_DIRS}
        )
        if(TEST_NAME STREQUAL "test_model_crypt")
            target_include_directories(${TEST_NAME} PRIVATE ${RKVC_SODIUM_INCLUDES})
        endif()
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
    endif()

    # 0.4 图内核回归（独立编译，分配器映射到 libc；不依赖 FFmpeg/MPP/RGA/RKNN）
    add_executable(test_graph_executor
        tests/test_graph_executor.c
        lib/graph.c lib/executor.c lib/frame.c lib/api.c lib/context.c lib/job.c
        lib/rkmodel.c lib/model_registry.c
        lib/backend_dso.c lib/builtin_backends.c lib/model_trust.c
    )
    target_compile_definitions(test_graph_executor PRIVATE
        RKVC_STANDALONE_TEST=1 _DEFAULT_SOURCE)
    target_include_directories(test_graph_executor
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/lib
            ${RKVC_SODIUM_INCLUDES}
            ${CMOCKA_INCLUDE_DIRS}
    )
    target_link_libraries(test_graph_executor PRIVATE
        cmocka pthread ${CMAKE_DL_LIBS} ${RKVC_SODIUM_LIBS})
    add_test(NAME test_graph_executor COMMAND test_graph_executor)
    set_tests_properties(test_graph_executor PROPERTIES LABELS unit)

    # 0.4 job 生命周期回归（同一组独立源 + job.c）
    add_executable(test_job
        tests/test_job.c
        lib/graph.c lib/executor.c lib/frame.c lib/api.c lib/context.c lib/job.c
        lib/rkmodel.c lib/model_registry.c
        lib/backend_dso.c lib/builtin_backends.c lib/model_trust.c
    )
    target_compile_definitions(test_job PRIVATE
        RKVC_STANDALONE_TEST=1 _DEFAULT_SOURCE)
    target_include_directories(test_job
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/lib
            ${RKVC_SODIUM_INCLUDES}
            ${CMOCKA_INCLUDE_DIRS}
    )
    target_link_libraries(test_job PRIVATE
        cmocka pthread ${CMAKE_DL_LIBS} ${RKVC_SODIUM_LIBS})
    add_test(NAME test_job COMMAND test_job)
    set_tests_properties(test_job PROPERTIES LABELS unit)

    # 0.4 模型容器/注册表回归（含写入侧构造 + 扫描过滤）
    add_executable(test_rkmodel
        tests/test_rkmodel.c
        lib/rkmodel.c lib/model_registry.c
        lib/backend_dso.c lib/builtin_backends.c lib/model_trust.c
        lib/context.c lib/api.c lib/frame.c
    )
    target_compile_definitions(test_rkmodel PRIVATE
        RKVC_STANDALONE_TEST=1 _DEFAULT_SOURCE _FILE_OFFSET_BITS=64)
    target_include_directories(test_rkmodel
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/lib
            ${RKVC_SODIUM_INCLUDES}
            ${CMOCKA_INCLUDE_DIRS}
    )
    target_link_libraries(test_rkmodel PRIVATE
        cmocka pthread ${CMAKE_DL_LIBS} ${RKVC_SODIUM_LIBS})
    add_test(NAME test_rkmodel COMMAND test_rkmodel)
    set_tests_properties(test_rkmodel PROPERTIES LABELS unit)

    # 0.4 后端 DSO 加载器回归（含夹具 DSO：合法 + 坏 ABI）
    add_library(rkvc_fixture_backend_good MODULE tests/fixture_backend.c)
    add_library(rkvc_fixture_backend_badabi MODULE tests/fixture_backend_badabi.c)
    foreach(fx rkvc_fixture_backend_good rkvc_fixture_backend_badabi)
        target_include_directories(${fx} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/lib)
        target_compile_definitions(${fx} PRIVATE
            RKVC_STANDALONE_TEST=1 _DEFAULT_SOURCE)
        set_target_properties(${fx} PROPERTIES
            PREFIX "" LIBRARY_OUTPUT_DIRECTORY
            ${CMAKE_CURRENT_BINARY_DIR}/fixture_backends)
    endforeach()

    add_executable(test_backend_loader
        tests/test_backend_loader.c
        lib/context.c lib/api.c lib/frame.c
        lib/rkmodel.c lib/model_registry.c lib/backend_dso.c
        lib/model_trust.c
        # 不链接 lib/builtin_backends.c：测试 TU 提供强定义
    )
    target_compile_definitions(test_backend_loader PRIVATE
        RKVC_STANDALONE_TEST=1 _DEFAULT_SOURCE
        RKVC_TEST_BACKEND_DIR="${CMAKE_CURRENT_BINARY_DIR}/fixture_backends")
    target_include_directories(test_backend_loader
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/lib
            ${RKVC_SODIUM_INCLUDES}
            ${CMOCKA_INCLUDE_DIRS}
    )
    target_link_libraries(test_backend_loader PRIVATE
        cmocka pthread ${CMAKE_DL_LIBS} ${RKVC_SODIUM_LIBS})
    add_dependencies(test_backend_loader
        rkvc_fixture_backend_good rkvc_fixture_backend_badabi)
    add_test(NAME test_backend_loader COMMAND test_backend_loader)
    set_tests_properties(test_backend_loader PROPERTIES LABELS unit)

    # 0.4 模型签名信任链回归（需要 libsodium；dev/prod 由编译定义决定）
    if(RKVC_ENABLE_MODEL_SIGN)
    add_executable(test_model_trust
        tests/test_model_trust.c
        lib/rkmodel.c lib/model_trust.c
    )
    target_compile_definitions(test_model_trust PRIVATE
        RKVC_STANDALONE_TEST=1 _DEFAULT_SOURCE _FILE_OFFSET_BITS=64
        ${RKVC_MODEL_SIGN_DEFS})
    target_include_directories(test_model_trust
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/lib
            ${RKVC_MODEL_SIGN_INCLUDES}
            ${CMOCKA_INCLUDE_DIRS}
    )
    target_link_libraries(test_model_trust PRIVATE
        cmocka ${RKVC_MODEL_SIGN_LIBS})
    add_test(NAME test_model_trust COMMAND test_model_trust)
    set_tests_properties(test_model_trust PROPERTIES LABELS unit)
    endif()

    if(RKVC_LEGACY_TESTS)
    set(RKVC_HW_TEST_CASES
        test_session_transcode_h264
        test_session_transcode_balanced
        test_session_transcode_quality
        test_session_transcode_offline
        test_session_decode_nv12
        test_session_encode_h264
        test_session_live_transcode_ports
        test_session_live_transcode_ports_hevc
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

    set(RKVC_CHECK_DEPENDS ${RKVC_TEST_TARGETS} test_graph_executor test_job)
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

    add_test(NAME test_mlvc_export
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mlvc_export.sh
    )
    set_tests_properties(test_mlvc_export PROPERTIES
        LABELS unit
        ENVIRONMENT "${RKVC_TEST_ENVIRONMENT}"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    DISABLED $<NOT:$<BOOL:${RKVC_LEGACY_TESTS}>>
    )

    add_test(NAME test_sr_export
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sr_export.sh
    )
    set_tests_properties(test_sr_export PROPERTIES
        LABELS unit
        ENVIRONMENT "${RKVC_TEST_ENVIRONMENT}"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    DISABLED $<NOT:$<BOOL:${RKVC_LEGACY_TESTS}>>
    )
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
