/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file test_rkmodel.c
 * @brief .rkmodel v1 容器与模型注册表回归测试。
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <cmocka.h>

#include "context_internal.h"
#include "rkmodel.h"

typedef struct {
    uint8_t *bytes;
    size_t len;
    size_t cap;
} blob;

static void blob_put(blob *b, const void *p, size_t n) {
    if (b->len + n > b->cap) {
        b->cap = (b->len + n) * 2 + 128;
        b->bytes = realloc(b->bytes, b->cap);
        assert_non_null(b->bytes);
    }
    memcpy(b->bytes + b->len, p, n);
    b->len += n;
}

static void blob_u16(blob *b, uint16_t v) { blob_put(b, &v, sizeof(v)); }
static void blob_u32(blob *b, uint32_t v) { blob_put(b, &v, sizeof(v)); }

static void blob_tlv(blob *b, uint16_t tag, const char *text) {
    blob_u16(b, tag);
    blob_u32(b, (uint32_t)strlen(text));
    blob_put(b, text, strlen(text));
}

static const uint8_t PAYLOAD_A[97] = {[0] = 0x42, [96] = 0x24};
static const uint8_t PAYLOAD_B[53] = {[0] = 0x7e, [52] = 0xe7};

typedef struct {
    uint32_t kind;
    const uint8_t *data;
    size_t len;
} test_payload;

static void build_container(blob *out, uint16_t extra_tlv_tag,
                            const test_payload *payloads, size_t count) {
    blob tlv = {0};
    rkmodel_fixed fixed = {0};
    size_t data_offset;

    blob_tlv(&tlv, RKMODEL_TAG_FAMILY, "sr");
    blob_tlv(&tlv, RKMODEL_TAG_ROLE, "upscale");
    blob_tlv(&tlv, RKMODEL_TAG_ID, "sr-x2-test");
    blob_tlv(&tlv, RKMODEL_TAG_VERSION, "1.0.0");
    blob_tlv(&tlv, RKMODEL_TAG_RKNN_TARGET, "rk3588");
    if (extra_tlv_tag) {
        blob_u16(&tlv, extra_tlv_tag);
        blob_u32(&tlv, 4);
        blob_u32(&tlv, 0xdeadbeef);
    }

    fixed.magic = RKMODEL_MAGIC;
    fixed.format_version = RKMODEL_VERSION;
    fixed.header_len = (uint32_t)tlv.len;
    fixed.payload_count = (uint32_t)count;
    fixed.payload_entry_size = sizeof(rkmodel_payload_entry);
    data_offset = sizeof(fixed) + tlv.len +
                  count * sizeof(rkmodel_payload_entry);

    memset(out, 0, sizeof(*out));
    blob_put(out, &fixed, sizeof(fixed));
    blob_put(out, tlv.bytes, tlv.len);
    for (size_t i = 0; i < count; ++i) {
        rkmodel_payload_entry entry = {0};
        entry.kind = payloads[i].kind;
        entry.offset = data_offset;
        entry.length = payloads[i].len;
        blob_put(out, &entry, sizeof(entry));
        data_offset += payloads[i].len;
    }
    for (size_t i = 0; i < count; ++i)
        blob_put(out, payloads[i].data, payloads[i].len);
    free(tlv.bytes);
}

static void write_file(const char *path, const uint8_t *data, size_t size) {
    FILE *file = fopen(path, "wb");
    assert_non_null(file);
    assert_int_equal(fwrite(data, 1, size, file), size);
    fclose(file);
}

static void test_roundtrip(void **state) {
    test_payload payloads[2] = {
        {RKMODEL_PAYLOAD_RKNN, PAYLOAD_A, sizeof(PAYLOAD_A)},
        {RKMODEL_PAYLOAD_PMF, PAYLOAD_B, sizeof(PAYLOAD_B)},
    };
    blob container;
    rkvc_rkmodel model;
    char error[160] = {0};
    void *data = NULL;
    size_t size = 0;
    (void)state;

    build_container(&container, 0x77, payloads, 2);
    write_file("/tmp/rkvc_test_roundtrip.rkmodel",
               container.bytes, container.len);
    assert_int_equal(rkvc_rkmodel_open("/tmp/rkvc_test_roundtrip.rkmodel",
                                       &model, error, sizeof(error)),
                     RKVC_STATUS_OK);
    assert_string_equal(model.info.id, "sr-x2-test");
    assert_string_equal(model.info.family, "sr");
    assert_string_equal(model.info.role, "upscale");
    assert_string_equal(model.info.version, "1.0.0");
    assert_string_equal(model.info.rknn_target, "rk3588");
    assert_int_equal(model.payload_count, 2);
    assert_true(model.info.payload_mask & (1u << RKMODEL_PAYLOAD_RKNN));
    assert_true(model.info.payload_mask & (1u << RKMODEL_PAYLOAD_PMF));

    assert_int_equal(rkvc_rkmodel_load_payload(&model, RKMODEL_PAYLOAD_RKNN,
                                               &data, &size), RKVC_STATUS_OK);
    assert_int_equal(size, sizeof(PAYLOAD_A));
    assert_memory_equal(data, PAYLOAD_A, sizeof(PAYLOAD_A));
    free(data);
    assert_int_equal(rkvc_rkmodel_load_payload(&model,
                                               RKMODEL_PAYLOAD_QPPATCH,
                                               &data, &size),
                     RKVC_STATUS_NOT_FOUND);
    free(container.bytes);
}

