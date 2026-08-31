/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file test_internal.c
 * @brief 内部映射与 buffer 包装测试。
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include "internal.h"

static void test_averror_mapping(void **state)
{
    (void)state;
    assert_int_equal(rkvc_from_averror(AVERROR(ENOMEM)), RKVC_ERR_NOMEM);
    assert_int_equal(rkvc_from_averror(AVERROR(EAGAIN)), RKVC_ERR_AGAIN);
    assert_int_equal(rkvc_from_averror(AVERROR_EOF), RKVC_ERR_EOF);
}

static void test_pix_fmt_roundtrip(void **state)
{
    (void)state;
    assert_int_equal(rkvc_to_av_pix_fmt(RKVC_PIX_FMT_NV12), AV_PIX_FMT_NV12);
    assert_int_equal(rkvc_from_av_pix_fmt(AV_PIX_FMT_NV12), RKVC_PIX_FMT_NV12);
}

static void test_buffer_wrap_avframe(void **state)
{
    (void)state;
    AVFrame *av = av_frame_alloc();
    av->width = 64;
    av->height = 64;
    av->format = AV_PIX_FMT_NV12;
    assert_int_equal(rkvc_avframe_alloc_contiguous(av), RKVC_OK);

    rkvc_buffer *b = rkvc_buffer_wrap_avframe(av, 1);
    assert_non_null(b);
    rkvc_buffer_video_info info;
    assert_int_equal(rkvc_buffer_get_video_info(b, &info), RKVC_OK);
    assert_int_equal(info.width, 64u);
    rkvc_buffer_unref(b);
}

static void test_port_queue(void **state)
{
    (void)state;
    rkvc_port_queue *q = rkvc_port_queue_create(2);
    rkvc_buffer *b = NULL;
    rkvc_buffer_alloc_video_host(&b, 16, 16, RKVC_PIX_FMT_NV12);
    assert_int_equal(rkvc_port_queue_push(q, b), RKVC_OK);
    rkvc_buffer *out = NULL;
    assert_int_equal(rkvc_port_queue_pull(q, &out, 0), RKVC_OK);
    rkvc_buffer_unref(out);
    rkvc_buffer_unref(b);
    rkvc_port_queue_destroy(q);
}

static void test_port_queue_close_and_reopen(void **state)
{
    (void)state;
    rkvc_port_queue *q = rkvc_port_queue_create(2);
    rkvc_buffer *b = NULL;
    rkvc_buffer_alloc_video_host(&b, 16, 16, RKVC_PIX_FMT_NV12);
    assert_int_equal(rkvc_port_queue_push(q, b), RKVC_OK);
    rkvc_port_queue_close(q);
    assert_int_equal(rkvc_port_queue_push(q, b), RKVC_ERR_EOF);

    rkvc_buffer *out = NULL;
    assert_int_equal(rkvc_port_queue_pull(q, &out, 0), RKVC_OK);
    rkvc_buffer_unref(out);
    assert_int_equal(rkvc_port_queue_pull(q, &out, -1), RKVC_ERR_EOF);

    rkvc_port_queue_reopen(q);
    assert_int_equal(rkvc_port_queue_push(q, b), RKVC_OK);
    assert_int_equal(rkvc_port_queue_pull(q, &out, 0), RKVC_OK);
    rkvc_buffer_unref(out);
    rkvc_buffer_unref(b);
    rkvc_port_queue_destroy(q);
}

static void test_hash_buffer(void **state)
{
    (void)state;
    const uint8_t data[] = "rkvc";
    char hex[65];

    assert_int_equal(rkvc_hash_buffer("sha256", data, sizeof(data) - 1,
                                      hex, sizeof(hex)), RKVC_OK);
    assert_string_equal(hex,
        "d14b3c138a0770f36abd90604e9c3672027d512c268fa4c6c44a001f9e2b2932");
}

static void test_dict_parse_opts(void **state)
{
    (void)state;
    AVDictionary *dict = NULL;

    assert_int_equal(rkvc_dict_parse_opts(&dict, "threads=1:low_delay=1"),
                     RKVC_OK);
    assert_non_null(dict);
    assert_non_null(av_dict_get(dict, "threads", NULL, 0));
    assert_non_null(av_dict_get(dict, "low_delay", NULL, 0));
    rkvc_dict_free(&dict);
}

