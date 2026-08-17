/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file test_net.c
 * @brief UDP/RTP 本机回环单元测试。
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <arpa/inet.h>
#include <endian.h>
#include <netinet/in.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "rkvc/rkvc.h"

typedef struct {
    rkvc_net_mode mode;
    int           port;
    int           nframes;
    int           ok;
} loop_ctx;

static void *recv_thread(void *arg)
{
    loop_ctx *c = arg;
    rkvc_net_config cfg = rkvc_net_config_defaults();
    cfg.mode = c->mode;
    cfg.bind_port = c->port;
    cfg.timeout_ms = 2000;

    rkvc_net *rx = NULL;
    if (rkvc_net_open(&rx, &cfg) != RKVC_OK) {
        c->ok = 0;
        return NULL;
    }

    int got = 0;
    int again_streak = 0;
    for (;;) {
        rkvc_buffer *pkt = NULL;
        rkvc_err err = rkvc_net_recv(rx, &pkt, 2000);
        if (err == RKVC_ERR_EOF)
            break;
        if (err == RKVC_ERR_AGAIN) {
            if (++again_streak > 5) {
                c->ok = 0;
                rkvc_net_close(rx);
                return NULL;
            }
            continue;
        }
        again_streak = 0;
        if (err != RKVC_OK) {
            c->ok = 0;
            rkvc_net_close(rx);
            return NULL;
        }
        rkvc_buffer_bitstream_view v;
        assert_int_equal(rkvc_buffer_get_bitstream(pkt, &v), RKVC_OK);
        assert_true(v.size >= 4);
        assert_int_equal(v.data[0], 0x00);
        assert_int_equal(v.data[1], 0x00);
        assert_int_equal(v.data[2], 0x00);
        assert_int_equal(v.data[3], 0x01);
        rkvc_buffer_unref(pkt);
        got++;
    }
    rkvc_net_stats st;
    rkvc_net_get_stats(rx, &st);
    c->ok = (got == c->nframes && (int)st.pkts_recv == c->nframes) ? 1 : 0;
    rkvc_net_close(rx);
    return NULL;
}

static void loopback(rkvc_net_mode mode, int port)
{
    loop_ctx c = { .mode = mode, .port = port, .nframes = 5, .ok = 0 };
    pthread_t th;
    assert_int_equal(pthread_create(&th, NULL, recv_thread, &c), 0);
    usleep(50000);

    rkvc_net_config cfg = rkvc_net_config_defaults();
    cfg.mode = mode;
    cfg.peer_ip = "127.0.0.1";
    cfg.peer_port = port;
    rkvc_net *tx = NULL;
    assert_int_equal(rkvc_net_open(&tx, &cfg), RKVC_OK);

    uint8_t payload[64];
    memset(payload, 0xAB, sizeof(payload));
    payload[0] = 0x00;
    payload[1] = 0x00;
    payload[2] = 0x00;
    payload[3] = 0x01;

    for (int i = 0; i < c.nframes; i++) {
        assert_int_equal(rkvc_net_send(tx, payload, sizeof(payload), i, i == 0),
                         RKVC_OK);
    }
    assert_int_equal(rkvc_net_finish(tx), RKVC_OK);
    rkvc_net_close(tx);

    pthread_join(th, NULL);
    assert_int_equal(c.ok, 1);
}

static void test_udp_loopback(void **state)
{
    (void)state;
    loopback(RKVC_NET_UDP, 19001);
}

static void test_rtp_loopback(void **state)
{
    (void)state;
    loopback(RKVC_NET_RTP, 19002);
}

