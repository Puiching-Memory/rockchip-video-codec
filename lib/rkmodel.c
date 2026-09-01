/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file rkmodel.c
 * @brief .rkmodel v1 容器读取器：有界头、TLV、载荷表、签名尾校验。
 *
 * 安全约束：任何长度字段在读前检查上界；TLV 遍历以 header_len 为界且
 * tag/len 头自身不得越界；载荷表 offset/length 不做读时跟踪（按需校验
 * 摘要时再 seek）；所有解析失败留下诊断字符串。
 */

#include "rkmodel.h"

#include "graph_internal.h"

#include <errno.h>
#include <inttypes.h>
#include <sodium.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

/** 把解析失败原因格式化进调用方 errbuf（可为空）。 */
static void fail(char *errbuf, size_t cap, const char *fmt, ...) {
    va_list ap;
    if (!errbuf || !cap)
        return;
    va_start(ap, fmt);
    vsnprintf(errbuf, cap, fmt, ap);
    va_end(ap);
}

/** 拷贝 TLV 字符串值并按目标容量截断、补 NUL。 */
static void tlv_str(const uint8_t *val, uint32_t len, char *dst, size_t cap) {
    size_t n = len < cap - 1 ? len : cap - 1;
    memcpy(dst, val, n);
    dst[n] = '\0';
}

