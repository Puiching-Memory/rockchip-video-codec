/**
 * @file test_reconfig.c
 * @brief 热切换 API 单元测试（无需硬件）。
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include "rkvc/rkvc.h"

static void test_reconfig_api(void **state)
{
    (void)state;
    rkvc_pipeline_desc d = rkvc_pipeline_desc_defaults();
    d.template_id = RKVC_TEMPLATE_FILE_ENCODE;
    d.output_path = "/tmp/rkvc_reconfig_dummy.mp4";
    d.bitrate = 2000000;
    d.gop_size = 30;

    rkvc_session *s = NULL;
    assert_int_equal(rkvc_session_create(&d, &s), RKVC_OK);
    assert_non_null(s);

    assert_int_equal(rkvc_session_set_bitrate(s, 0), RKVC_ERR_INVALID);
    assert_int_equal(rkvc_session_set_bitrate(s, 1500000), RKVC_OK);
    assert_int_equal(rkvc_session_set_gop(s, 0), RKVC_ERR_INVALID);
    assert_int_equal(rkvc_session_set_gop(s, 15), RKVC_OK);
    assert_int_equal(rkvc_session_request_idr(s), RKVC_OK);

    rkvc_reconfig_desc r = {
        .flags = RKVC_RECONFIG_BITRATE | RKVC_RECONFIG_GOP | RKVC_RECONFIG_IDR,
        .bitrate = 800000,
        .gop_size = 60,
    };
    assert_int_equal(rkvc_session_reconfigure(s, &r), RKVC_OK);

    r.flags = RKVC_RECONFIG_BITRATE;
    r.bitrate = -1;
    assert_int_equal(rkvc_session_reconfigure(s, &r), RKVC_ERR_INVALID);

    assert_non_null(rkvc_session_port(s, "preview"));

    rkvc_session_destroy(s);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_reconfig_api),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
