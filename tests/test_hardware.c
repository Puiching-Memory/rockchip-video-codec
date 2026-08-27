/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file test_hardware.c
 * @brief Session 级硬件集成测试（需 RKVC_RUN_HARDWARE_TESTS=1）。
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>
#include <cmocka.h>
#include <libavformat/avformat.h>
#include "rkvc/rkvc.h"
#include "test_support.h"
#include "test_fixtures.h"

/** mkstemp 要求 XXXXXX 在路径末尾；生成带后缀的空临时文件供 session 写入。 */
static int test_mktemp_file(char *out_path, size_t out_sz,
                            const char *prefix, const char *suffix)
{
    char tmpl[PATH_MAX];
    snprintf(tmpl, sizeof(tmpl), "%sXXXXXX", prefix);
    int fd = mkstemp(tmpl);
    if (fd < 0)
        return -1;
    close(fd);

    if (suffix && suffix[0]) {
        snprintf(out_path, out_sz, "%s%s", tmpl, suffix);
        if (rename(tmpl, out_path) != 0) {
            unlink(tmpl);
            return -1;
        }
    } else {
        snprintf(out_path, out_sz, "%s", tmpl);
    }
    return 0;
}

static const char *fixture_h264(void)
{
    const char *env = getenv("RKVC_TEST_INPUT_H264");
    if (env && env[0])
        return env;

    const char *gen = rkvc_test_fixture_h264_mp4(640, 480, 10);
    if (!gen)
        return "tests/fixtures/sample.h264.mp4";
    return gen;
}

/** 约定模型：RKVC_SR_MODEL 覆盖，否则 Phase-RLFN bundle 内 RKNN。 */
static const char *resolve_sr_model_path(char *buf, size_t buf_sz)
{
    const char *env = getenv("RKVC_SR_MODEL");
    if (env && env[0]) {
        struct stat st;
        if (stat(env, &st) == 0 && S_ISREG(st.st_mode))
            return env;
        return NULL;
    }

    const char *root = getenv("RKVC_SOURCE_ROOT");
    if (!root || !root[0])
        return NULL;

    snprintf(buf, buf_sz,
             "%s/models/rkvc-sr/phase_rlfn_sr_x3.rknn", root);
    {
        struct stat st;
        if (stat(buf, &st) == 0 && S_ISREG(st.st_mode))
            return buf;
    }
    return NULL;
}

static void skip_unless_hw(void)
{
    if (!rkvc_test_hardware_opted_in())
        skip();
    rkvc_init();
    rkvc_caps caps;
    if (rkvc_query_caps(&caps) != RKVC_OK)
        skip();
    if (!caps.has_h264_dec || !caps.has_h264_enc)
        skip();
}

static void skip_unless_hevc(void)
{
    skip_unless_hw();
    rkvc_caps caps;
    rkvc_query_caps(&caps);
    if (!caps.has_hevc_enc || !caps.has_hevc_dec)
        skip();
}

static void skip_unless_av1(void)
{
    skip_unless_hw();
    rkvc_caps caps;
    rkvc_query_caps(&caps);
    if (!caps.has_av1_enc || !caps.has_av1_dec)
        skip();
}

static int count_video_packets(const char *path)
{
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, NULL) < 0)
        return -1;
    if (avformat_find_stream_info(fmt, NULL) < 0) {
        avformat_close_input(&fmt);
        return -1;
    }

    int stream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO,
                                     -1, -1, NULL, 0);
    if (stream < 0) {
        avformat_close_input(&fmt);
        return -1;
    }

    int count = 0;
    AVPacket *pkt = av_packet_alloc();
    assert_non_null(pkt);
    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == stream)
            count++;
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avformat_close_input(&fmt);
    return count;
}

static void assert_complete_transcode(const char *input,
                                      const rkvc_session_stats *stats)
{
    int expected = count_video_packets(input);
    assert_true(expected > 0);
    assert_int_equal(stats->frames_in, expected);
    assert_int_equal(stats->frames_out, expected);
}

static void assert_complete_encode(const rkvc_session_stats *stats)
{
    assert_true(stats->frames_in > 0);
    assert_int_equal(stats->frames_out, stats->frames_in);
}

