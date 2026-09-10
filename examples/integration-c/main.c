/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Minimal embedder contract sample: FrameSink AV1 encode via the C ABI. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rkvc/rkvc.h"

static void fail(const char *what, rkvc_status st, rkvc_diagnostic *d) {
    fprintf(stderr, "intc: %s failed: %s\n", what, rkvc_status_str(st));
    if (d) {
        char buf[1024];
        rkvc_diag_fmt_text(d, buf, sizeof(buf));
        fprintf(stderr, "%s", buf);
        rkvc_diag_release(d);
    }
    exit(2);
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s <plugin.so> <width> <height> <frames>\n",
                argv[0]);
        return 1;
    }
    uint32_t width = (uint32_t)atoi(argv[2]);
    uint32_t height = (uint32_t)atoi(argv[3]);
    int frames = atoi(argv[4]);
    if (!width || !height || frames <= 0 || (width & 1u) || (height & 1u)) {
        fprintf(stderr, "intc: need even W/H and frames > 0\n");
        return 1;
    }

    rkvc_context_options opts;
    rkvc_context_options_init(&opts, sizeof(opts));
    /* backend_dirs takes directories; the plugin file's directory works
     * because discovery scans it for *.so. */
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", argv[1]);
    char *slash = strrchr(dir, '/');
    const char *dirs[1];
    if (slash) {
        *slash = '\0';
        dirs[0] = dir;
        opts.backend_dirs = dirs;
        opts.backend_dir_count = 1;
    }
    rkvc_context *ctx = NULL;
    if (rkvc_context_create(&opts, &ctx) != RKVC_OK) {
        fprintf(stderr, "intc: context create failed\n");
        return 2;
    }

    rkvc_session_request req;
    rkvc_session_request_init(&req, sizeof(req));
    req.operation = RKVC_OP_ENCODE;
    req.codec = RKVC_CODEC_AV1;
    req.input.kind = RKVC_ENDPOINT_FRAME_SINK;
    req.input.fmt = RKVC_FRAME_FMT_NV12;
    req.input.width = width;
    req.input.height = height;
    req.output.kind = RKVC_ENDPOINT_FRAME_SINK;
    req.output.fmt = RKVC_FRAME_FMT_BITSTREAM;

    rkvc_session *s = NULL;
    rkvc_diagnostic *diag = NULL;
    rkvc_status st = rkvc_session_create(ctx, &req, &s, &diag);
    if (st != RKVC_OK)
        fail("session create", st, diag);
    st = rkvc_session_start(s, &diag);
    if (st != RKVC_OK)
        fail("session start", st, diag);

    size_t frame_bytes = (size_t)width * height * 3 / 2;
    uint8_t *yuv = malloc(frame_bytes);
    if (!yuv) {
        fprintf(stderr, "intc: no memory\n");
        return 2;
    }
    memset(yuv, 128, frame_bytes);
    for (int i = 0; i < frames; i++) {
        rkvc_frame_desc desc;
        rkvc_frame_desc_init(&desc, sizeof(desc));
        desc.spec.width = width;
        desc.spec.height = height;
        desc.spec.fmt = RKVC_FRAME_FMT_NV12;
        desc.spec.domain = RKVC_MEM_DOMAIN_HOST;
        desc.data = yuv;
        desc.size = frame_bytes;
        desc.pts = (int64_t)i * 3000;
        rkvc_frame *f = NULL;
        st = rkvc_frame_wrap(&desc, &f);
        if (st != RKVC_OK)
            fail("frame wrap", st, NULL);
        /* Synchronous send: backpressure is absorbed inside. */
        for (;;) {
            st = rkvc_session_push(s, f);
            if (st == RKVC_AGAIN) {
                rkvc_frame *drained = NULL;
                if (rkvc_session_try_pull(s, &drained) == RKVC_OK)
                    rkvc_frame_release(drained);
                continue;
            }
            break;
        }
        rkvc_frame_release(f);
        if (st != RKVC_OK)
            fail("push", st, NULL);
    }
    free(yuv);
    if (rkvc_session_push_eos(s) != RKVC_OK) {
        fprintf(stderr, "intc: push_eos failed\n");
        return 2;
    }
    size_t bytes = 0;
    int packets = 0;
    for (;;) {
        rkvc_frame *f = NULL;
        st = rkvc_session_pull(s, &f);
        if (st == RKVC_EOF)
            break;
        if (st != RKVC_OK)
            fail("pull", st, NULL);
        rkvc_frame_desc desc;
        rkvc_frame_desc_init(&desc, sizeof(desc));
        rkvc_frame_get_desc(f, &desc);
        bytes += desc.size;
        packets++;
        rkvc_frame_release(f);
    }
    rkvc_session_destroy(s);
    rkvc_context_destroy(ctx);
    printf("intc: %d packets %zu bytes\n", packets, bytes);
    return (packets > 0 && bytes > 0) ? 0 : 3;
}
