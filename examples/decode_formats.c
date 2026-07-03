/**
 * @file decode_formats.c
 * @brief v2 示例：同一码流分别以 NV12 / YUV420P / NV16 / P010 解码，校验输出帧格式。
 */

#include "rkvc/rkvc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *pix_fmt_label(rkvc_pix_fmt fmt)
{
    switch (fmt) {
    case RKVC_PIX_FMT_NV12:    return "NV12";
    case RKVC_PIX_FMT_YUV420P: return "YUV420P";
    case RKVC_PIX_FMT_NV16:    return "NV16";
    case RKVC_PIX_FMT_P010:    return "P010";
    default:                   return "?";
    }
}

static int encode_fixture(const char *path)
{
    char raw_path[] = "/tmp/rkvc_fmt_raw_XXXXXX";
    int fd = mkstemp(raw_path);
    if (fd < 0)
        return -1;

    const int w = 320, h = 240, frames = 10;
    const size_t frame_sz = (size_t)w * (size_t)h * 3 / 2;
    uint8_t *raw = malloc(frame_sz);
    if (!raw) {
        close(fd);
        unlink(raw_path);
        return -1;
    }

    for (int i = 0; i < frames; i++) {
        memset(raw, (uint8_t)(i & 0xff), (size_t)w * (size_t)h);
        memset(raw + (size_t)w * (size_t)h, 128, frame_sz - (size_t)w * (size_t)h);
        if (write(fd, raw, frame_sz) != (ssize_t)frame_sz) {
            free(raw);
            close(fd);
            unlink(raw_path);
            return -1;
        }
    }
    close(fd);
    free(raw);

    rkvc_pipeline_desc d;
    rkvc_pipeline_from_template(RKVC_TEMPLATE_FILE_ENCODE, &d);
    d.input_path  = raw_path;
    d.output_path = path;
    d.width       = w;
    d.height      = h;
    d.fps_num     = 30;
    d.bitrate     = 2000000;
    d.policy      = RKVC_POLICY_BALANCED;

    rkvc_session *s = NULL;
    rkvc_err err = rkvc_session_create(&d, &s);
    if (err == RKVC_OK)
        err = rkvc_session_run_file(s);
    rkvc_session_destroy(s);
    unlink(raw_path);
    return err == RKVC_OK ? 0 : -1;
}

static int decode_check_format(const char *input, rkvc_pix_fmt fmt)
{
    char outpath[] = "/tmp/rkvc_fmt_out_XXXXXX";
    int fd = mkstemp(outpath);
    if (fd < 0)
        return 0;
    close(fd);

    rkvc_pipeline_desc d;
    rkvc_pipeline_from_template(RKVC_TEMPLATE_FILE_DECODE, &d);
    d.input_path    = input;
    d.output_path   = outpath;
    d.pixel_format  = fmt;

    rkvc_session *s = NULL;
    if (rkvc_session_create(&d, &s) != RKVC_OK) {
        unlink(outpath);
        return 0;
    }

    rkvc_err err = rkvc_session_run_file(s);
    int ok = 0;

    if (err == RKVC_OK) {
        rkvc_port *out = rkvc_session_port(s, "output");
        rkvc_buffer *buf = NULL;
        if (out && rkvc_port_pull(out, &buf, 0) == RKVC_OK) {
            rkvc_buffer_video_info info;
            if (rkvc_buffer_get_video_info(buf, &info) == RKVC_OK &&
                info.format == fmt)
                ok = 1;
            rkvc_buffer_unref(buf);
        }
    }

    rkvc_session_destroy(s);
    unlink(outpath);
    return ok;
}

int main(int argc, char **argv)
{
    const char *input = (argc >= 2) ? argv[1] : NULL;
    char generated[] = "/tmp/rkvc_fmt_in_XXXXXX.mp4";

    rkvc_init();

    if (!input) {
        if (mkstemps(generated, 4) < 0) {
            fprintf(stderr, "failed to create temp input\n");
            return 1;
        }
        if (encode_fixture(generated) != 0) {
            fprintf(stderr, "failed to generate test clip (need RK3588 HW)\n");
            unlink(generated);
            return 1;
        }
        input = generated;
        printf("generated fixture: %s\n", input);
    }

    static const rkvc_pix_fmt formats[] = {
        RKVC_PIX_FMT_NV12,
        RKVC_PIX_FMT_YUV420P,
        RKVC_PIX_FMT_NV16,
        RKVC_PIX_FMT_P010,
    };

    int failed = 0;
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        rkvc_pix_fmt fmt = formats[i];
        int ok = decode_check_format(input, fmt);
        printf("  %s: %s\n", pix_fmt_label(fmt), ok ? "✓" : "✗");
        if (!ok)
            failed = 1;
    }

    if (input == generated)
        unlink(generated);

    rkvc_deinit();
    return failed ? 1 : 0;
}