static rkvc_err run_transcode(rkvc_policy policy, int w, int h,
                              const char *input, const char *output,
                              rkvc_session_stats *stats)
{
    rkvc_pipeline_desc d;
    rkvc_pipeline_from_template(RKVC_TEMPLATE_FILE_TRANSCODE, &d);
    d.policy      = policy;
    d.input_path  = input;
    d.output_path = output;
    d.width       = w;
    d.height      = h;

    rkvc_session *s = NULL;
    if (rkvc_session_create(&d, &s) != RKVC_OK)
        return RKVC_ERR_INTERNAL;
    rkvc_err err = rkvc_session_run_file(s);
    if (stats)
        rkvc_session_get_stats(s, stats);
    rkvc_session_destroy(s);
    return err;
}

static void test_session_transcode_h264(void **state)
{
    (void)state;
    skip_unless_hw();

    char out[PATH_MAX];
    if (test_mktemp_file(out, sizeof(out), "/tmp/rkvc_transcode_", ".mp4") != 0)
        fail();

    const char *input = fixture_h264();
    rkvc_session_stats stats = {0};
    rkvc_err err = run_transcode(RKVC_POLICY_REALTIME, 640, 480,
                                 input, out, &stats);
    remove(out);
    assert_int_equal(err, RKVC_OK);
    assert_complete_transcode(input, &stats);
}

static void test_session_transcode_balanced(void **state)
{
    (void)state;
    skip_unless_hevc();

    char out[PATH_MAX];
    if (test_mktemp_file(out, sizeof(out), "/tmp/rkvc_trans_hevc_", ".mp4") != 0)
        fail();

    const char *input = fixture_h264();
    rkvc_session_stats stats = {0};
    rkvc_err err = run_transcode(RKVC_POLICY_BALANCED, 640, 480,
                                 input, out, &stats);
    remove(out);
    assert_int_equal(err, RKVC_OK);
    assert_complete_transcode(input, &stats);
}

static void test_session_transcode_quality(void **state)
{
    (void)state;
    skip_unless_av1();

    char out[PATH_MAX];
    if (test_mktemp_file(out, sizeof(out), "/tmp/rkvc_trans_av1_", ".mp4") != 0)
        fail();

    const char *input = fixture_h264();
    rkvc_session_stats stats = {0};
    rkvc_err err = run_transcode(RKVC_POLICY_QUALITY, 640, 480,
                                 input, out, &stats);
    remove(out);
    assert_int_equal(err, RKVC_OK);
    assert_complete_transcode(input, &stats);
}

static void test_session_transcode_offline(void **state)
{
    (void)state;
    skip_unless_av1();

    char out[PATH_MAX];
    if (test_mktemp_file(out, sizeof(out), "/tmp/rkvc_trans_offline_", ".mp4") != 0)
        fail();

    const char *input = fixture_h264();
    rkvc_session_stats stats = {0};
    rkvc_err err = run_transcode(RKVC_POLICY_OFFLINE, 640, 480,
                                 input, out, &stats);
    remove(out);
    assert_int_equal(err, RKVC_OK);
    assert_complete_transcode(input, &stats);
}

static void test_session_decode_nv12(void **state)
{
    (void)state;
    skip_unless_hw();

    char out[PATH_MAX];
    if (test_mktemp_file(out, sizeof(out), "/tmp/rkvc_decode_", ".nv12") != 0)
        fail();

    rkvc_pipeline_desc d;
    rkvc_pipeline_from_template(RKVC_TEMPLATE_FILE_DECODE, &d);
    const char *input = fixture_h264();
    d.input_path  = input;
    d.output_path = out;

    rkvc_session *s = NULL;
    rkvc_session_create(&d, &s);
    rkvc_err err = rkvc_session_run_file(s);
    rkvc_session_destroy(s);

    struct stat st;
    assert_int_equal(stat(out, &st), 0);
    int expected = count_video_packets(input);
    assert_true(expected > 0);
    assert_int_equal(st.st_size,
                     (off_t)expected * 640 * 480 * 3 / 2);
    remove(out);
    assert_int_equal(err, RKVC_OK);
}

