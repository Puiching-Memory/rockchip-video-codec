/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file model_crypt_layout.h
 * @brief 模型加密线格式常量（lib/model_crypt.c 与 tools/rkvc_model_crypt.c 的单一来源）。
 *
 * 加密端（打包工具）与解密端（librkvc）必须字节级一致，故共享此头文件，
 * 避免魔法数 / 区域长度在两处手抄后漂移。
 *
 * 设计（与 Rockchip rknn_crypt_tool 的机制对位，但密钥完全自主可控）：
 *  - 模型体用随机数据密钥 data_key（32B）做 XSalsa20-Poly1305
 *    （libsodium crypto_secretbox，自带 16B MAC）。
 *  - data_key 不随包分发：打包方用内嵌在 librkvc 的 master_key 把
 *    「data_key + 目标机机器码」密封成每机一份的 model.key 授权文件。
 *  - 运行时解密 model.key 后校验机器码（复用 1机1码 指纹），通过才解密模型。
 *    包拷到未授权机器 → 机器码不符 → 模型不可解密。
 */
#ifndef RKVC_MODEL_CRYPT_LAYOUT_H
#define RKVC_MODEL_CRYPT_LAYOUT_H

#include <stdint.h>

/* ── 加密模型文件（*.rknn 原地替换，内容以下列头部开头） ─────────── */

/** 加密模型 magic（8 字节） */
#define RKVC_MODEL_ENC_MAGIC     "RKVCENC1"
#define RKVC_MODEL_ENC_MAGIC_LEN 8u

/** 加密模型头部总长：magic(8)+version(4)+flags(4)+plain_len(8)+nonce(24) */
#define RKVC_MODEL_ENC_HDR_LEN   48u

/** 当前格式版本 */
#define RKVC_MODEL_ENC_VERSION   1u

/** flags bit0：预留（未来 per-machine 直加密）。当前必须为 0 */
#define RKVC_MODEL_ENC_FLAG_RESERVED 0x1u

/** crypto_secretbox MAC 长度（libsodium 常量，此处显式声明便于布局计算） */
#define RKVC_MODEL_ENC_MAC_LEN   16u
/** XSalsa20-Poly1305 nonce 长度（libsodium crypto_secretbox） */
#define RKVC_MODEL_ENC_NONCE_LEN 24u

/* ── 每机模型授权文件 model.key ─────────────────────────────────── */

/** model.key magic（8 字节） */
#define RKVC_MODEL_KEY_MAGIC     "RKVCMKEY"
#define RKVC_MODEL_KEY_MAGIC_LEN 8u

/** model.key 版本 */
#define RKVC_MODEL_KEY_VERSION   1u

/** model.key 明文 = data_key(32) + machine_id hex ASCII(64) */
#define RKVC_MODEL_KEY_PLAIN_LEN (32u + 64u)

/** model.key 总长：magic(8)+version(4)+nonce(24)+secretbox(MAC16+明文) */
#define RKVC_MODEL_KEY_FILE_LEN  (8u + 4u + 24u + RKVC_MODEL_ENC_MAC_LEN + RKVC_MODEL_KEY_PLAIN_LEN)

/** 机器码十六进制**字符数**（不含 NUL；license_machine.h 的 LIC_MACHINE_ID_HEX_LEN
 *  为该值 + 1，是输出缓冲容量，两者不可混用） */
#define RKVC_MODEL_CRYPT_MACHINE_ID_HEX_LEN 64u

_Static_assert(RKVC_MODEL_KEY_FILE_LEN == 148u,
               "model.key file must be 148 bytes");
_Static_assert(RKVC_MODEL_ENC_MAGIC_LEN + 4u + 4u + 8u +
               RKVC_MODEL_ENC_NONCE_LEN == RKVC_MODEL_ENC_HDR_LEN,
               "encrypted model header layout mismatch");
_Static_assert(RKVC_MODEL_ENC_HDR_LEN == 48u,
               "encrypted model wire header must remain 48 bytes");

#endif /* RKVC_MODEL_CRYPT_LAYOUT_H */
