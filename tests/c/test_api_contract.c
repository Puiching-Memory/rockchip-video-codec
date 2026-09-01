/* SPDX-License-Identifier: AGPL-3.0-or-later */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>

#include <string.h>

#include "rkvc/rkvc.h"

static void test_initializers_publish_current_abi(void **state) {
    rkvc_request req;
    rkvc_context_options opts;
    rkvc_frame_desc desc;
    (void)state;

    memset(&req, 0xff, sizeof(req));
    memset(&opts, 0xff, sizeof(opts));
    memset(&desc, 0xff, sizeof(desc));
    rkvc_request_init(&req, sizeof(req));
    rkvc_context_options_init(&opts, sizeof(opts));
    rkvc_frame_desc_init(&desc, sizeof(desc));
    assert_int_equal(req.header.struct_size, sizeof(req));
    assert_int_equal(opts.header.struct_size, sizeof(opts));
    assert_int_equal(desc.header.struct_size, sizeof(desc));
    assert_int_equal(req.header.api_version, RKVC_ABI_VERSION);
    assert_int_equal(opts.header.api_version, RKVC_ABI_VERSION);
    assert_int_equal(desc.header.api_version, RKVC_ABI_VERSION);
    assert_int_equal(req.quality.qp, -1);
    assert_int_equal(desc.fd, -1);
    assert_int_equal(desc.pts, RKVC_FRAME_TS_UNKNOWN);
    assert_int_equal(desc.dts, RKVC_FRAME_TS_UNKNOWN);
}

static void test_status_strings_are_total(void **state) {
    int status;
    (void)state;
    for (status = RKVC_STATUS_INTEGRITY; status <= RKVC_STATUS_OK; ++status)
        assert_non_null(rkvc_status_str((rkvc_status)status));
    assert_string_equal(rkvc_status_str((rkvc_status)-999), "unknown");
}

/* ROI 字段追加之前编译的调用方（旧 struct_size）仍然被接受。 */
static void test_legacy_frame_prefix_remains_accepted(void **state) {
    unsigned char data[24] = {0};
    rkvc_frame_desc desc;
    rkvc_frame *frame = NULL;
    (void)state;

    rkvc_frame_desc_init(&desc, sizeof(desc));
    desc.header.struct_size = offsetof(rkvc_frame_desc, roi_regions);
    desc.spec.width = 4;
    desc.spec.height = 4;
    desc.spec.stride = 4;
    desc.spec.ver_stride = 4;
    desc.spec.fmt = RKVC_FRAME_FMT_NV12;
    desc.spec.domain = RKVC_MEM_DOMAIN_HOST;
    desc.data = data;
    desc.size = sizeof(data);
    assert_int_equal(rkvc_frame_wrap(&desc, &frame), RKVC_STATUS_OK);
    assert_non_null(frame);
    rkvc_frame_release(frame);
}

static void test_header_major_version_is_checked(void **state) {
    unsigned char data[24] = {0};
    rkvc_frame_desc desc;
    rkvc_frame *frame = NULL;
    (void)state;

    rkvc_frame_desc_init(&desc, sizeof(desc));
    desc.header.api_version = 1u << 16;
    desc.spec.width = 4;
    desc.spec.height = 4;
    desc.spec.fmt = RKVC_FRAME_FMT_NV12;
    desc.spec.domain = RKVC_MEM_DOMAIN_HOST;
    desc.data = data;
    desc.size = sizeof(data);
    assert_int_equal(rkvc_frame_wrap(&desc, &frame), RKVC_STATUS_INVALID);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_initializers_publish_current_abi),
        cmocka_unit_test(test_status_strings_are_total),
        cmocka_unit_test(test_legacy_frame_prefix_remains_accepted),
        cmocka_unit_test(test_header_major_version_is_checked),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