static void test_session_encode_h264(void **state)
{
    (void)state;
    skip_unless_hw();

    const char *raw = getenv("RKVC_TEST_RAW_NV12");
    if (!raw || !raw[0])
        raw = rkvc_test_fixture_nv12_1080p();
    if (!raw)
        fail();

    char out[PATH_MAX];
    if (test_mktemp_file(out, sizeof(out), "/tmp/rkvc_encode_", ".mp4") != 0)
        fail();

    rkvc_pipeline_desc d;
    rkvc_pipeline_from_template(RKVC_TEMPLATE_FILE_ENCODE, &d);
    d.input_path  = raw;
    d.output_path = out;
    d.width       = 1920;
    d.height      = 1080;
    d.policy      = RKVC_POLICY_REALTIME;

    rkvc_session *s = NULL;
    rkvc_session_create(&d, &s);
    rkvc_err err = rkvc_session_run_file(s);
    rkvc_session_stats stats = {0};
    rkvc_session_get_stats(s, &stats);
    rkvc_session_destroy(s);
    remove(out);
    assert_int_equal(err, RKVC_OK);
    assert_complete_encode(&stats);
}

static void test_session_av1_storage(void **state)
{
    (void)state;
    skip_unless_av1();

    char out[PATH_MAX];
    if (test_mktemp_file(out, sizeof(out), "/tmp/rkvc_av1_", ".mp4") != 0)
        fail();

    rkvc_pipeline_desc d;
    rkvc_pipeline_from_template(RKVC_TEMPLATE_AV1_STORAGE, &d);
    d.input_path  = fixture_h264();
    d.output_path = out;
    d.width       = 640;
    d.height      = 480;

    rkvc_session *s = NULL;
    rkvc_session_create(&d, &s);
    rkvc_err err = rkvc_session_run_file(s);
    rkvc_session_stats stats = {0};
    rkvc_session_get_stats(s, &stats);
    rkvc_session_destroy(s);
    remove(out);
    assert_int_equal(err, RKVC_OK);
    assert_complete_transcode(d.input_path, &stats);
}

static void test_session_encode_decode_upscale_3x(void **state)
{
    (void)state;
    skip_unless_hw();

    rkvc_caps caps;
    rkvc_query_caps(&caps);
    if (!caps.has_rga)
        skip();

    const char *raw = getenv("RKVC_TEST_RAW_NV12");
    if (!raw || !raw[0])
        raw = rkvc_test_fixture_nv12_1080p();
    if (!raw)
        fail();

    char enc_path[PATH_MAX];
    char dec_path[PATH_MAX];
    if (test_mktemp_file(enc_path, sizeof(enc_path), "/tmp/rkvc_enc3x_", ".mp4") != 0)
        fail();
    if (test_mktemp_file(dec_path, sizeof(dec_path), "/tmp/rkvc_dec3x_", ".nv12") != 0) {
        remove(enc_path);
        fail();
    }

    rkvc_pipeline_desc enc = {0};
    rkvc_pipeline_from_template(RKVC_TEMPLATE_FILE_ENCODE, &enc);
    enc.input_path       = raw;
    enc.output_path      = enc_path;
    enc.policy           = RKVC_POLICY_REALTIME;
    enc.width            = 1920;
    enc.height           = 1080;
    enc.enc_scale_denom  = 3;

    rkvc_session *s = NULL;
    assert_int_equal(rkvc_session_create(&enc, &s), RKVC_OK);
    rkvc_err err = rkvc_session_run_file(s);
    rkvc_session_destroy(s);
    assert_int_equal(err, RKVC_OK);

    rkvc_pipeline_desc dec = {0};
    rkvc_pipeline_from_template(RKVC_TEMPLATE_FILE_DECODE, &dec);
    dec.input_path        = enc_path;
    dec.output_path       = dec_path;
    dec.width             = 1920;
    dec.height            = 1080;
    dec.enc_scale_denom   = 3;
    dec.post_upscale_algo = RKVC_UPSCALE_BILINEAR;

    assert_int_equal(rkvc_session_create(&dec, &s), RKVC_OK);
    err = rkvc_session_run_file(s);
    rkvc_session_destroy(s);
    assert_int_equal(err, RKVC_OK);

    struct stat st;
    assert_int_equal(stat(dec_path, &st), 0);
    assert_true((size_t)st.st_size >= (size_t)1920 * 1080 * 3 / 2);

    remove(enc_path);
    remove(dec_path);
}