static void test_now_us_monotonic(void **state)
{
    (void)state;
    int64_t t0 = rkvc_now_us();
    int64_t t1 = rkvc_now_us();
    assert_true(t1 >= t0);
}

typedef struct {
    rkvc_buffer *packet;
    rkvc_err send_results[4];
    rkvc_err receive_results[4];
    rkvc_err drain_results[4];
    size_t send_result_count;
    size_t receive_result_count;
    size_t drain_result_count;
    int read_calls;
    int send_calls;
    int receive_calls;
    int drain_calls;
    const rkvc_buffer *sent_packets[4];
} decode_pump_fake;

static rkvc_err fake_result(const rkvc_err *results, size_t count,
                            int call, rkvc_err fallback)
{
    return (size_t)call < count ? results[call] : fallback;
}

static rkvc_err fake_read_packet(void *opaque, rkvc_buffer **pkt)
{
    decode_pump_fake *fake = opaque;
    fake->read_calls++;
    if (!fake->packet)
        return RKVC_ERR_EOF;
    *pkt = fake->packet;
    fake->packet = NULL;
    return RKVC_OK;
}

static rkvc_err fake_send_packet(void *opaque, const rkvc_buffer *pkt)
{
    decode_pump_fake *fake = opaque;
    int call = fake->send_calls++;
    if (call < (int)(sizeof(fake->sent_packets) /
                     sizeof(fake->sent_packets[0])))
        fake->sent_packets[call] = pkt;
    return fake_result(fake->send_results, fake->send_result_count, call,
                       RKVC_OK);
}

static rkvc_err fake_receive_frame(void *opaque, rkvc_buffer **frame)
{
    decode_pump_fake *fake = opaque;
    int call = fake->receive_calls++;
    rkvc_err err = fake_result(fake->receive_results,
                               fake->receive_result_count, call,
                               RKVC_ERR_AGAIN);
    if (err == RKVC_OK)
        assert_int_equal(rkvc_buffer_alloc_video_host(
                             frame, 16, 16, RKVC_PIX_FMT_NV12),
                         RKVC_OK);
    return err;
}

static rkvc_err fake_drain(void *opaque)
{
    decode_pump_fake *fake = opaque;
    int call = fake->drain_calls++;
    return fake_result(fake->drain_results, fake->drain_result_count, call,
                       RKVC_OK);
}

static const rkvc_decode_pump_ops fake_decode_pump_ops = {
    .read_packet = fake_read_packet,
    .send_packet = fake_send_packet,
    .receive_frame = fake_receive_frame,
    .drain = fake_drain,
};

static rkvc_buffer *alloc_test_packet(void)
{
    static const uint8_t data[] = {0x00, 0x00, 0x01, 0x65};
    rkvc_buffer *packet = NULL;
    assert_int_equal(rkvc_buffer_alloc_bitstream(
                         &packet, data, sizeof(data), 1),
                     RKVC_OK);
    return packet;
}

static void test_decode_pump_retries_same_packet(void **state)
{
    (void)state;
    decode_pump_fake fake = {
        .packet = alloc_test_packet(),
        .send_results = {RKVC_ERR_AGAIN, RKVC_OK},
        .receive_results = {RKVC_OK, RKVC_ERR_AGAIN, RKVC_ERR_EOF},
        .send_result_count = 2,
        .receive_result_count = 3,
    };
    rkvc_buffer *packet_guard = rkvc_buffer_ref(fake.packet);
    rkvc_decode_pump pump = {0};
    rkvc_buffer *frame = NULL;

    assert_int_equal(rkvc_decode_pump_next(
                         &pump, &fake_decode_pump_ops, &fake, &frame),
                     RKVC_OK);
    assert_non_null(frame);
    rkvc_buffer_unref(frame);
    assert_ptr_equal(pump.pending_pkt, packet_guard);
    assert_int_equal(fake.read_calls, 1);
    assert_int_equal(fake.send_calls, 1);

    frame = NULL;
    assert_int_equal(rkvc_decode_pump_next(
                         &pump, &fake_decode_pump_ops, &fake, &frame),
                     RKVC_ERR_AGAIN);
    assert_null(frame);
    assert_null(pump.pending_pkt);
    assert_int_equal(fake.read_calls, 1);
    assert_int_equal(fake.send_calls, 2);
    assert_ptr_equal(fake.sent_packets[0], fake.sent_packets[1]);

    assert_int_equal(rkvc_decode_pump_next(
                         &pump, &fake_decode_pump_ops, &fake, &frame),
                     RKVC_ERR_EOF);
    assert_int_equal(fake.read_calls, 2);
    assert_int_equal(fake.drain_calls, 1);

    rkvc_decode_pump_cleanup(&pump);
    rkvc_buffer_unref(packet_guard);
}