rkvc_status rkvc_rkmodel_open(const char *path, rkvc_rkmodel *out,
                              rkvc_rkmodel_verify_fn verify, void *opaque,
                              char *errbuf, size_t errcap) {
    FILE *f = NULL;
    rkmodel_fixed fixed;
    uint8_t *tlv = NULL;         /* TLV 区 + 载荷表 + 签名尾的签名输入缓冲 */
    size_t table_bytes, sig_input_len;
    rkvc_status rc = RKVC_STATUS_OK;

    if (!path || !out) {
        fail(errbuf, errcap, "invalid argument");
        return RKVC_STATUS_INVALID;
    }
    memset(out, 0, sizeof(*out));
    out->info.trust = RKVC_MODEL_TRUST_UNSIGNED;
    /* 记录来源路径（截断到字段容量），供后续按需装载载荷。 */
    {
        size_t plen = strlen(path);
        if (plen >= sizeof(out->path))
            plen = sizeof(out->path) - 1;
        memcpy(out->path, path, plen);
        out->path[plen] = '\0';
    }

    f = fopen(path, "rb");
    if (!f) {
        fail(errbuf, errcap, "open: %s", strerror(errno));
        return RKVC_STATUS_IO;
    }
    if (fread(&fixed, 1, sizeof(fixed), f) != sizeof(fixed)) {
        fail(errbuf, errcap, "short fixed header");
        rc = RKVC_STATUS_INVALID;
        goto out;
    }
    if (fixed.magic != RKMODEL_MAGIC) {
        fail(errbuf, errcap, "bad magic 0x%08" PRIx32, fixed.magic);
        rc = RKVC_STATUS_INVALID;
        goto out;
    }
    if (fixed.format_version != RKMODEL_VERSION) {
        fail(errbuf, errcap, "unsupported version %" PRIu32,
             fixed.format_version);
        rc = RKVC_STATUS_INVALID;
        goto out;
    }
    if (fixed.header_len > RKMODEL_MAX_HEADER) {
        fail(errbuf, errcap, "header_len %" PRIu32 " exceeds bound",
             fixed.header_len);
        rc = RKVC_STATUS_INVALID;
        goto out;
    }
    if (fixed.payload_count > RKMODEL_MAX_PAYLOADS) {
        fail(errbuf, errcap, "payload_count %" PRIu32 " exceeds bound",
             fixed.payload_count);
        rc = RKVC_STATUS_INVALID;
        goto out;
    }
    for (size_t i = 0; i < sizeof(fixed.reserved); ++i) {
        if (fixed.reserved[i] != 0) {
            fail(errbuf, errcap, "nonzero reserved bytes");
            rc = RKVC_STATUS_INVALID;
            goto out;
        }
    }

    table_bytes = (size_t)fixed.payload_count * sizeof(rkmodel_payload_entry);
    sig_input_len = sizeof(fixed) + fixed.header_len + table_bytes;
    tlv = rkvc_g_calloc(1, fixed.header_len ? fixed.header_len : 1);
    if (!tlv) {
        fail(errbuf, errcap, "out of memory");
        rc = RKVC_STATUS_NOMEM;
        goto out;
    }
    if (fread(tlv, 1, fixed.header_len, f) != fixed.header_len) {
        fail(errbuf, errcap, "short TLV region");
        rc = RKVC_STATUS_INVALID;
        goto out;
    }

    /* TLV 遍历：tag u16 + len u32 + value */
    {
        size_t pos = 0;
        while (pos + 6 <= fixed.header_len) {
            uint16_t tag = (uint16_t)(tlv[pos] | ((uint16_t)tlv[pos + 1] << 8));
            uint32_t len = (uint32_t)tlv[pos + 2] |
                           ((uint32_t)tlv[pos + 3] << 8) |
                           ((uint32_t)tlv[pos + 4] << 16) |
                           ((uint32_t)tlv[pos + 5] << 24);
            const uint8_t *val = tlv + pos + 6;
            pos += 6;
            if (len > fixed.header_len - pos) {
                fail(errbuf, errcap, "TLV tag %u overruns header", tag);
                rc = RKVC_STATUS_INVALID;
                goto out;
            }
            switch (tag) {
            case RKMODEL_TAG_FAMILY:
                tlv_str(val, len, out->info.family, sizeof(out->info.family));
                break;
            case RKMODEL_TAG_ROLE:
                tlv_str(val, len, out->info.role, sizeof(out->info.role));
                break;
            case RKMODEL_TAG_ID:
                tlv_str(val, len, out->info.id, sizeof(out->info.id));
                break;
            case RKMODEL_TAG_VERSION:
                tlv_str(val, len, out->info.version, sizeof(out->info.version));
                break;
            case RKMODEL_TAG_RKNN_TARGET:
                tlv_str(val, len, out->info.rknn_target,
                        sizeof(out->info.rknn_target));
                break;
            case RKMODEL_TAG_MIN_ABI:
                if (len == 4)
                    out->min_runtime_abi = (uint32_t)val[0] |
                                           ((uint32_t)val[1] << 8) |
                                           ((uint32_t)val[2] << 16) |
                                           ((uint32_t)val[3] << 24);
                break;
            case RKMODEL_TAG_KEY_SLOT:
                tlv_str(val, len, out->key_slot, sizeof(out->key_slot));
                break;
            default:
                break; /* 未知 tag：跳过 */
            }
            pos += len;
        }
        if (pos != fixed.header_len) {
            fail(errbuf, errcap, "trailing %zu bytes in TLV region",
                 fixed.header_len - pos);
            rc = RKVC_STATUS_INVALID;
            goto out;
        }
    }
    if (out->info.id[0] == '\0' || out->info.family[0] == '\0' ||
        out->info.role[0] == '\0' || out->info.version[0] == '\0') {
        fail(errbuf, errcap, "missing mandatory TLV (id/family/role/version)");
        rc = RKVC_STATUS_INVALID;
        goto out;
    }

    if (fread(out->payloads, sizeof(rkmodel_payload_entry),
              fixed.payload_count, f) != fixed.payload_count) {
        fail(errbuf, errcap, "short payload table");
        rc = RKVC_STATUS_INVALID;
        goto out;
    }
    out->payload_count = fixed.payload_count;
    for (uint32_t i = 0; i < fixed.payload_count; ++i) {
        uint32_t kind = out->payloads[i].kind;
        if (kind == 0 || kind > 31) {
            fail(errbuf, errcap, "invalid payload kind %" PRIu32, kind);
            rc = RKVC_STATUS_INVALID;
            goto out;
        }
        out->info.payload_mask |= (uint32_t)1 << kind;
    }

    if (fixed.flags & RKMODEL_FLAG_SIGNED) {
        rkmodel_sig_trailer trailer;
        uint8_t *input;
        out->has_signature = 1;
        if (fread(&trailer, 1, sizeof(trailer), f) != sizeof(trailer)) {
            fail(errbuf, errcap, "short signature trailer");
            rc = RKVC_STATUS_INVALID;
            goto out;
        }
        if (trailer.alg != RKMODEL_SIG_ED25519) {
            fail(errbuf, errcap, "unsupported sig alg %" PRIu32, trailer.alg);
            rc = RKVC_STATUS_INVALID;
            goto out;
        }
        if (!verify) {
            out->info.trust = RKVC_MODEL_TRUST_UNTRUSTED;
            goto done;
        }
        input = rkvc_g_calloc(1, sig_input_len);
        if (!input) {
            rc = RKVC_STATUS_NOMEM;
            goto out;
        }
        memcpy(input, &fixed, sizeof(fixed));
        memcpy(input + sizeof(fixed), tlv, fixed.header_len);
        memcpy(input + sizeof(fixed) + fixed.header_len, out->payloads,
               table_bytes);
        out->info.trust = RKVC_MODEL_TRUST_UNTRUSTED;
        if (verify(trailer.key_id, trailer.sig, input, sig_input_len,
                   &out->info.trust, opaque) != 0)
            out->info.trust = RKVC_MODEL_TRUST_UNTRUSTED;
        rkvc_g_free(input);
    }

done:
out:
    rkvc_g_free(tlv);
    fclose(f);
    return rc;
}

