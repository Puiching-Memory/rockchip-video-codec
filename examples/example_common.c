/* SPDX-License-Identifier: AGPL-3.0-or-later */

#include "example_common.h"

#include <pthread.h>
#include <sched.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_diag(const rkvc_diag *diag) {
    char text[1024];
    if (!diag)
        return;
    rkvc_diag_fmt_text(diag, text, sizeof(text));
    if (text[0])
        fprintf(stderr, "rkvc: %s\n", text);
}

int example_run_file(rkvc_operation operation, rkvc_codec codec,
                     const char *input, const char *output,
                     uint32_t width, uint32_t height, int32_t bitrate) {
    rkvc_context *ctx = NULL;
    rkvc_job *job = NULL;
    rkvc_diag *diag = NULL;
    rkvc_request req;
    rkvc_status st;

    st = rkvc_context_create(NULL, &ctx);
    if (st != RKVC_STATUS_OK)
        goto done;
    rkvc_request_init(&req, sizeof(req));
    req.operation = operation;
    req.codec = codec;
    req.policy = RKVC_POLICY_REALTIME;
    req.input.kind = RKVC_ENDPOINT_FILE;
    req.input.uri = input;
    req.output.kind = RKVC_ENDPOINT_FILE;
    req.output.uri = output;
    req.width = width;
    req.height = height;
    req.quality.bitrate_bps = bitrate;
    req.quality.qp = -1;
    st = rkvc_job_create(ctx, &req, &diag, &job);
    if (st == RKVC_STATUS_OK)
        st = rkvc_job_start(job, &diag);
    if (st == RKVC_STATUS_OK)
        st = rkvc_job_wait(job);

done:
    if (st != RKVC_STATUS_OK) {
        fprintf(stderr, "rkvc example failed: %s\n", rkvc_status_str(st));
        print_diag(diag);
    }
    rkvc_job_destroy(job);
    rkvc_diag_release(diag);
    rkvc_context_destroy(ctx);
    return st == RKVC_STATUS_OK ? 0 : 1;
}

struct output_consumer {
    rkvc_job *job;
    FILE *fp;
    rkvc_status status;
    uint64_t packets;
};

static void *consume_output(void *opaque) {
    struct output_consumer *consumer = opaque;
    consumer->status = RKVC_STATUS_OK;
    for (;;) {
        rkvc_frame *frame = NULL;
        rkvc_frame_desc desc;
        rkvc_status st = rkvc_job_pull(consumer->job, &frame);
        if (st == RKVC_STATUS_EOF)
            break;
        if (st != RKVC_STATUS_OK) {
            consumer->status = st;
            break;
        }
        st = rkvc_frame_get_desc(frame, &desc);
        if (st == RKVC_STATUS_OK &&
            fwrite(desc.data, 1, desc.size, consumer->fp) != desc.size)
            st = RKVC_STATUS_IO;
        rkvc_frame_release(frame);
        if (st != RKVC_STATUS_OK) {
            consumer->status = st;
            break;
        }
        consumer->packets++;
    }
    return NULL;
}

static void fill_nv12(unsigned char *data, uint32_t width, uint32_t height,
                      uint32_t sequence) {
    size_t y_size = (size_t)width * height;
    uint32_t y;
    for (y = 0; y < height; ++y)
        memset(data + (size_t)y * width,
               (unsigned char)(16u + (sequence * 3u + y) % 200u), width);
    memset(data + y_size, 128, y_size / 2);
}

