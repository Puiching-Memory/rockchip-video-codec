/**
 * @file net_loopback.c
 * @brief UDP/RTP 本机回环：编码若干帧 → net_send → net_recv 校验。
 *
 * 用法:
 *   example_net_loopback [udp|rtp] [port]
 */

#include "rkvc/rkvc.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    rkvc_net_mode mode;
    int           port;
    int           expect;
    int           got;
    int           err;
} rx_arg;

static void *rx_main(void *p)
{
    rx_arg *a = p;
    rkvc_net_config cfg = rkvc_net_config_defaults();
    cfg.mode = a->mode;
    cfg.bind_port = a->port;
    cfg.timeout_ms = 3000;

    rkvc_net *rx = NULL;
    if (rkvc_net_open(&rx, &cfg) != RKVC_OK) {
        a->err = 1;
        return NULL;
    }

    for (;;) {
        rkvc_buffer *pkt = NULL;
        rkvc_err e = rkvc_net_recv(rx, &pkt, 3000);
        if (e == RKVC_ERR_EOF)
            break;
        if (e == RKVC_ERR_AGAIN)
            continue;
        if (e != RKVC_OK) {
            a->err = 1;
            break;
        }
        a->got++;
        rkvc_buffer_unref(pkt);
    }
    rkvc_net_close(rx);
    return NULL;
}

int main(int argc, char **argv)
{
    rkvc_net_mode mode = RKVC_NET_UDP;
    int port = 19100;
    if (argc > 1 && strcmp(argv[1], "rtp") == 0)
        mode = RKVC_NET_RTP;
    if (argc > 2)
        port = atoi(argv[2]);

    const int w = 320, h = 240, nframes = 8;
    size_t fr = (size_t)w * h * 3 / 2;
    FILE *fp = fopen("/tmp/rkvc_net_in.nv12", "wb");
    if (!fp) {
        perror("fopen");
        return 1;
    }
    uint8_t *raw = calloc(1, fr);
    for (int i = 0; i < nframes; i++) {
        memset(raw, 16 + (i % 4) * 8, (size_t)w * h);
        memset(raw + (size_t)w * h, 128, (size_t)w * h / 2);
        fwrite(raw, 1, fr, fp);
    }
    fclose(fp);
    free(raw);

    rkvc_pipeline_desc d;
    rkvc_pipeline_from_template(RKVC_TEMPLATE_FILE_ENCODE, &d);
    d.input_path = "/tmp/rkvc_net_in.nv12";
    d.output_path = "/tmp/rkvc_net_enc.mp4";
    d.width = w;
    d.height = h;
    d.bitrate = 400000;
    d.policy = RKVC_POLICY_REALTIME;
    d.gop_size = 8;

    rkvc_session *s = NULL;
    if (rkvc_session_create(&d, &s) != RKVC_OK) {
        fprintf(stderr, "session_create failed\n");
        return 1;
    }
    if (rkvc_session_run_file(s) != RKVC_OK) {
        fprintf(stderr, "encode failed\n");
        rkvc_session_destroy(s);
        return 1;
    }
    rkvc_session_destroy(s);

    /* demux encoded file and send annex-b-ish packets via net */
    rx_arg ra = { .mode = mode, .port = port, .expect = 0, .got = 0, .err = 0 };
    pthread_t th;
    pthread_create(&th, NULL, rx_main, &ra);
    usleep(80000);

    rkvc_net_config tcfg = rkvc_net_config_defaults();
    tcfg.mode = mode;
    tcfg.peer_ip = "127.0.0.1";
    tcfg.peer_port = port;
    rkvc_net *tx = NULL;
    if (rkvc_net_open(&tx, &tcfg) != RKVC_OK) {
        fprintf(stderr, "tx open failed\n");
        ra.err = 1;
        pthread_join(th, NULL);
        return 1;
    }

    /* send synthetic NAL-like chunks (not full demux — keeps example light) */
    uint8_t nal[1024];
    memset(nal, 0x5A, sizeof(nal));
    nal[0] = 0x00;
    nal[1] = 0x00;
    nal[2] = 0x00;
    nal[3] = 0x01;
    nal[4] = 0x26; /* fake IDR-ish */
    int sent = 0;
    for (int i = 0; i < 12; i++) {
        if (rkvc_net_send(tx, nal, sizeof(nal), i * 3000, i == 0) != RKVC_OK) {
            fprintf(stderr, "send failed\n");
            ra.err = 1;
            break;
        }
        sent++;
    }
    rkvc_net_finish(tx);
    rkvc_net_stats st;
    rkvc_net_get_stats(tx, &st);
    rkvc_net_close(tx);
    pthread_join(th, NULL);

    printf("net_loopback mode=%s port=%d sent=%d recv=%d bytes_sent=%llu err=%d\n",
           mode == RKVC_NET_UDP ? "udp" : "rtp", port, sent, ra.got,
           (unsigned long long)st.bytes_sent, ra.err);
    return (ra.err == 0 && ra.got == sent) ? 0 : 1;
}