rkvc_status rkvc_rkmodel_check_payload(FILE *f, const rkvc_rkmodel *m,
                                       uint32_t kind) {
    const rkmodel_payload_entry *e = NULL;
    crypto_hash_sha256_state st;
    uint8_t digest[32];
    uint8_t buf[8192];

    for (uint32_t i = 0; i < m->payload_count; ++i) {
        if (m->payloads[i].kind == kind) {
            e = &m->payloads[i];
            break;
        }
    }
    if (!e)
        return RKVC_STATUS_NOT_FOUND;
    if (e->length > (uint64_t)1 << 40) /* 1 TiB 防呆 */
        return RKVC_STATUS_INVALID;
    if (fseeko(f, (off_t)e->offset, SEEK_SET) != 0)
        return RKVC_STATUS_IO;

    crypto_hash_sha256_init(&st);
    uint64_t left = e->length;
    while (left) {
        size_t want = left < sizeof(buf) ? (size_t)left : sizeof(buf);
        size_t got = fread(buf, 1, want, f);
        if (got == 0)
            return RKVC_STATUS_IO;
        crypto_hash_sha256_update(&st, buf, got);
        left -= got;
    }
    crypto_hash_sha256_final(&st, digest);
    return memcmp(digest, e->sha256, 32) == 0 ? RKVC_STATUS_OK
                                              : RKVC_STATUS_INTEGRITY;
}

rkvc_status rkvc_rkmodel_load_payload(const rkvc_rkmodel *m, uint32_t kind,
                                      void **buf, size_t *size) {
    const rkmodel_payload_entry *e = NULL;
    FILE *f;
    uint8_t *data;
    rkvc_status rc;

    if (!m || !buf || !size || !m->path[0])
        return RKVC_STATUS_INVALID;
    *buf = NULL;
    *size = 0;
    for (uint32_t i = 0; i < m->payload_count; ++i) {
        if (m->payloads[i].kind == kind) {
            e = &m->payloads[i];
            break;
        }
    }
    if (!e)
        return RKVC_STATUS_NOT_FOUND;
    if (e->length > (uint64_t)1 << 30) /* 1 GiB 防呆 */
        return RKVC_STATUS_INVALID;

    f = fopen(m->path, "rb");
    if (!f)
        return RKVC_STATUS_IO;
    rc = rkvc_rkmodel_check_payload(f, m, kind);
    if (rc != RKVC_STATUS_OK) {
        fclose(f);
        return rc;
    }
    if (fseeko(f, (off_t)e->offset, SEEK_SET) != 0) {
        fclose(f);
        return RKVC_STATUS_IO;
    }
    data = rkvc_g_calloc(1, (size_t)e->length ? (size_t)e->length : 1);
    if (!data) {
        fclose(f);
        return RKVC_STATUS_NOMEM;
    }
    if (fread(data, 1, (size_t)e->length, f) != (size_t)e->length) {
        rkvc_g_free(data);
        fclose(f);
        return RKVC_STATUS_IO;
    }
    fclose(f);
    *buf = data;
    *size = (size_t)e->length;
    return RKVC_STATUS_OK;
}