int example_stream_encode(const char *output, rkvc_codec codec,
                          uint32_t width, uint32_t height, uint32_t frames,
                          int with_roi, int adaptive, int udp_loopback) {
    rkvc_context *ctx = NULL;
    rkvc_job *job = NULL;
    rkvc_diag *diag = NULL;
    rkvc_request req;
    rkvc_status st = RKVC_STATUS_OK;
    struct output_consumer consumer = {0};
    pthread_t consumer_thread;
    unsigned char **payloads = NULL;
    FILE *fp = NULL;
    uint32_t i, pushed = 0;
    int thread_started = 0;
    size_t frame_size;
    int send_fd = -1, recv_fd = -1;
    struct sockaddr_in loop_addr;

    if (!width || !height || !frames || (width & 1u) || (height & 1u))
        return 2;
    frame_size = (size_t)width * height * 3 / 2;
    memset(&loop_addr, 0, sizeof(loop_addr));
    if (udp_loopback) {
        socklen_t addr_len = sizeof(loop_addr);
        recv_fd = socket(AF_INET, SOCK_DGRAM, 0);
        send_fd = socket(AF_INET, SOCK_DGRAM, 0);
        loop_addr.sin_family = AF_INET;
        loop_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        loop_addr.sin_port = 0;
        if (recv_fd < 0 || send_fd < 0 ||
            bind(recv_fd, (const struct sockaddr *)&loop_addr,
                 sizeof(loop_addr)) != 0 ||
            getsockname(recv_fd, (struct sockaddr *)&loop_addr,
                        &addr_len) != 0) {
            st = RKVC_STATUS_IO;
            goto done;
        }
    }
    payloads = calloc(frames, sizeof(*payloads));
    fp = fopen(output, "wb");
    if (!payloads || !fp) {
        st = RKVC_STATUS_IO;
        goto done;
    }
    st = rkvc_context_create(NULL, &ctx);
    if (st != RKVC_STATUS_OK)
        goto done;
    rkvc_request_init(&req, sizeof(req));
    req.operation = RKVC_OPERATION_ENCODE;
    req.codec = codec;
    req.policy = RKVC_POLICY_REALTIME;
    req.input.kind = RKVC_ENDPOINT_FRAME_SINK;
    req.input.fmt = RKVC_FRAME_FMT_NV12;
    req.output.kind = RKVC_ENDPOINT_FRAME_SINK;
    req.output.fmt = RKVC_FRAME_FMT_BITSTREAM;
    req.width = width;
    req.height = height;
    req.quality.bitrate_bps = 1200000;
    req.quality.qp = -1;
    st = rkvc_job_create(ctx, &req, &diag, &job);
    if (st == RKVC_STATUS_OK)
        st = rkvc_job_start(job, &diag);
    if (st != RKVC_STATUS_OK)
        goto done;

    consumer.job = job;
    consumer.fp = fp;
    if (pthread_create(&consumer_thread, NULL, consume_output, &consumer) != 0) {
        st = RKVC_STATUS_INTERNAL;
        goto done;
    }
    thread_started = 1;

    for (i = 0; i < frames; ++i) {
        rkvc_frame_desc desc;
        rkvc_frame *frame = NULL;
        rkvc_roi_region regions[2] = {
            {width / 4, height / 4, width / 2, height / 2, -8, 0, 0},
            {0, 0, width / 4, height / 4, 10, 0, 0},
        };
        payloads[i] = malloc(frame_size);
        if (!payloads[i]) {
            st = RKVC_STATUS_NOMEM;
            break;
        }
        fill_nv12(payloads[i], width, height, i);
        if (udp_loopback) {
            ssize_t sent = sendto(send_fd, payloads[i], frame_size, 0,
                                  (const struct sockaddr *)&loop_addr,
                                  sizeof(loop_addr));
            if (sent != (ssize_t)frame_size) {
                st = RKVC_STATUS_IO;
                break;
            }
            memset(payloads[i], 0, frame_size);
            if (recvfrom(recv_fd, payloads[i], frame_size, 0, NULL, NULL) !=
                (ssize_t)frame_size) {
                st = RKVC_STATUS_IO;
                break;
            }
        }
        rkvc_frame_desc_init(&desc, sizeof(desc));
        desc.spec.width = width;
        desc.spec.height = height;
        desc.spec.stride = width;
        desc.spec.ver_stride = height;
        desc.spec.fmt = RKVC_FRAME_FMT_NV12;
        desc.spec.domain = RKVC_MEM_DOMAIN_HOST;
        desc.data = payloads[i];
        desc.size = frame_size;
        desc.pts = (int64_t)i;
        if (with_roi) {
            desc.roi_regions = regions;
            desc.roi_region_count = 2;
            regions[0].force_intra = (uint8_t)(i % 30u == 0);
        }
        if (adaptive && i % 30u == 0) {
            desc.encode.bitrate_bps = (i / 30u) % 2u ? 600000 : 1600000;
            desc.encode.gop_size = 30;
            desc.encode.force_idr = 1;
        }
        st = rkvc_frame_wrap(&desc, &frame);
        if (st != RKVC_STATUS_OK)
            break;
        do {
            st = rkvc_job_push(job, frame);
            if (st == RKVC_STATUS_AGAIN)
                sched_yield();
        } while (st == RKVC_STATUS_AGAIN);
        if (st != RKVC_STATUS_OK) {
            rkvc_frame_release(frame);
            break;
        }
        pushed++;
    }
    if (st == RKVC_STATUS_OK)
        st = rkvc_job_push_eos(job);
    else
        (void)rkvc_job_cancel(job);
    if (thread_started) {
        pthread_join(consumer_thread, NULL);
        thread_started = 0;
        if (st == RKVC_STATUS_OK)
            st = consumer.status;
    }
    if (st == RKVC_STATUS_OK)
        st = rkvc_job_wait(job);

done:
    if (thread_started) {
        rkvc_job_cancel(job);
        pthread_join(consumer_thread, NULL);
    }
    if (st != RKVC_STATUS_OK) {
        fprintf(stderr, "rkvc streaming example failed: %s\n",
                rkvc_status_str(st));
        print_diag(diag);
    } else {
        printf("encoded %u frames into %llu packets: %s\n", pushed,
               (unsigned long long)consumer.packets, output);
    }
    rkvc_job_destroy(job);
    rkvc_diag_release(diag);
    rkvc_context_destroy(ctx);
    if (send_fd >= 0)
        close(send_fd);
    if (recv_fd >= 0)
        close(recv_fd);
    if (fp)
        fclose(fp);
    for (i = 0; payloads && i < frames; ++i)
        free(payloads[i]);
    free(payloads);
    return st == RKVC_STATUS_OK ? 0 : 1;
}

