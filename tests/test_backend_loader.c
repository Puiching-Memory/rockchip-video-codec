/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file test_backend_loader.c
 * @brief 后端 DSO 加载器与内建后端注册回归（独立编译，cmocka）。
 *
 * 覆盖：内建后端注册、可信目录 DSO 加载、ABI 不匹配淘汰、非 DSO/缺失目录
 * 容忍、淘汰计数与诊断。
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include "context_internal.h"

/* 内建后端：本 TU 提供强定义（替代 lib/builtin_backends.c 的空表）。 */
static int selftest_probe(const rkvc_device_caps *caps, void *probe_ctx,
                          rkvc_diag **diag) {
    (void)caps; (void)probe_ctx; (void)diag;
    return 0;
}

static const rkvc_node_factory *selftest_factories(void *probe_ctx,
                                                   size_t *count) {
    (void)probe_ctx;
    *count = 0;
    return NULL;
}

static rkvc_backend g_selftest = {
    .abi_version = RKVC_ABI_VERSION,
    .id = "selftest-builtin",
    .probe = selftest_probe,
    .factories = selftest_factories,
};

void rkvc_backend_register_builtins(rkvc_context *ctx) {
    assert_int_equal(rkvc_registry_add_backend(ctx, &g_selftest),
                     RKVC_STATUS_OK);
}

static int has_backend(const rkvc_context *ctx, const char *id) {
    size_t i;
    for (i = 0; i < ctx->backend_count; ++i)
        if (strcmp(ctx->backends[i]->id, id) == 0)
            return 1;
    return 0;
}

static void test_builtin_registered(void **state) {
    rkvc_context *ctx = NULL;
    (void)state;
    assert_int_equal(rkvc_context_create(NULL, &ctx), RKVC_STATUS_OK);
    assert_true(has_backend(ctx, "selftest-builtin"));
    rkvc_context_destroy(ctx);
}

static void test_dso_loaded_and_badabi_rejected(void **state) {
    const char *dirs[1] = { RKVC_TEST_BACKEND_DIR };
    rkvc_context_options opts;
    rkvc_context *ctx = NULL;
    (void)state;

    rkvc_context_options_init(&opts, sizeof(opts));
    opts.paths.backend_dirs = dirs;
    opts.paths.backend_dir_count = 1;
    assert_int_equal(rkvc_context_create(&opts, &ctx), RKVC_STATUS_OK);

    /* 好夹具装载；坏 ABI 夹具被淘汰；淘汰有诊断 */
    assert_true(has_backend(ctx, "fixture-good"));
    assert_true(ctx->caps.has_rga);
    assert_false(has_backend(ctx, "fixture-badabi"));
    assert_int_equal(ctx->backend_skipped, 1);
    assert_non_null(strstr(ctx->backend_diag, "fixture_backend_badabi"));
    /* 内建仍在（且先于 DSO） */
    assert_true(has_backend(ctx, "selftest-builtin"));
    assert_string_equal(ctx->backends[0]->id, "selftest-builtin");
    rkvc_context_destroy(ctx);
}

static void test_missing_dir_tolerated(void **state) {
    const char *dirs[1] = { "/nonexistent/rkvc/backends" };
    rkvc_context_options opts;
    rkvc_context *ctx = NULL;
    (void)state;

    rkvc_context_options_init(&opts, sizeof(opts));
    opts.paths.backend_dirs = dirs;
    opts.paths.backend_dir_count = 1;
    assert_int_equal(rkvc_context_create(&opts, &ctx), RKVC_STATUS_OK);
    assert_int_equal(ctx->backend_skipped, 0);
    rkvc_context_destroy(ctx);
}

static const rkvc_node_factory g_malformed_factory = {
    .id = "malformed",
    .backend_id = NULL,
    .stage = RKVC_NODE_STAGE_TRANSFORM,
};

static const rkvc_node_factory *malformed_factories(void *probe_ctx,
                                                    size_t *count) {
    (void)probe_ctx;
    *count = 1;
    return &g_malformed_factory;
}

static void test_malformed_and_duplicate_backend_rejected(void **state) {
    rkvc_context *ctx = NULL;
    rkvc_backend malformed = {
        .abi_version = RKVC_ABI_VERSION,
        .id = "malformed-backend",
        .factories = malformed_factories,
    };
    (void)state;

    assert_int_equal(rkvc_context_create(NULL, &ctx), RKVC_STATUS_OK);
    assert_int_equal(rkvc_registry_add_backend(ctx, &malformed),
                     RKVC_STATUS_FORMAT);
    assert_int_equal(rkvc_registry_add_backend(ctx, &g_selftest),
                     RKVC_STATUS_FORMAT);
    assert_false(has_backend(ctx, "malformed-backend"));
    rkvc_context_destroy(ctx);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_builtin_registered),
        cmocka_unit_test(test_dso_loaded_and_badabi_rejected),
        cmocka_unit_test(test_missing_dir_tolerated),
        cmocka_unit_test(test_malformed_and_duplicate_backend_rejected),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
