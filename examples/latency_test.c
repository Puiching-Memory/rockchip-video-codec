/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/** latency_test.c — v2 session 启动延迟 */
#include "rkvc/rkvc.h"
#include <stdio.h>
#include <sys/time.h>
int main(void) {
    rkvc_pipeline_desc d = rkvc_pipeline_desc_defaults();
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    rkvc_session *s = NULL;
    rkvc_session_create(&d, &s);
    gettimeofday(&t1, NULL);
    double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
                (t1.tv_usec - t0.tv_usec) / 1000.0;
    printf("session_create: %.2f ms\n", ms);
    rkvc_session_destroy(s);
    return 0;
}