int example_stream_transcode(const char *input, const char *output,
                             rkvc_codec target_codec) {
    rkvc_context *ctx = NULL;
    rkvc_job *job = NULL;
    rkvc_diag *diag = NULL;
    rkvc_request req;
    rkvc_frame_desc desc;
    rkvc_frame *frame = NULL;
    rkvc_status st = RKVC_STATUS_OK;
    struct output_consumer consumer = {0};
    pthread_t consumer_thread;
    FILE *in = NULL, *out = NULL;
    unsigned char *payload = NULL;
    long file_size;
    int thread_started = 0;

    in = fopen(input, "rb");
    out = fopen(output, "wb");
    if (!in || !out || fseek(in, 0, SEEK_END) != 0 ||
        (file_size = ftell(in)) <= 0 || fseek(in, 0, SEEK_SET) != 0) {
        st = RKVC_STATUS_IO;
        goto done;
    }
    payload = malloc((size_t)file_size);
    if (!payload) {
        st = RKVC_STATUS_NOMEM;
        goto done;
    }
    if (fread(payload, 1, (size_t)file_size, in) != (size_t)file_size) {
        st = RKVC_STATUS_IO;
        goto done;
    }
    st = rkvc_context_create(NULL, &ctx);
    if (st != RKVC_STATUS_OK)
        goto done;
    rkvc_request_init(&req, sizeof(req));
    req.operation = RKVC_OPERATION_TRANSCODE;
    req.codec = target_codec;
    req.policy = RKVC_POLICY_REALTIME;
    req.input.kind = RKVC_ENDPOINT_FRAME_SINK;
    req.input.fmt = RKVC_FRAME_FMT_BITSTREAM;
    req.output.kind = RKVC_ENDPOINT_FRAME_SINK;
    req.output.fmt = RKVC_FRAME_FMT_BITSTREAM;
    req.quality.bitrate_bps = 2000000;
    st = rkvc_job_create(ctx, &req, &diag, &job);
    if (st == RKVC_STATUS_OK)
        st = rkvc_job_start(job, &diag);
    if (st != RKVC_STATUS_OK)
        goto done;
    consumer.job = job;
    consumer.fp = out;
    if (pthread_create(&consumer_thread, NULL, consume_output, &consumer) != 0) {
        st = RKVC_STATUS_INTERNAL;
        goto done;
    }
    thread_started = 1;
    rkvc_frame_desc_init(&desc, sizeof(desc));
    desc.spec.fmt = RKVC_FRAME_FMT_BITSTREAM;
    desc.spec.domain = RKVC_MEM_DOMAIN_HOST;
    desc.data = payload;
    desc.size = (size_t)file_size;
    st = rkvc_frame_wrap(&desc, &frame);
    if (st == RKVC_STATUS_OK)
        st = rkvc_job_push(job, frame);
    if (st != RKVC_STATUS_OK && frame)
        rkvc_frame_release(frame);
    if (st == RKVC_STATUS_OK)
        st = rkvc_job_push_eos(job);
    else
        (void)rkvc_job_cancel(job);
    pthread_join(consumer_thread, NULL);
    thread_started = 0;
    if (st == RKVC_STATUS_OK)
        st = consumer.status;
    if (st == RKVC_STATUS_OK)
        st = rkvc_job_wait(job);

done:
    if (thread_started) {
        rkvc_job_cancel(job);
        pthread_join(consumer_thread, NULL);
    }
    if (st != RKVC_STATUS_OK) {
        fprintf(stderr, "rkvc port transcode failed: %s\n",
                rkvc_status_str(st));
        print_diag(diag);
    }
    rkvc_job_destroy(job);
    rkvc_diag_release(diag);
    rkvc_context_destroy(ctx);
    free(payload);
    if (in)
        fclose(in);
    if (out)
        fclose(out);
    return st == RKVC_STATUS_OK ? 0 : 1;
}
