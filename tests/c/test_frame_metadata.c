/* SPDX-License-Identifier: AGPL-3.0-or-later */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>

#include <string.h>

#include "rkvc/rkvc.h"

static void init_nv12(rkvc_frame_desc *desc, unsigned char *pixels,
                      size_t size) {
    rkvc_frame_desc_init(desc, sizeof(*desc));
    desc->spec.width = 64;
    desc->spec.height = 48;
    desc->spec.stride = 64;
    desc->spec.ver_stride = 48;
    desc->spec.fmt = RKVC_FRAME_FMT_NV12;
    desc->spec.domain = RKVC_MEM_DOMAIN_HOST;
    desc->data = pixels;
    desc->size = size;
}

static void test_roi_is_validated_and_copied(void **state) {
    unsigned char pixels[64 * 48 * 3 / 2] = {0};
    rkvc_roi_region roi = {16, 8, 32, 24, -7, 1, 0};
    rkvc_frame_desc desc, got;
    rkvc_frame *frame = NULL;
    (void)state;

    init_nv12(&desc, pixels, sizeof(pixels));
    desc.roi_regions = &roi;
    desc.roi_region_count = 1;
    desc.encode.bitrate_bps = 800000;
    desc.encode.gop_size = 30;
    desc.encode.force_idr = 1;
    assert_int_equal(rkvc_frame_wrap(&desc, &frame), RKVC_STATUS_OK);

    /* 帧只借用像素载荷，但元数据（ROI/编码控制）为帧自有深拷贝。 */
    roi.x = 0;
    roi.qp_delta = 20;
    assert_int_equal(rkvc_frame_get_desc(frame, &got), RKVC_STATUS_OK);
    assert_int_equal(got.roi_region_count, 1);
    assert_int_equal(got.roi_regions[0].x, 16);
    assert_int_equal(got.roi_regions[0].qp_delta, -7);
    assert_int_equal(got.roi_regions[0].force_intra, 1);
    assert_int_equal(got.encode.bitrate_bps, 800000);
    assert_int_equal(got.encode.gop_size, 30);
    assert_int_equal(got.encode.force_idr, 1);
    rkvc_frame_release(frame);
}

static void test_roi_rejects_bad_rectangles(void **state) {
    unsigned char pixels[64 * 48 * 3 / 2] = {0};
    rkvc_roi_region roi = {48, 0, 32, 16, 0, 0, 0};
    rkvc_frame_desc desc;
    rkvc_frame *frame = NULL;
    (void)state;

    init_nv12(&desc, pixels, sizeof(pixels));
    desc.roi_regions = &roi;
    desc.roi_region_count = 1;
    assert_int_equal(rkvc_frame_wrap(&desc, &frame), RKVC_STATUS_INVALID);
    roi.x = 0;
    roi.width = 16;
    roi.qp_delta = 52;
    assert_int_equal(rkvc_frame_wrap(&desc, &frame), RKVC_STATUS_INVALID);
    roi.qp_delta = 0;
    desc.roi_regions = NULL;
    assert_int_equal(rkvc_frame_wrap(&desc, &frame), RKVC_STATUS_INVALID);
    desc.roi_regions = &roi;
    desc.roi_region_count = RKVC_ROI_MAX_REGIONS + 1;
    assert_int_equal(rkvc_frame_wrap(&desc, &frame), RKVC_STATUS_INVALID);
}

static void test_control_rejects_invalid_values(void **state) {
    unsigned char pixels[64 * 48 * 3 / 2] = {0};
    rkvc_frame_desc desc;
    rkvc_frame *frame = NULL;
    (void)state;

    init_nv12(&desc, pixels, sizeof(pixels));
    desc.encode.bitrate_bps = -1;
    assert_int_equal(rkvc_frame_wrap(&desc, &frame), RKVC_STATUS_INVALID);
    desc.encode.bitrate_bps = 0;
    desc.encode.gop_size = (uint32_t)INT32_MAX + 1u;
    assert_int_equal(rkvc_frame_wrap(&desc, &frame), RKVC_STATUS_INVALID);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_roi_is_validated_and_copied),
        cmocka_unit_test(test_roi_rejects_bad_rectangles),
        cmocka_unit_test(test_control_rejects_invalid_values),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
