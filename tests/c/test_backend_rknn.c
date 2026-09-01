/* SPDX-License-Identifier: AGPL-3.0-or-later */
/** @file test_backend_rknn.c Fake-runtime E2E test for backend_rknn.c. */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sodium.h>

#include "rkvc/backend.h"
#include "rkvc/context.h"
#include "rkvc/job.h"
#include "context_internal.h"
#include "rkmodel_layout.h"

#define TEST_ROOT "/tmp/rkvc_test_backend_rknn"
#define MODEL_DIR TEST_ROOT "/models"
#define MODEL_PATH MODEL_DIR "/phase.rkmodel"

extern const rkvc_backend *rkvc_backend_query(void);

typedef struct blob {
    uint8_t *data;
    size_t len;
    size_t cap;
} blob;

static void put(blob *b, const void *data, size_t size) {
    if (b->len + size > b->cap) {
        b->cap = (b->len + size) * 2 + 64;
        b->data = realloc(b->data, b->cap);
        assert_non_null(b->data);
    }
    memcpy(b->data + b->len, data, size);
    b->len += size;
}

static void put_u16(blob *b, uint16_t value) { put(b, &value, sizeof(value)); }
static void put_u32(blob *b, uint32_t value) { put(b, &value, sizeof(value)); }

static void put_tlv(blob *b, uint16_t tag, const char *text) {
    put_u16(b, tag);
    put_u32(b, (uint32_t)strlen(text));
    put(b, text, strlen(text));
}

static void write_model(void) {
    static const uint8_t model_payload[64] = {
        'F', 'A', 'K', 'E', '-', 'R', 'K', 'N', 'N'
    };
    blob tlv = {0}, file = {0};
    rkmodel_fixed fixed;
    rkmodel_payload_entry entry;
    FILE *fp;

    put_tlv(&tlv, RKMODEL_TAG_FAMILY, "phase-rlfn");
    put_tlv(&tlv, RKMODEL_TAG_ROLE, "upscale");
    put_tlv(&tlv, RKMODEL_TAG_ID, "phase-test");
    put_tlv(&tlv, RKMODEL_TAG_VERSION, "1.0.0");
    memset(&fixed, 0, sizeof(fixed));
    fixed.magic = RKMODEL_MAGIC;
    fixed.format_version = RKMODEL_VERSION;
    fixed.header_len = (uint32_t)tlv.len;
    fixed.payload_count = 1;
    memset(&entry, 0, sizeof(entry));
    entry.kind = RKMODEL_PAYLOAD_RKNN;
    entry.offset = RKMODEL_FIXED_SIZE + tlv.len + sizeof(entry);
    entry.length = sizeof(model_payload);
    crypto_hash_sha256(entry.sha256, model_payload, sizeof(model_payload));
    put(&file, &fixed, sizeof(fixed));
    put(&file, tlv.data, tlv.len);
    put(&file, &entry, sizeof(entry));
    put(&file, model_payload, sizeof(model_payload));

    (void)mkdir(TEST_ROOT, 0755);
    (void)mkdir(MODEL_DIR, 0755);
    fp = fopen(MODEL_PATH, "wb");
    assert_non_null(fp);
    assert_int_equal(fwrite(file.data, 1, file.len, fp), file.len);
    fclose(fp);
    free(tlv.data);
    free(file.data);
}

static void test_phase_model_runs_as_transform(void **state) {
    rkvc_context_options context_options;
    rkvc_context *context = NULL;
    rkvc_request request;
    rkvc_job *job = NULL;
    rkvc_diag *diag = NULL;
    rkvc_frame_spec input_spec;
    rkvc_frame *input = NULL;
    rkvc_frame *output = NULL;
    rkvc_frame_desc output_desc;
    uint8_t input_data[8 * 8 * 3 / 2];
    size_t i;
    (void)state;

    write_model();
    memset(input_data, 80, 8 * 8);
    memset(input_data + 8 * 8, 128, 8 * 8 / 2);
    rkvc_context_options_init(&context_options, sizeof(context_options));
    context_options.model_dir_override = MODEL_DIR;
    assert_int_equal(rkvc_context_create(&context_options, &context),
                     RKVC_STATUS_OK);
    assert_int_equal(rkvc_registry_add_backend(context, rkvc_backend_query()),
                     RKVC_STATUS_OK);

    rkvc_request_init(&request, sizeof(request));
    request.operation = RKVC_OPERATION_UPSCALE;
    request.input.kind = RKVC_ENDPOINT_FRAME_SINK;
    request.input.fmt = RKVC_FRAME_FMT_NV12;
    request.output.kind = RKVC_ENDPOINT_FRAME_SINK;
    request.output.fmt = RKVC_FRAME_FMT_NV12;
    request.width = 8;
    request.height = 8;
    request.model_id = "phase-test";
    assert_int_equal(rkvc_job_create(context, &request, &diag, &job),
                     RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_start(job, &diag), RKVC_STATUS_OK);

    memset(&input_spec, 0, sizeof(input_spec));
    input_spec.width = 8;
    input_spec.height = 8;
    input_spec.fmt = RKVC_FRAME_FMT_NV12;
    input_spec.domain = RKVC_MEM_DOMAIN_HOST;
    input_spec.stride = 8;
    input_spec.ver_stride = 8;
    assert_int_equal(rkvc_frame_wrap_host(&input_spec, input_data,
                                         sizeof(input_data), &input),
                     RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_push(job, input), RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_push_eos(job), RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_pull(job, &output), RKVC_STATUS_OK);
    assert_int_equal(rkvc_frame_get_desc(output, &output_desc),
                     RKVC_STATUS_OK);
    assert_int_equal(output_desc.spec.width, 24);
    assert_int_equal(output_desc.spec.height, 24);
    assert_int_equal(output_desc.spec.fmt, RKVC_FRAME_FMT_NV12);
    assert_int_equal(output_desc.spec.domain, RKVC_MEM_DOMAIN_HOST);
    assert_int_equal(output_desc.size, 24 * 24 * 3 / 2);
    for (i = 0; i < 24 * 24; ++i)
        assert_int_equal(((uint8_t *)output_desc.data)[i], 81);
    for (; i < output_desc.size; ++i) {
        uint8_t expected = ((i - 24 * 24) & 1u) ? 131 : 130;
        assert_int_equal(((uint8_t *)output_desc.data)[i], expected);
    }
    rkvc_frame_release(output);
    assert_int_equal(rkvc_job_pull(job, &output), RKVC_STATUS_EOF);
    assert_int_equal(rkvc_job_wait(job), RKVC_STATUS_OK);

    rkvc_job_destroy(job);
    rkvc_diag_release(diag);
    rkvc_context_destroy(context);
    unlink(MODEL_PATH);
    rmdir(MODEL_DIR);
    rmdir(TEST_ROOT);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_phase_model_runs_as_transform),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