static void test_decode_pump_waits_on_double_again(void **state)
{
    (void)state;
    decode_pump_fake fake = {
        .packet = alloc_test_packet(),
        .send_results = {RKVC_ERR_AGAIN},
        .receive_results = {RKVC_ERR_AGAIN},
        .send_result_count = 1,
        .receive_result_count = 1,
    };
    rkvc_buffer *packet_guard = rkvc_buffer_ref(fake.packet);
    rkvc_decode_pump pump = {0};
    rkvc_buffer *frame = NULL;

    assert_int_equal(rkvc_decode_pump_next(
                         &pump, &fake_decode_pump_ops, &fake, &frame),
                     RKVC_ERR_AGAIN);
    assert_ptr_equal(pump.pending_pkt, packet_guard);
    rkvc_decode_pump_cleanup(&pump);
    assert_null(pump.pending_pkt);
    assert_int_equal(packet_guard->ref_count, 1);
    rkvc_buffer_unref(packet_guard);
}

static void test_decode_pump_retries_drain(void **state)
{
    (void)state;
    decode_pump_fake fake = {
        .receive_results = {RKVC_OK, RKVC_ERR_EOF},
        .drain_results = {RKVC_ERR_AGAIN, RKVC_OK},
        .receive_result_count = 2,
        .drain_result_count = 2,
    };
    rkvc_decode_pump pump = {.input_eof = 1};
    rkvc_buffer *frame = NULL;

    assert_int_equal(rkvc_decode_pump_next(
                         &pump, &fake_decode_pump_ops, &fake, &frame),
                     RKVC_OK);
    assert_non_null(frame);
    rkvc_buffer_unref(frame);
    assert_int_equal(fake.drain_calls, 1);
    assert_false(pump.drain_sent);

    frame = NULL;
    assert_int_equal(rkvc_decode_pump_next(
                         &pump, &fake_decode_pump_ops, &fake, &frame),
                     RKVC_ERR_EOF);
    assert_int_equal(fake.drain_calls, 2);
    assert_true(pump.drain_sent);
    rkvc_decode_pump_cleanup(&pump);
}

static void test_bitstream_has_padding(void **state)
{
    (void)state;
    const uint8_t data[] = {0x00, 0x00, 0x00, 0x01, 0x67};
    rkvc_buffer *b = NULL;
    assert_int_equal(rkvc_buffer_alloc_bitstream(&b, data, sizeof(data), 1),
                     RKVC_OK);
    assert_non_null(b);
    assert_non_null(b->data);
    assert_int_equal(b->size, sizeof(data));
    for (int i = 0; i < AV_INPUT_BUFFER_PADDING_SIZE; i++)
        assert_int_equal(b->data[b->size + (size_t)i], 0);
    rkvc_buffer_unref(b);
}

static void test_rknn_sr_available_agrees_with_caps(void **state)
{
    (void)state;
    rkvc_caps caps;
    assert_int_equal(rkvc_query_caps(&caps), RKVC_OK);
    assert_int_equal(!!caps.has_rknn, !!rkvc_rknn_sr_available());
}

int main(void)
{
    rkvc_init();

    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_averror_mapping),
        cmocka_unit_test(test_pix_fmt_roundtrip),
        cmocka_unit_test(test_buffer_wrap_avframe),
        cmocka_unit_test(test_port_queue),
        cmocka_unit_test(test_port_queue_close_and_reopen),
        cmocka_unit_test(test_hash_buffer),
        cmocka_unit_test(test_dict_parse_opts),
        cmocka_unit_test(test_now_us_monotonic),
        cmocka_unit_test(test_decode_pump_retries_same_packet),
        cmocka_unit_test(test_decode_pump_waits_on_double_again),
        cmocka_unit_test(test_decode_pump_retries_drain),
        cmocka_unit_test(test_bitstream_has_padding),
        cmocka_unit_test(test_rknn_sr_available_agrees_with_caps),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