static void test_bad_magic(void **state) {
    test_payload payload = {RKMODEL_PAYLOAD_RKNN, PAYLOAD_B, sizeof(PAYLOAD_B)};
    blob container;
    rkvc_rkmodel model;
    char error[160] = {0};
    (void)state;

    build_container(&container, 0, &payload, 1);
    container.bytes[0] ^= 0xff;
    write_file("/tmp/rkvc_test_badmagic.rkmodel",
               container.bytes, container.len);
    assert_int_equal(rkvc_rkmodel_open("/tmp/rkvc_test_badmagic.rkmodel",
                                       &model, error, sizeof(error)),
                     RKVC_STATUS_INVALID);
    assert_non_null(strstr(error, "magic"));
    free(container.bytes);
}

static void test_oversized_header(void **state) {
    test_payload payload = {RKMODEL_PAYLOAD_RKNN, PAYLOAD_B, sizeof(PAYLOAD_B)};
    blob container;
    rkvc_rkmodel model;
    uint32_t huge = RKMODEL_MAX_HEADER + 1;
    (void)state;

    build_container(&container, 0, &payload, 1);
    memcpy(container.bytes + offsetof(rkmodel_fixed, header_len),
           &huge, sizeof(huge));
    write_file("/tmp/rkvc_test_hugehdr.rkmodel",
               container.bytes, container.len);
    assert_int_equal(rkvc_rkmodel_open("/tmp/rkvc_test_hugehdr.rkmodel",
                                       &model, NULL, 0), RKVC_STATUS_INVALID);
    free(container.bytes);
}

static void test_old_payload_table_layout_rejected(void **state) {
    test_payload payload = {RKMODEL_PAYLOAD_RKNN, PAYLOAD_A, sizeof(PAYLOAD_A)};
    blob container;
    rkvc_rkmodel model;
    rkmodel_fixed *fixed;
    (void)state;

    build_container(&container, 0, &payload, 1);
    fixed = (rkmodel_fixed *)container.bytes;
    fixed->payload_entry_size = 0;
    write_file("/tmp/rkvc_test_old_layout.rkmodel",
               container.bytes, container.len);
    assert_int_equal(rkvc_rkmodel_open("/tmp/rkvc_test_old_layout.rkmodel",
                                       &model, NULL, 0), RKVC_STATUS_INVALID);
    free(container.bytes);
}

static void test_payload_out_of_bounds(void **state) {
    test_payload payload = {RKMODEL_PAYLOAD_RKNN, PAYLOAD_A, sizeof(PAYLOAD_A)};
    blob container;
    rkvc_rkmodel model;
    rkmodel_fixed *fixed;
    rkmodel_payload_entry *entry;
    (void)state;

    build_container(&container, 0, &payload, 1);
    fixed = (rkmodel_fixed *)container.bytes;
    entry = (rkmodel_payload_entry *)(container.bytes + sizeof(*fixed) +
                                      fixed->header_len);
    entry->length = container.len;
    write_file("/tmp/rkvc_test_oob.rkmodel", container.bytes, container.len);
    assert_int_equal(rkvc_rkmodel_open("/tmp/rkvc_test_oob.rkmodel",
                                       &model, NULL, 0), RKVC_STATUS_INVALID);
    free(container.bytes);
}

static void test_duplicate_payload_kind(void **state) {
    test_payload payloads[2] = {
        {RKMODEL_PAYLOAD_RKNN, PAYLOAD_A, sizeof(PAYLOAD_A)},
        {RKMODEL_PAYLOAD_RKNN, PAYLOAD_B, sizeof(PAYLOAD_B)},
    };
    blob container;
    rkvc_rkmodel model;
    (void)state;

    build_container(&container, 0, payloads, 2);
    write_file("/tmp/rkvc_test_duplicate.rkmodel",
               container.bytes, container.len);
    assert_int_equal(rkvc_rkmodel_open("/tmp/rkvc_test_duplicate.rkmodel",
                                       &model, NULL, 0), RKVC_STATUS_INVALID);
    free(container.bytes);
}

static void test_registry_scan(void **state) {
    test_payload payload = {RKMODEL_PAYLOAD_RKNN, PAYLOAD_B, sizeof(PAYLOAD_B)};
    blob good;
    blob bad;
    const char *dirs[1] = {"/tmp/rkvc_test_models"};
    rkvc_context_options options;
    rkvc_context *context = NULL;
    rkvc_model_info info;
    (void)state;

    mkdir(dirs[0], 0755);
    build_container(&good, 0, &payload, 1);
    write_file("/tmp/rkvc_test_models/good.rkmodel", good.bytes, good.len);
    build_container(&bad, 0, &payload, 1);
    bad.bytes[0] ^= 0xff;
    write_file("/tmp/rkvc_test_models/bad.rkmodel", bad.bytes, bad.len);

    rkvc_context_options_init(&options, sizeof(options));
    options.paths.model_dirs = dirs;
    options.paths.model_dir_count = 1;
    assert_int_equal(rkvc_context_create(&options, &context), RKVC_STATUS_OK);
    assert_int_equal(rkvc_model_count(context), 1);
    assert_int_equal(rkvc_model_info_at(context, 0, &info), RKVC_STATUS_OK);
    assert_string_equal(info.id, "sr-x2-test");
    assert_int_equal(rkvc_model_info_at(context, 1, &info),
                     RKVC_STATUS_NOT_FOUND);
    rkvc_context_destroy(context);
    free(good.bytes);
    free(bad.bytes);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_roundtrip),
        cmocka_unit_test(test_bad_magic),
        cmocka_unit_test(test_oversized_header),
        cmocka_unit_test(test_old_payload_table_layout_rejected),
        cmocka_unit_test(test_payload_out_of_bounds),
        cmocka_unit_test(test_duplicate_payload_kind),
        cmocka_unit_test(test_registry_scan),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
