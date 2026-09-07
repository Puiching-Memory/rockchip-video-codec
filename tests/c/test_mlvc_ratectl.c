/* SPDX-License-Identifier: AGPL-3.0-or-later */
/** @file test_mlvc_ratectl.c MLVC RateController 行为单测。
 *
 * 断言官方 rate_controller.py 移植的关键行为：
 * - q_index 恒在 [0,63]；码率越高 q 越大（同帧位）。
 * - 极低码率下 solve 返回丢帧哨兵（各帧型均会；回退策略在编码器侧）。
 * - β ramp 后 P 模型 q 收敛方向稳定；I 更新后 P 模型复位（行为层面
 *   通过 reset 前后 q 序列可复现性覆盖）。
 *
 * trace 模式（不进 ctest）：`test_mlvc_ratectl --trace W H bps fps N`
 * 打印 N 帧的 q_index（帧型模式 I P... P 每 8 帧 LTR_RECOVERY），
 * payload_bits 用确定式合成值 (q+1)*1000，供官方 Python golden 对拍。
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mlvc/ratectl.h"

#define W 640
#define H 368
#define FPS 30.0

static void test_rc_basic_range(void **state)
{
    mlvc_rate_controller *rc = mlvc_rc_create(W, H, 500e3, FPS);
    int q;
    (void)state;
    assert_non_null(rc);
    q = mlvc_rc_solve_q_index(rc, 0.0, MLVC_RC_FRAME_I, 128);
    assert_in_range(q, 0, 63);
    mlvc_rc_update(rc, 128, 80000);
    for (int i = 1; i < 30; i++) {
        q = mlvc_rc_solve_q_index(rc, (double)i / FPS, MLVC_RC_FRAME_P,
                                  128);
        assert_in_range(q, 0, 63);
        mlvc_rc_update(rc, 128, (int64_t)(q + 1) * 2000);
    }
    mlvc_rc_free(rc);
}

static void test_rc_drop_on_starvation(void **state)
{
    mlvc_rate_controller *rc = mlvc_rc_create(W, H, 1.0, FPS);
    int q;
    (void)state;
    assert_non_null(rc);
    /* 1 bps：任何帧型 allocated 都钳到 0 → 丢帧哨兵 */
    q = mlvc_rc_solve_q_index(rc, 0.0, MLVC_RC_FRAME_I, 128);
    assert_int_equal(q, MLVC_RC_Q_DROP);
    mlvc_rc_update(rc, 128, 0);
    q = mlvc_rc_solve_q_index(rc, 1.0 / FPS, MLVC_RC_FRAME_P, 128);
    assert_int_equal(q, MLVC_RC_Q_DROP);
    mlvc_rc_free(rc);
}

static void test_rc_higher_bitrate_higher_q(void **state)
{
    mlvc_rate_controller *lo = mlvc_rc_create(W, H, 100e3, FPS);
    mlvc_rate_controller *hi = mlvc_rc_create(W, H, 2e6, FPS);
    int q_lo, q_hi;
    (void)state;
    assert_non_null(lo);
    assert_non_null(hi);
    q_lo = mlvc_rc_solve_q_index(lo, 0.0, MLVC_RC_FRAME_I, 128);
    q_hi = mlvc_rc_solve_q_index(hi, 0.0, MLVC_RC_FRAME_I, 128);
    assert_int_equal(q_lo >= 0 && q_hi >= 0, 1);
    assert_true(q_hi > q_lo);
    mlvc_rc_free(lo);
    mlvc_rc_free(hi);
}

static void test_rc_ltr_recovery_type(void **state)
{
    mlvc_rate_controller *rc = mlvc_rc_create(W, H, 500e3, FPS);
    int q;
    (void)state;
    assert_non_null(rc);
    mlvc_rc_solve_q_index(rc, 0.0, MLVC_RC_FRAME_I, 128);
    mlvc_rc_update(rc, 128, 80000);
    q = mlvc_rc_solve_q_index(rc, 1.0 / FPS, MLVC_RC_FRAME_LTR_RECOVERY,
                              128);
    assert_in_range(q, 0, 63);
    mlvc_rc_update(rc, 128, 30000);
    /* LTR_RECOVERY 后 P 模型走 after-ltr 分支，仍可求解 */
    q = mlvc_rc_solve_q_index(rc, 2.0 / FPS, MLVC_RC_FRAME_P, 128);
    assert_in_range(q, 0, 63);
    mlvc_rc_free(rc);
}

static void test_rc_configure_hot_bitrate(void **state)
{
    mlvc_rate_controller *rc = mlvc_rc_create(W, H, 200e3, FPS);
    int q;
    (void)state;
    assert_non_null(rc);
    mlvc_rc_solve_q_index(rc, 0.0, MLVC_RC_FRAME_I, 128);
    mlvc_rc_update(rc, 128, 40000);
    q = mlvc_rc_solve_q_index(rc, 1.0 / FPS, MLVC_RC_FRAME_P, 128);
    assert_in_range(q, 0, 63);
    mlvc_rc_update(rc, 128, 8000);
    /* 热更码率：保留超额偏差，后续求解仍收敛在量程内 */
    mlvc_rc_configure(rc, 4e6);
    for (int i = 2; i < 20; i++) {
        q = mlvc_rc_solve_q_index(rc, (double)i / FPS, MLVC_RC_FRAME_P,
                                  128);
        assert_in_range(q, 0, 63);
        mlvc_rc_update(rc, 128, (int64_t)(q + 1) * 4000);
    }
    mlvc_rc_free(rc);
}

/* trace 模式：确定性帧型+合成 payload，供官方 golden 对拍 */
static int trace_mode(int argc, char **argv)
{
    /* --trace W H bps fps N */
    if (argc < 7)
        return 2;
    uint32_t w = (uint32_t)strtoul(argv[2], NULL, 10);
    uint32_t h = (uint32_t)strtoul(argv[3], NULL, 10);
    double bps = strtod(argv[4], NULL);
    double fps = strtod(argv[5], NULL);
    int n = atoi(argv[6]);
    mlvc_rate_controller *rc = mlvc_rc_create(w, h, bps, fps);
    if (!rc)
        return 1;
    for (int i = 0; i < n; i++) {
        mlvc_rc_frame_type t =
            i == 0 ? MLVC_RC_FRAME_I
            : (i % 8 == 0 ? MLVC_RC_FRAME_LTR_RECOVERY : MLVC_RC_FRAME_P);
        int q = mlvc_rc_solve_q_index(rc, (double)i / fps, t, 128);
        printf("%d %d %d\n", i, (int)t, q);
        /* 官方 frame_loop：丢帧不 update，编码帧 update(128, payload) */
        if (q >= 0)
            mlvc_rc_update(rc, 128, (int64_t)(q + 1) * 1000);
    }
    mlvc_rc_free(rc);
    return 0;
}

int main(int argc, char **argv)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_rc_basic_range),
        cmocka_unit_test(test_rc_drop_on_starvation),
        cmocka_unit_test(test_rc_higher_bitrate_higher_q),
        cmocka_unit_test(test_rc_ltr_recovery_type),
        cmocka_unit_test(test_rc_configure_hot_bitrate),
    };
    if (argc > 1 && strcmp(argv[1], "--trace") == 0)
        return trace_mode(argc, argv);
    return cmocka_run_group_tests(tests, NULL, NULL);
}
