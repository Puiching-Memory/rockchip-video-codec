/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file cli_parse.h
 * @brief CLI 共用参数解析（policy / 分辨率 / rc-mode / codec / pix-fmt）。
 */

#ifndef RKVC_CLI_PARSE_H
#define RKVC_CLI_PARSE_H

#include "rkvc/rkvc.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @return 0 成功，-1 无法识别。 */
int rkvc_cli_parse_policy(const char *s, rkvc_policy *out);
int rkvc_cli_parse_wxh(const char *s, int *w, int *h);
int rkvc_cli_parse_rc_mode(const char *s, rkvc_rc_mode *out);
int rkvc_cli_parse_codec(const char *s, rkvc_codec *out);
int rkvc_cli_parse_pix_fmt(const char *s, rkvc_pix_fmt *out);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_CLI_PARSE_H */
