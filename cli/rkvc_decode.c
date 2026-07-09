/**
 * @file rkvc_decode.c
 * @brief v2 Session CLI：压缩文件 → 原始 NV12。
 */

#include "rkvc/rkvc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

static void usage(void)
{
    printf("rkvc_decode -i input.mp4 -o out.nv12\n");
}

int main(int argc, char **argv)
{
    const char *input = NULL, *output = NULL;

    static struct option opts[] = {
        {"input", required_argument, 0, 'i'},
        {"output", required_argument, 0, 'o'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "i:o:h", opts, NULL)) != -1) {
        if (c == 'i') input = optarg;
        else if (c == 'o') output = optarg;
        else { usage(); return c == 'h' ? 0 : 1; }
    }

    if (!input || !output) {
        usage();
        return 1;
    }

    rkvc_pipeline_desc d;
    rkvc_pipeline_from_template(RKVC_TEMPLATE_FILE_DECODE, &d);
    d.input_path  = input;
    d.output_path = output;

    rkvc_session *s = NULL;
    rkvc_err err = rkvc_session_create(&d, &s);
    if (err != RKVC_OK) {
        fprintf(stderr, "session create: %s\n", rkvc_err_str(err));
        return 1;
    }

    err = rkvc_session_run_file(s);
    rkvc_session_destroy(s);
    if (err != RKVC_OK) {
        fprintf(stderr, "decode failed: %s\n", rkvc_err_str(err));
        return 1;
    }
    return 0;
}
