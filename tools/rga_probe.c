/* 最小 librga 验证：NV12 640x368 HOST -> 1280x736 HOST，逐 interp 模式。
 * 用法: LD_LIBRARY_PATH=<pkg>/lib ./rga_probe [in_w in_h out_w out_h fmt] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "im2d.h"

static const char *imst(IM_STATUS s) { return imStrError_t(s); }

int main(int argc, char **argv) {
    int in_w = argc > 4 ? atoi(argv[1]) : 640;
    int in_h = argc > 5 ? atoi(argv[2]) : 368;
    int out_w = argc > 6 ? atoi(argv[3]) : 1280;
    int out_h = argc > 7 ? atoi(argv[4]) : 736;
    int use_cubic_only = argc > 8 ? atoi(argv[5]) : 0;
    size_t in_sz = (size_t)in_w * in_h * 3 / 2;
    size_t out_sz = (size_t)out_w * out_h * 3 / 2;
    unsigned char *inb = malloc(in_sz);
    unsigned char *outb = malloc(out_sz);
    rga_buffer_t src, dst;
    IM_STATUS ret;
    int modes[] = {IM_INTERP_CUBIC, IM_INTERP_LINEAR, IM_INTERP_DEFAULT};
    const char *names[] = {"CUBIC", "LINEAR", "DEFAULT"};
    int i;

    if (!inb || !outb)
        return 2;
    memset(inb, 0x80, in_sz);
    src = wrapbuffer_virtualaddr_t(inb, in_w, in_h, in_w, in_h,
                                   RK_FORMAT_YCbCr_420_SP);
    dst = wrapbuffer_virtualaddr_t(outb, out_w, out_h, out_w, out_h,
                                   RK_FORMAT_YCbCr_420_SP);
    printf("probe %dx%d -> %dx%d NV12\n", in_w, in_h, out_w, out_h);
    for (i = 0; i < 3; ++i) {
        if (use_cubic_only && modes[i] != IM_INTERP_CUBIC)
            continue;
        memset(outb, 0, out_sz);
        ret = imresize_t(src, dst, 0, 0, modes[i], 1);
        printf("  %-7s: ret=%d (%s)", names[i], (int)ret, imst(ret));
        if (ret == IM_STATUS_NOERROR || ret == IM_STATUS_SUCCESS)
            printf(" out[0..3]=%02x %02x %02x %02x",
                   outb[0], outb[1], outb[2], outb[3]);
        printf("\n");
    }
    /* 缩小对照（RGA 常用方向）*/
    {
        unsigned char *small = malloc((size_t)in_w * in_h * 3 / 2);
        rga_buffer_t sdst = wrapbuffer_virtualaddr_t(small, in_w, in_h,
                                                     in_w, in_h,
                                                     RK_FORMAT_YCbCr_420_SP);
        ret = imresize_t(src, sdst, 0, 0, IM_INTERP_LINEAR, 1);
        printf("  down-to-self(LINEAR): ret=%d (%s)\n", (int)ret, imst(ret));
        free(small);
    }
    free(inb);
    free(outb);
    return 0;
}
