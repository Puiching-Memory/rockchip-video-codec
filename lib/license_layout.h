/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file license_layout.h
 * @brief 授权 blob 线格式常量（lib/license.c 与 tools/rkvc_lic.c 的单一来源）。
 *
 * 签发端（rkvc_lic）与校验端（librkvc）必须字节级一致，故共享此头文件，
 * 避免魔法数 / 区域长度在两处手抄后漂移，导致校验静默失败。
 */
#ifndef RKVC_LICENSE_LAYOUT_H
#define RKVC_LICENSE_LAYOUT_H

#include <stdint.h>

/** magic（小端 → "RKVC"）*/
#define RKVC_LICENSE_MAGIC      0x43564B52u

/** 签名覆盖区长度：magic(4) + product(4) + machine_id[32] */
#define RKVC_LICENSE_SIGNED_LEN 40u

/** Ed25519 detached 签名长度 */
#define RKVC_LICENSE_SIG_LEN    64u

_Static_assert(RKVC_LICENSE_SIGNED_LEN == 40u,
               "signed region must be 40 bytes");
_Static_assert(RKVC_LICENSE_SIG_LEN == 64u,
               "Ed25519 signature must be 64 bytes");

#endif /* RKVC_LICENSE_LAYOUT_H */
