cmake_minimum_required(VERSION 3.21)
project(rkvc
    VERSION 0.4.0
    DESCRIPTION "Rockchip multi-SoC media graph library"
    LANGUAGES C)

# Only context/request/job/frame and the backend ABI are built or installed.
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(RkvcOptions)
include(RkvcDependencies)
include(RkvcTargets)
include(RkvcInstall)

if(RKVC_BUILD_TESTS)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(CMOCKA REQUIRED cmocka)
    enable_testing()

    find_package(Git QUIET)
    if(Git_FOUND AND EXISTS
       "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg-rockchip/.git")
        add_test(NAME test_ffmpeg_patches
            COMMAND ${CMAKE_COMMAND}
                -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
                -DRKVC_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckFfmpegPatches.cmake)
        set_tests_properties(test_ffmpeg_patches PROPERTIES LABELS infrastructure)
    endif()

    set(RKVC_STANDALONE_SOURCES
        lib/graph.c
        lib/executor.c
        lib/frame.c
        lib/api.c
        lib/context.c
        lib/job.c
        lib/rkmodel.c
        lib/model_registry.c
        lib/backend_dso.c
        lib/builtin_backends.c
        lib/node_fileio.c
        lib/model_trust.c)

    function(rkvc_add_core_test name source)
        add_executable(${name} ${source} ${RKVC_STANDALONE_SOURCES})
        target_compile_definitions(${name} PRIVATE
            RKVC_STANDALONE_TEST=1 _DEFAULT_SOURCE
            ${RKVC_MODEL_SIGN_DEFS})
        target_include_directories(${name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/lib
            ${RKVC_SODIUM_INCLUDES}
            ${CMOCKA_INCLUDE_DIRS})
        target_link_libraries(${name} PRIVATE
            cmocka pthread ${CMAKE_DL_LIBS} ${RKVC_SODIUM_LIBS}
            rkvc_instrumentation)
        add_test(NAME ${name} COMMAND ${name})
        set_tests_properties(${name} PROPERTIES LABELS "c;unit")
    endfunction()

    rkvc_add_core_test(test_graph_executor tests/c/test_graph_executor.c)
    rkvc_add_core_test(test_job tests/c/test_job.c)
    rkvc_add_core_test(test_rkmodel tests/c/test_rkmodel.c)
    rkvc_add_core_test(test_media_pipeline tests/c/test_media_pipeline.c)
    rkvc_add_core_test(test_model_binding tests/c/test_model_binding.c)
    rkvc_add_core_test(test_frame_metadata tests/c/test_frame_metadata.c)
    rkvc_add_core_test(test_api_contract tests/c/test_api_contract.c)

    # RKNN 后端用受控 fake runtime 做 x86 端到端测试：覆盖模型绑定、
    # tensor 契约、推理调用、Phase-RLFN 后处理与帧所有权，不要求 NPU。
    add_executable(test_backend_rknn
        tests/c/test_backend_rknn.c
        tests/c/fake_rknn/fake_rknn.c
        backends/backend_rknn.c
        ${RKVC_STANDALONE_SOURCES})
    target_compile_definitions(test_backend_rknn PRIVATE
        RKVC_STANDALONE_TEST=1 RKVC_RKNN_TESTING=1 _DEFAULT_SOURCE
        ${RKVC_MODEL_SIGN_DEFS})
    target_include_directories(test_backend_rknn PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/c/fake_rknn
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/lib
        ${RKVC_SODIUM_INCLUDES}
        ${CMOCKA_INCLUDE_DIRS})
    target_link_libraries(test_backend_rknn PRIVATE
        cmocka pthread m ${CMAKE_DL_LIBS} ${RKVC_SODIUM_LIBS}
        rkvc_instrumentation)
    add_test(NAME test_backend_rknn COMMAND test_backend_rknn)
    set_tests_properties(test_backend_rknn PROPERTIES LABELS "c;unit;backend")

    # MLVC 后端：fake RKNN runtime + 最小 PMF 表，x86 端到端
    # encode→container 与 encode→decode 往返，不要求 NPU。
    add_executable(test_backend_mlvc
        tests/c/test_backend_mlvc.c
        tests/c/fake_rknn/fake_rknn.c
        backends/backend_mlvc.c
        backends/mlvc/rans.c
        backends/mlvc/mlvc_pixel.c
        backends/mlvc/pmf.c
        backends/mlvc/qppatch.c
        backends/mlvc/container.c
        ${RKVC_STANDALONE_SOURCES})
    target_compile_definitions(test_backend_mlvc PRIVATE
        RKVC_STANDALONE_TEST=1 RKVC_MLVC_TESTING=1 _DEFAULT_SOURCE
        ${RKVC_MODEL_SIGN_DEFS})
    target_include_directories(test_backend_mlvc PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/c/fake_rknn
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/backends
        ${RKVC_SODIUM_INCLUDES}
        ${CMOCKA_INCLUDE_DIRS})
    target_link_libraries(test_backend_mlvc PRIVATE
        cmocka pthread m ${CMAKE_DL_LIBS} ${RKVC_SODIUM_LIBS}
        rkvc_instrumentation)
    add_test(NAME test_backend_mlvc COMMAND test_backend_mlvc)
    set_tests_properties(test_backend_mlvc PROPERTIES LABELS "c;unit;backend")

    add_library(rkvc_fixture_backend_good MODULE tests/c/fixture_backend.c)
    add_library(rkvc_fixture_backend_badabi MODULE tests/c/fixture_backend_badabi.c)
    foreach(fixture rkvc_fixture_backend_good rkvc_fixture_backend_badabi)
        target_include_directories(${fixture} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/lib)
        target_compile_definitions(${fixture} PRIVATE
            RKVC_STANDALONE_TEST=1 _DEFAULT_SOURCE)
        set_target_properties(${fixture} PROPERTIES
            PREFIX ""
            LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/fixture_backends)
    endforeach()

    add_library(rkvc_fixture_media_backend MODULE
        tests/c/fixture_media_backend.c)
    target_include_directories(rkvc_fixture_media_backend PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    set_target_properties(rkvc_fixture_media_backend PROPERTIES
        PREFIX ""
        OUTPUT_NAME rkvc_fixture_media_backend
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/fixture_media_backends)

    set(RKVC_BACKEND_LOADER_SOURCES ${RKVC_STANDALONE_SOURCES})
    list(REMOVE_ITEM RKVC_BACKEND_LOADER_SOURCES
        lib/builtin_backends.c lib/node_fileio.c)
    add_executable(test_backend_loader
        tests/c/test_backend_loader.c ${RKVC_BACKEND_LOADER_SOURCES})
    target_compile_definitions(test_backend_loader PRIVATE
        RKVC_STANDALONE_TEST=1 _DEFAULT_SOURCE
        RKVC_TEST_BACKEND_DIR="${CMAKE_CURRENT_BINARY_DIR}/fixture_backends"
        ${RKVC_MODEL_SIGN_DEFS})
    target_include_directories(test_backend_loader PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/lib
        ${RKVC_SODIUM_INCLUDES}
        ${CMOCKA_INCLUDE_DIRS})
    target_link_libraries(test_backend_loader PRIVATE
        cmocka pthread ${CMAKE_DL_LIBS} ${RKVC_SODIUM_LIBS}
        rkvc_instrumentation)
    add_dependencies(test_backend_loader
        rkvc_fixture_backend_good rkvc_fixture_backend_badabi)
    add_test(NAME test_backend_loader COMMAND test_backend_loader)
    set_tests_properties(test_backend_loader PROPERTIES LABELS "c;unit")

    if(RKVC_ENABLE_MODEL_SIGN)
        add_executable(test_model_trust
            tests/c/test_model_trust.c lib/rkmodel.c lib/model_trust.c)
        target_compile_definitions(test_model_trust PRIVATE
            RKVC_STANDALONE_TEST=1 _DEFAULT_SOURCE _FILE_OFFSET_BITS=64
            ${RKVC_MODEL_SIGN_DEFS})
        target_include_directories(test_model_trust PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/lib
            ${RKVC_SODIUM_INCLUDES}
            ${CMOCKA_INCLUDE_DIRS})
        target_link_libraries(test_model_trust PRIVATE
            cmocka ${RKVC_SODIUM_LIBS})
        add_test(NAME test_model_trust COMMAND test_model_trust)
        set_tests_properties(test_model_trust PROPERTIES LABELS "c;unit")
    endif()

    set(RKVC_CHECK_TARGETS
        test_graph_executor test_job test_rkmodel
        test_media_pipeline test_model_binding
        test_frame_metadata test_api_contract
        test_backend_loader test_backend_rknn test_backend_mlvc)
    if(RKVC_BUILD_EXAMPLES)
        list(APPEND RKVC_CHECK_TARGETS
            example_decode_file example_encode_file example_transcode
            example_stream_ports example_roi_encode example_adaptive_bitrate
            example_live_capture example_net_loopback
            example_live_transcode_ports example_upscale_ctx)
    endif()
    if(TARGET test_model_trust)
        list(APPEND RKVC_CHECK_TARGETS test_model_trust)
    endif()
    if(TARGET rkvc_cli)
        add_dependencies(rkvc_cli rkvc_fixture_media_backend)
        add_test(NAME test_cli_version COMMAND rkvc_cli version --json)
        set_tests_properties(test_cli_version PROPERTIES LABELS unit)
        add_test(NAME test_cli_license COMMAND rkvc_cli license --json)
        set_tests_properties(test_cli_license PROPERTIES LABELS "unit;cli")
        add_test(NAME test_cli_bench_help COMMAND rkvc_cli bench --help)
        set_tests_properties(test_cli_bench_help PROPERTIES LABELS "unit;cli")
        add_test(NAME test_cli_bench
            COMMAND rkvc_cli bench encode
                -i ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/nv12_2x2.fixture
                -o ${CMAKE_CURRENT_BINARY_DIR}/bench-output.h264
                --width 2 --height 2 --codec h264
                --backend-dir ${CMAKE_CURRENT_BINARY_DIR}/fixture_media_backends
                --warmup 0 --iterations 2 --json)
        set_tests_properties(test_cli_bench PROPERTIES LABELS "unit;cli")
        list(APPEND RKVC_CHECK_TARGETS rkvc_cli)
    endif()

    add_custom_target(check
        COMMAND ${CMAKE_CTEST_COMMAND} -j1 --output-on-failure
        DEPENDS ${RKVC_CHECK_TARGETS}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Building and running rkvc tests"
        VERBATIM)
endif()

if(RKVC_BUILD_DOCS)
    find_package(Doxygen REQUIRED)
    set(DOXYGEN_IN ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in)
    set(DOXYGEN_OUT ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile)
    configure_file(${DOXYGEN_IN} ${DOXYGEN_OUT} @ONLY)
    add_custom_target(docs
        COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYGEN_OUT}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Generating API documentation"
        VERBATIM)
endif()
