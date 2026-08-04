/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file test_support.h
 * @brief Shared helpers for optional hardware / driver stress tests.
 *
 * Real RKMPP / RGA / SVT paths can wedge or reboot embedded boards when run
 * concurrently or under Valgrind. Safe unit tests always run; hardware stress
 * tests require RKVC_RUN_HARDWARE_TESTS=1 (opt-in).
 */

#ifndef RKVC_TEST_SUPPORT_H
#define RKVC_TEST_SUPPORT_H

#include <stdlib.h>

#define RKVC_CTEST_SKIP 77

static inline int rkvc_test_hardware_opted_in(void)
{
    const char *run = getenv("RKVC_RUN_HARDWARE_TESTS");
    const char *skip = getenv("RKVC_SKIP_HARDWARE_TESTS");

    if (skip && skip[0] != '\0' && skip[0] != '0')
        return 0;
    return run && run[0] != '\0' && run[0] != '0';
}

#endif /* RKVC_TEST_SUPPORT_H */
