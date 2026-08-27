/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "rkvc_sr_phase.h"

static void test_pack_pixel_unshuffle_order(void **state)
{
    (void)state;
    const uint8_t y[16] = {
         0,  1,  2,  3,
         4,  5,  6,  7,
         8,  9, 10, 11,
        12, 13, 14, 15,
    };
    const uint8_t uv[8] = {
        20, 30, 21, 31,
        22, 32, 23, 33,
    };
    uint8_t phases[48];
    memset(phases, 0, sizeof(phases));

    assert_int_equal(rkvc_sr_phase_pack_nv12(y, 4, uv, 4, 4, 4,
                                             phases, sizeof(phases)), 0);
    /* NHWC 宿主字节序：每个 core 像素的 12 个 phase 通道连续存放。
     * 逻辑值与旧平面测试完全相同（像素序 (0,0),(0,1),(1,0),(1,1)，
     * 通道序 ch*4 + dy*2 + dx），仅物理排列改为逐像素交错。 */
    static const uint8_t plane_y[16] = {
        0, 2, 8, 10,  1, 3, 9, 11,
        4, 6, 12, 14, 5, 7, 13, 15,
    };
    static const uint8_t plane_u[16] = {
        20, 21, 22, 23, 21, 21, 23, 23,
        21, 22, 22, 23, 22, 22, 23, 23,
    };
    static const uint8_t plane_v[16] = {
        30, 31, 32, 33, 31, 31, 33, 33,
        31, 32, 32, 33, 32, 32, 33, 33,
    };
    const uint8_t *planes[3] = { plane_y, plane_u, plane_v };
    for (int ch = 0; ch < 3; ch++)
        for (int sub = 0; sub < 4; sub++)
            for (int px = 0; px < 4; px++)
                assert_int_equal(phases[px * 12 + ch * 4 + sub],
                                 planes[ch][sub * 4 + px]);
}

static void test_pack_rejects_bad_contract(void **state)
{
    (void)state;
    uint8_t bytes[128] = {0};
    assert_int_equal(rkvc_sr_phase_pack_nv12(bytes, 4, bytes, 4, 3, 4,
                                             bytes, sizeof(bytes)), -1);
    assert_int_equal(rkvc_sr_phase_pack_nv12(bytes, 4, bytes, 4, 4, 4,
                                             bytes, 47), -1);
}

static void test_add_zero_residual_keeps_base(void **state)
{
    (void)state;
    uint8_t y[36];
    uint8_t uv[18];
    float residual[108] = {0};
    memset(y, 80, sizeof(y));
    memset(uv, 120, sizeof(uv));
    assert_int_equal(rkvc_sr_phase_add_residual_nv12(residual, 1, 1,
                                                     y, 6, uv, 6, 6, 6), 0);
    for (size_t i = 0; i < sizeof(y); i++)
        assert_int_equal(y[i], 80);
    for (size_t i = 0; i < sizeof(uv); i++)
        assert_int_equal(uv[i], 120);
}

static void test_add_pixel_shuffle_and_chroma_average(void **state)
{
    (void)state;
    uint8_t y[36];
    uint8_t uv[18];
    float residual[108] = {0};
    memset(y, 100, sizeof(y));
    memset(uv, 100, sizeof(uv));

    /* Y channel packed index dy*6+dx. */
    residual[0] = 10.0f;
    residual[35] = -200.0f;
    /* U top-left 2x2: (4 + 8 + 10 + 14) / 4 = 9. */
    residual[36 + 0] = 4.0f;
    residual[36 + 1] = 8.0f;
    residual[36 + 6] = 10.0f;
    residual[36 + 7] = 14.0f;
    /* V top-left 2x2 average clips high. */
    residual[72 + 0] = 800.0f;
    residual[72 + 1] = 800.0f;
    residual[72 + 6] = 800.0f;
    residual[72 + 7] = 800.0f;

    assert_int_equal(rkvc_sr_phase_add_residual_nv12(residual, 1, 1,
                                                     y, 6, uv, 6, 6, 6), 0);
    assert_int_equal(y[0], 110);
    assert_int_equal(y[35], 0);
    assert_int_equal(uv[0], 109);
    assert_int_equal(uv[1], 255);
}

static void test_add_residual_is_plane_major(void **state)
{
    (void)state;
    /* 2x1 的 core：plane=2，NCHW 平面主序与 NHWC 像素交错的偏移不再重合，
     * 可锁死 rknn 输出属性 dims=1x108xHxW 的字节序契约。 */
    uint8_t y[72];
    uint8_t uv[36];
    float residual[216] = {0};
    memset(y, 100, sizeof(y));
    memset(uv, 100, sizeof(uv));

    /* Y：channel 0、(dy,dx)=(0,0)、core (0,1) -> 平面偏移 0*2 + 1 = 1，
     * 对应全分辨率像素 (row 0, col 1*6+0) = y[6]。 */
    residual[0 * 2 + 1] = 10.0f;
    /* V：packed_c = 2*36 = 72、core (0,0) -> 平面偏移 72*2 = 144；
     * 若误按 NHWC 读，144 会落到第二个 core 像素的 U 通道。 */
    residual[72 * 2 + 0] = 20.0f;

    assert_int_equal(rkvc_sr_phase_add_residual_nv12(residual, 2, 1,
                                                     y, 12, uv, 12, 12, 6), 0);
    assert_int_equal(y[6], 110);
    assert_int_equal(y[0], 100);
    /* V 的 (0,0) 子带只贡献 1 个 20，2x2 平均后 +5；U 不受影响。 */
    assert_int_equal(uv[1], 105);
    assert_int_equal(uv[0], 100);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_pack_pixel_unshuffle_order),
        cmocka_unit_test(test_pack_rejects_bad_contract),
        cmocka_unit_test(test_add_zero_residual_keeps_base),
        cmocka_unit_test(test_add_pixel_shuffle_and_chroma_average),
        cmocka_unit_test(test_add_residual_is_plane_major),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
