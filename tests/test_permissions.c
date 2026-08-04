/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file test_permissions.c
 * @brief Device permission gate tests using a fake /dev tree.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "internal.h"

#ifndef RKVC_ENABLE_FAULT_INJECTION
#error "test_permissions.c requires RKVC_ENABLE_FAULT_INJECTION"
#endif

static char g_dev_root[PATH_MAX];

static void make_path(const char *rel, char *out, size_t size)
{
    snprintf(out, size, "%s/%s", g_dev_root, rel);
}

static void mkdir_checked(const char *rel)
{
    char path[PATH_MAX];
    make_path(rel, path, sizeof(path));
    assert_true(mkdir(path, 0777) == 0 || errno == EEXIST);
}

static void touch_checked(const char *rel)
{
    char path[PATH_MAX];
    make_path(rel, path, sizeof(path));
    FILE *fp = fopen(path, "wb");
    assert_non_null(fp);
    fclose(fp);
}

static int setup_fake_dev(void **state)
{
    (void)state;

    snprintf(g_dev_root, sizeof(g_dev_root), "/tmp/rkvc-dev-XXXXXX");
    assert_non_null(mkdtemp(g_dev_root));
    mkdir_checked("dev");
    setenv("RKVC_TEST_DEV_ROOT", g_dev_root, 1);
    unsetenv("RKVC_TEST_DENY_DEV_PATH");
    return 0;
}

static int teardown_fake_dev(void **state)
{
    (void)state;

    unsetenv("RKVC_TEST_DEV_ROOT");
    unsetenv("RKVC_TEST_DENY_DEV_PATH");

    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dev_root);
    assert_int_equal(system(cmd), 0);
    g_dev_root[0] = '\0';
    return 0;
}

static void add_mpp_service(void)
{
    touch_checked("dev/mpp_service");
}

static void add_rkvenc_service(void)
{
    touch_checked("dev/rkvenc");
}

static void add_dma_heap_dir(void)
{
    mkdir_checked("dev/dma_heap");
}

static void add_default_dma_heaps(void)
{
    add_dma_heap_dir();
    touch_checked("dev/dma_heap/system-uncached");
    touch_checked("dev/dma_heap/system");
    touch_checked("dev/dma_heap/system-uncached-dma32");
    touch_checked("dev/dma_heap/system-dma32");
}

static void add_render_node(void)
{
    mkdir_checked("dev/dri");
    touch_checked("dev/dri/renderD128");
}

static void test_missing_mpp_service_denied(void **state)
{
    (void)state;

    add_default_dma_heaps();
    assert_int_equal(rkvc_check_hw_permissions(), RKVC_ERR_PERMISSION);
}

static void test_dma_heap_directory_without_default_heap_denied(void **state)
{
    (void)state;

    add_mpp_service();
    add_dma_heap_dir();
    touch_checked("dev/dma_heap/cma");
    add_render_node();

    assert_int_equal(rkvc_check_hw_permissions(), RKVC_ERR_PERMISSION);
}

static void test_dma_heap_default_node_denied(void **state)
{
    (void)state;

    add_mpp_service();
    add_default_dma_heaps();
    add_render_node();
    setenv("RKVC_TEST_DENY_DEV_PATH", "/dev/dma_heap/system-uncached", 1);

    assert_int_equal(rkvc_check_hw_permissions(), RKVC_OK);

    unsetenv("RKVC_TEST_DENY_DEV_PATH");
    setenv("RKVC_TEST_DENY_DEV_PATH", "/dev/dma_heap/system", 1);
    assert_int_equal(rkvc_check_hw_permissions(), RKVC_OK);
}

static void test_all_default_dma_heaps_denied(void **state)
{
    (void)state;

    add_mpp_service();
    add_dma_heap_dir();
    add_render_node();
    touch_checked("dev/dma_heap/cma");

    assert_int_equal(rkvc_check_hw_permissions(), RKVC_ERR_PERMISSION);
}

static void test_drm_fallback_when_dma_heap_absent(void **state)
{
    (void)state;

    add_mpp_service();
    add_render_node();

    assert_int_equal(rkvc_check_hw_permissions(), RKVC_OK);
}

static void test_legacy_codec_node_allows_permission_gate(void **state)
{
    (void)state;

    add_rkvenc_service();
    add_default_dma_heaps();

    assert_int_equal(rkvc_check_hw_permissions(), RKVC_OK);
}

static void test_query_caps_reflects_permission_gate(void **state)
{
    (void)state;

    rkvc_caps caps;
    add_default_dma_heaps();

    assert_int_equal(rkvc_query_caps(&caps), RKVC_OK);
    assert_int_equal(caps.has_av1_enc, 1);
    assert_int_equal(caps.has_h264_dec, 0);
    assert_int_equal(caps.has_dma_heap, 1);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(
            test_missing_mpp_service_denied, setup_fake_dev, teardown_fake_dev),
        cmocka_unit_test_setup_teardown(
            test_dma_heap_directory_without_default_heap_denied,
            setup_fake_dev, teardown_fake_dev),
        cmocka_unit_test_setup_teardown(
            test_dma_heap_default_node_denied, setup_fake_dev, teardown_fake_dev),
        cmocka_unit_test_setup_teardown(
            test_all_default_dma_heaps_denied, setup_fake_dev, teardown_fake_dev),
        cmocka_unit_test_setup_teardown(
            test_drm_fallback_when_dma_heap_absent, setup_fake_dev,
            teardown_fake_dev),
        cmocka_unit_test_setup_teardown(
            test_legacy_codec_node_allows_permission_gate, setup_fake_dev,
            teardown_fake_dev),
        cmocka_unit_test_setup_teardown(
            test_query_caps_reflects_permission_gate, setup_fake_dev,
            teardown_fake_dev),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