static void test_udp_rejects_oversized_frame_len(void **state)
{
    (void)state;
    rkvc_net_config rxcfg = rkvc_net_config_defaults();
    rxcfg.bind_port = 19003;
    rxcfg.timeout_ms = 200;
    rkvc_net *rx = NULL;
    assert_int_equal(rkvc_net_open(&rx, &rxcfg), RKVC_OK);

    rkvc_net_config txcfg = rkvc_net_config_defaults();
    txcfg.peer_ip = "127.0.0.1";
    txcfg.peer_port = 19003;
    rkvc_net *tx = NULL;
    assert_int_equal(rkvc_net_open(&tx, &txcfg), RKVC_OK);

    /* 伪造超大 frame_len（0xffffffff），接收端应忽略而非 OOM */
    uint8_t hdr[16];
    uint16_t frag_id = htons(0);
    uint16_t frag_tot = htons(1);
    uint32_t frame_len = htonl(0xFFFFFFFFu);
    uint64_t pts = htobe64(1);
    memcpy(hdr, &frag_id, 2);
    memcpy(hdr + 2, &frag_tot, 2);
    memcpy(hdr + 4, &frame_len, 4);
    memcpy(hdr + 8, &pts, 8);
    uint8_t pkt[20];
    memcpy(pkt, hdr, 16);
    memset(pkt + 16, 0xAA, 4);

    /* 直接 sendto 绕过 API 校验 */
    {
        struct sockaddr_in peer;
        memset(&peer, 0, sizeof(peer));
        peer.sin_family = AF_INET;
        peer.sin_port = htons(19003);
        inet_pton(AF_INET, "127.0.0.1", &peer.sin_addr);
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        assert_true(fd >= 0);
        assert_true(sendto(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&peer,
                           sizeof(peer)) > 0);
        close(fd);
    }

    rkvc_buffer *out = NULL;
    rkvc_err err = rkvc_net_recv(rx, &out, 300);
    assert_int_equal(err, RKVC_ERR_AGAIN);
    assert_null(out);

    rkvc_net_close(tx);
    rkvc_net_close(rx);
}

static void test_rtp_rejects_oversized_frame(void **state)
{
    (void)state;
    rkvc_net_config cfg = rkvc_net_config_defaults();
    cfg.mode = RKVC_NET_RTP;
    cfg.peer_ip = "127.0.0.1";
    cfg.peer_port = 19005;
    rkvc_net *tx = NULL;
    assert_int_equal(rkvc_net_open(&tx, &cfg), RKVC_OK);

    const uint8_t byte = 0;
    const size_t oversized = 16u * 65491u + 1u;
    assert_int_equal(rkvc_net_send(tx, &byte, oversized, 0, 0),
                     RKVC_ERR_INVALID);

    rkvc_net_close(tx);
}

static void test_udp_finish_signal(void **state)
{
    (void)state;
    rkvc_net_config rxcfg = rkvc_net_config_defaults();
    rxcfg.bind_port = 19004;
    rxcfg.timeout_ms = 1000;
    rkvc_net *rx = NULL;
    assert_int_equal(rkvc_net_open(&rx, &rxcfg), RKVC_OK);

    rkvc_net_config txcfg = rkvc_net_config_defaults();
    txcfg.peer_ip = "127.0.0.1";
    txcfg.peer_port = 19004;
    rkvc_net *tx = NULL;
    assert_int_equal(rkvc_net_open(&tx, &txcfg), RKVC_OK);

    usleep(20000);
    assert_int_equal(rkvc_net_finish(tx), RKVC_OK);

    rkvc_buffer *out = NULL;
    rkvc_err err = rkvc_net_recv(rx, &out, 1000);
    assert_int_equal(err, RKVC_ERR_EOF);
    assert_null(out);

    rkvc_net_close(tx);
    rkvc_net_close(rx);
}

static void test_net_config_invalid(void **state)
{
    (void)state;
    rkvc_net_config cfg = rkvc_net_config_defaults();
    rkvc_net *n = NULL;
    assert_int_equal(rkvc_net_open(&n, &cfg), RKVC_ERR_INVALID);
    assert_null(n);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_net_config_invalid),
        cmocka_unit_test(test_udp_loopback),
        cmocka_unit_test(test_rtp_loopback),
        cmocka_unit_test(test_udp_rejects_oversized_frame_len),
        cmocka_unit_test(test_rtp_rejects_oversized_frame),
        cmocka_unit_test(test_udp_finish_signal),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