static void test_session_encode_decode_upscale_3x_ai_sr(void **state)
{
    (void)state;
    skip_unless_hw();

    rkvc_caps caps;
    rkvc_query_caps(&caps);
    if (!caps.has_rga || !caps.has_rknn)
        skip();

    char model_buf[PATH_MAX];
    const char *model = resolve_sr_model_path(model_buf, sizeof(model_buf));
    if (!model)
        skip();

    const char *raw = getenv("RKVC_TEST_RAW_NV12");
    if (!raw || !raw[0])
        raw = rkvc_test_fixture_nv12_1080p();
    if (!raw)
        fail();

    char enc_path[PATH_MAX];
    char dec_path[PATH_MAX];
    if (test_mktemp_file(enc_path, sizeof(enc_path), "/tmp/rkvc_enc_ai_", ".mp4") != 0)
        fail();
    if (test_mktemp_file(dec_path, sizeof(dec_path), "/tmp/rkvc_dec_ai_", ".nv12") != 0) {
        remove(enc_path);
        fail();
    }

    rkvc_pipeline_desc enc = {0};
    rkvc_pipeline_from_template(RKVC_TEMPLATE_FILE_ENCODE, &enc);
    enc.input_path       = raw;
    enc.output_path      = enc_path;
    enc.policy           = RKVC_POLICY_REALTIME;
    enc.width            = 1920;
    enc.height           = 1080;
    enc.enc_scale_denom  = 3;

    rkvc_session *s = NULL;
    assert_int_equal(rkvc_session_create(&enc, &s), RKVC_OK);
    rkvc_err err = rkvc_session_run_file(s);
    rkvc_session_destroy(s);
    assert_int_equal(err, RKVC_OK);

    rkvc_pipeline_desc dec = {0};
    rkvc_pipeline_from_template(RKVC_TEMPLATE_FILE_DECODE, &dec);
    dec.input_path                   = enc_path;
    dec.output_path                  = dec_path;
    dec.width                        = 1920;
    dec.height                       = 1080;
    dec.enc_scale_denom              = 3;
    dec.post_upscale_algo            = RKVC_UPSCALE_AI_SR;
    dec.post_upscale_rkvc_model_path = model;

    assert_int_equal(rkvc_session_create(&dec, &s), RKVC_OK);
    err = rkvc_session_run_file(s);
    rkvc_session_destroy(s);
    assert_int_equal(err, RKVC_OK);

    struct stat st;
    assert_int_equal(stat(dec_path, &st), 0);
    assert_true((size_t)st.st_size >= (size_t)1920 * 1080 * 3 / 2);

    remove(enc_path);
    remove(dec_path);
}

static const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_session_transcode_h264),
    cmocka_unit_test(test_session_transcode_balanced),
    cmocka_unit_test(test_session_transcode_quality),
    cmocka_unit_test(test_session_transcode_offline),
    cmocka_unit_test(test_session_decode_nv12),
    cmocka_unit_test(test_session_encode_h264),
    cmocka_unit_test(test_session_av1_storage),
    cmocka_unit_test(test_session_encode_decode_upscale_3x),
    cmocka_unit_test(test_session_encode_decode_upscale_3x_ai_sr),
};

int main(int argc, char **argv)
{
    if (!rkvc_test_hardware_opted_in())
        return RKVC_CTEST_SKIP;

    if (argc > 1) {
        for (size_t i = 0; tests[i].test_func != NULL; i++) {
            if (strcmp(argv[1], tests[i].name) == 0) {
                const struct CMUnitTest one[] = {
                    tests[i],
                    {0},
                };
                return cmocka_run_group_tests(one, NULL, NULL);
            }
        }
        fprintf(stderr, "unknown case: %s\n", argv[1]);
        return 1;
    }

    return cmocka_run_group_tests(tests, NULL, NULL);
}
