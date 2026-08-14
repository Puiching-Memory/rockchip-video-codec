/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#include "rkvc/rkvc.h"
#include "qppatch.h"

static void wr_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void wr_u64le(uint8_t *p, uint64_t v)
{
    wr_u32le(p, (uint32_t)v);
    wr_u32le(p + 4, (uint32_t)(v >> 32));
}

static uint32_t crc32_buf(const uint8_t *p, size_t n)
{
    uLong c = crc32(0L, Z_NULL, 0);
    if (n)
        c = crc32(c, p, (uInt)n);
    return (uint32_t)c;
}

typedef struct {
    uint32_t off;
    uint32_t len;
} test_range;

static uint8_t *build_patch(const uint8_t *base, size_t n, const uint8_t *target,
                            uint32_t qp, const test_range *rs, uint32_t nr,
                            size_t *out_sz)
{
    size_t payload_n = 0;
    uint32_t i;
    for (i = 0; i < nr; i++)
        payload_n += rs[i].len;
    size_t sz = RKVC_QPPATCH_HEADER_SIZE + (size_t)nr * 8 + payload_n;
    uint8_t *p = malloc(sz);
    assert_non_null(p);
    memset(p, 0, sz);
    memcpy(p, "QPP1", 4);
    wr_u32le(p + 4, 1);
    wr_u64le(p + 8, n);
    wr_u32le(p + 16, qp);
    wr_u32le(p + 20, nr);
    wr_u32le(p + 24, 0);
    wr_u32le(p + 28, crc32_buf(base, n));
    wr_u32le(p + 36, 64);
    uint8_t *ranges = p + RKVC_QPPATCH_HEADER_SIZE;
    uint8_t *payload = ranges + (size_t)nr * 8;
    size_t cur = 0;
    for (i = 0; i < nr; i++) {
        wr_u32le(ranges + (size_t)i * 8, rs[i].off);
        wr_u32le(ranges + (size_t)i * 8 + 4, rs[i].len);
        memcpy(payload + cur, target + rs[i].off, rs[i].len);
        cur += rs[i].len;
    }
    wr_u32le(p + 32, crc32_buf(payload, payload_n));
    *out_sz = sz;
    return p;
}

static void test_apply_two_ranges(void **state)
{
    (void)state;
    uint8_t base[256];
    uint8_t target[256];
    memset(base, 0x11, sizeof base);
    memcpy(target, base, sizeof base);
    memset(target + 10, 0xAA, 6);
    target[100] = 0xBB;

    test_range rs[] = { {10, 6}, {100, 1} };
    size_t psz = 0;
    uint8_t *patch = build_patch(base, sizeof base, target, 25, rs, 2, &psz);

    uint8_t work[256];
    memcpy(work, base, sizeof work);
    assert_int_equal(rkvc_qppatch_apply(work, sizeof work, patch, psz, 25), RKVC_OK);
    assert_memory_equal(work, target, sizeof target);
    free(patch);
}

static void test_empty_patch_identity(void **state)
{
    (void)state;
    uint8_t base[64];
    memset(base, 0x5A, sizeof base);
    size_t psz = 0;
    uint8_t *patch = build_patch(base, sizeof base, base, 21, NULL, 0, &psz);
    uint8_t work[64];
    memcpy(work, base, sizeof work);
    assert_int_equal(rkvc_qppatch_apply(work, sizeof work, patch, psz, 21), RKVC_OK);
    assert_memory_equal(work, base, sizeof base);
    free(patch);
}

static void test_wrong_qp(void **state)
{
    (void)state;
    uint8_t base[32];
    memset(base, 1, sizeof base);
    size_t psz = 0;
    uint8_t *patch = build_patch(base, sizeof base, base, 25, NULL, 0, &psz);
    assert_int_equal(rkvc_qppatch_apply(base, sizeof base, patch, psz, 21), RKVC_ERR_FORMAT);
    assert_int_equal(rkvc_qppatch_apply(base, sizeof base, patch, psz, -1), RKVC_OK);
    free(patch);
}

static void test_crc_and_size(void **state)
{
    (void)state;
    uint8_t base[32];
    uint8_t other[32];
    memset(base, 1, sizeof base);
    memset(other, 2, sizeof other);
    size_t psz = 0;
    uint8_t *patch = build_patch(base, sizeof base, base, 21, NULL, 0, &psz);
    assert_int_equal(rkvc_qppatch_apply(other, sizeof other, patch, psz, 21), RKVC_ERR_FORMAT);
    assert_int_equal(rkvc_qppatch_apply(base, 16, patch, psz, 21), RKVC_ERR_FORMAT);
    patch[0] = 'X';
    assert_int_equal(rkvc_qppatch_apply(base, sizeof base, patch, psz, 21), RKVC_ERR_FORMAT);
    free(patch);
}

static void test_range_overflow(void **state)
{
    (void)state;
    uint8_t base[32];
    memset(base, 0, sizeof base);
    test_range rs[] = { {30, 8} };
    uint8_t target[32];
    memset(target, 9, sizeof target);
    size_t psz = 0;
    uint8_t *patch = build_patch(base, sizeof base, target, 21, rs, 1, &psz);
    assert_int_equal(rkvc_qppatch_apply(base, sizeof base, patch, psz, 21), RKVC_ERR_FORMAT);
    free(patch);
}

static void test_build_path_and_file(void **state)
{
    (void)state;
    char path[256];
    assert_int_equal(rkvc_qppatch_build_path(path, sizeof path, "/tmp/patches/", "enc", 25),
                     RKVC_OK);
    assert_string_equal(path, "/tmp/patches/enc_qp25.qppatch");

    const char *resolved = (const char *)(uintptr_t)1;
    assert_int_equal(rkvc_qppatch_resolve(NULL, "enc", 21, path, sizeof path, &resolved),
                     RKVC_OK);
    assert_null(resolved);
    assert_int_equal(rkvc_qppatch_resolve("", "enc", 21, path, sizeof path, &resolved),
                     RKVC_OK);
    assert_null(resolved);
    assert_int_equal(
        rkvc_qppatch_resolve("/no/such/rkvc_qp_patches", "enc", 21, path, sizeof path,
                             &resolved),
        RKVC_ERR_IO);

    uint8_t base[48];
    uint8_t target[48];
    memset(base, 0x22, sizeof base);
    memcpy(target, base, sizeof base);
    memset(target + 4, 0x33, 8);
    test_range rs[] = { {4, 8} };
    size_t psz = 0;
    uint8_t *patch = build_patch(base, sizeof base, target, 29, rs, 1, &psz);

    char tmp[] = "/tmp/rkvc_qppatch_XXXXXX";
    int fd = mkstemp(tmp);
    assert_true(fd >= 0);
    assert_int_equal((size_t)write(fd, patch, psz), psz);
    close(fd);
    free(patch);

    uint8_t work[48];
    memcpy(work, base, sizeof work);
    assert_int_equal(rkvc_qppatch_apply_file(work, sizeof work, tmp, 29), RKVC_OK);
    assert_memory_equal(work, target, sizeof target);
    unlink(tmp);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_apply_two_ranges),
        cmocka_unit_test(test_empty_patch_identity),
        cmocka_unit_test(test_wrong_qp),
        cmocka_unit_test(test_crc_and_size),
        cmocka_unit_test(test_range_overflow),
        cmocka_unit_test(test_build_path_and_file),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
