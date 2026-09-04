/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Fake RKNN runtime for x86 tests: legacy "phase" mode (test_backend_rknn)
 * and MLVC mode (test_backend_mlvc) with native zero-copy memory API. */
#include "rknn_api.h"

#include <stdlib.h>
#include <string.h>

#define CORE_W 4u
#define CORE_H 4u

/* MLVC geometry (matches the fake encoder/decoder contract): */
#define MLVC_IMG_W 64u
#define MLVC_IMG_H 64u
#define MLVC_REF_C 96u
#define MLVC_REF_H 16u
#define MLVC_REF_W 16u
#define MLVC_ZC 24u
#define MLVC_ZH 8u
#define MLVC_ZW 8u
#define MLVC_YC 24u
#define MLVC_YH 64u
#define MLVC_YW 64u
#define MLVC_BS 8u

static int g_active;
static int g_have_input;
static int g_mode; /* 0 = phase, 1 = mlvc encoder, 2 = mlvc decoder */
static rknn_tensor_mem *g_in_mem[4];
static rknn_tensor_mem *g_out_mem[2];
static uint32_t g_io_mem_inputs;
static int g_run_count;

/* Detect fake-model flavour from the model blob header. */
static int detect_mode(const void *model, uint32_t size)
{
    const char *bytes = model;
    uint32_t i;
    if (!bytes || size < 8)
        return 0;
    for (i = 0; i + 8 <= size; i++) {
        if (memcmp(bytes + i, "MLVC-ENC", 8) == 0)
            return 1;
        if (memcmp(bytes + i, "MLVC-DEC", 8) == 0)
            return 2;
    }
    return 0;
}

int rknn_init(rknn_context *context, void *model, uint32_t size,
              uint32_t flags, void *extend) {
    (void)flags;
    (void)extend;
    if (!context || !model || !size)
        return -1;
    *context = 1;
    g_active = 1;
    g_have_input = 0;
    g_io_mem_inputs = 0;
    g_run_count = 0;
    memset(g_in_mem, 0, sizeof(g_in_mem));
    memset(g_out_mem, 0, sizeof(g_out_mem));
    g_mode = detect_mode(model, size);
    return RKNN_SUCC;
}

int rknn_destroy(rknn_context context) {
    uint32_t i;
    if (!context)
        return -1;
    g_active = 0;
    g_have_input = 0;
    for (i = 0; i < 4; i++)
        g_in_mem[i] = NULL;
    for (i = 0; i < 2; i++)
        g_out_mem[i] = NULL;
    return RKNN_SUCC;
}

static void fill_mlvc_enc_input(uint32_t i, rknn_tensor_attr *attr)
{
    attr->n_dims = 4;
    attr->fmt = RKNN_TENSOR_NHWC;
    attr->type = RKNN_TENSOR_FLOAT16;
    if (i == 0) {
        strcpy(attr->name, "x");
        attr->dims[0] = 1;
        attr->dims[1] = MLVC_IMG_H;
        attr->dims[2] = MLVC_IMG_W;
        attr->dims[3] = 3;
        attr->n_elems = 3 * MLVC_IMG_H * MLVC_IMG_W;
    } else {
        strcpy(attr->name, "ref_feature");
        attr->dims[0] = 1;
        attr->dims[1] = MLVC_REF_H;
        attr->dims[2] = MLVC_REF_W;
        attr->dims[3] = MLVC_REF_C;
        attr->n_elems = MLVC_REF_C * MLVC_REF_H * MLVC_REF_W;
    }
    attr->size = attr->n_elems * 2;
}

static void fill_mlvc_dec_input(uint32_t i, rknn_tensor_attr *attr)
{
    attr->n_dims = 4;
    attr->fmt = RKNN_TENSOR_NHWC;
    attr->type = RKNN_TENSOR_FLOAT16;
    if (i == 0) {
        strcpy(attr->name, "z_raw");
        attr->dims[3] = MLVC_ZC;
        attr->dims[1] = MLVC_ZH;
        attr->dims[2] = MLVC_ZW;
    } else if (i == 1) {
        strcpy(attr->name, "y_raw_0");
        attr->dims[3] = MLVC_YC;
        attr->dims[1] = MLVC_YH;
        attr->dims[2] = MLVC_YW;
    } else if (i == 2) {
        strcpy(attr->name, "y_raw_1");
        attr->dims[3] = MLVC_YC;
        attr->dims[1] = MLVC_YH;
        attr->dims[2] = MLVC_YW;
    } else {
        strcpy(attr->name, "ref_feature");
        attr->dims[3] = MLVC_REF_C;
        attr->dims[1] = MLVC_REF_H;
        attr->dims[2] = MLVC_REF_W;
    }
    attr->dims[0] = 1;
    attr->n_elems = attr->dims[1] * attr->dims[2] * attr->dims[3];
    attr->size = attr->n_elems * 2;
}

static void fill_mlvc_enc_output(uint32_t i, rknn_tensor_attr *attr)
{
    attr->n_dims = 4;
    attr->fmt = RKNN_TENSOR_NCHW;
    attr->type = RKNN_TENSOR_FLOAT16;
    if (i == 0) {
        strcpy(attr->name, "feature");
        attr->dims[1] = MLVC_REF_C;
        attr->dims[2] = MLVC_REF_H;
        attr->dims[3] = MLVC_REF_W;
    } else if (i == 1) {
        strcpy(attr->name, "z_raw");
        attr->dims[1] = MLVC_ZC;
        attr->dims[2] = MLVC_ZH;
        attr->dims[3] = MLVC_ZW;
    } else {
        strcpy(attr->name, i == 2 ? "y_raw_0" : "y_raw_1");
        attr->dims[1] = MLVC_YC;
        attr->dims[2] = MLVC_YH;
        attr->dims[3] = MLVC_YW;
    }
    attr->dims[0] = 1;
    attr->n_elems = attr->dims[1] * attr->dims[2] * attr->dims[3];
    attr->size = attr->n_elems * 2;
}

static void fill_mlvc_dec_output(uint32_t i, rknn_tensor_attr *attr)
{
    attr->n_dims = 4;
    attr->fmt = RKNN_TENSOR_NCHW;
    attr->type = RKNN_TENSOR_FLOAT16;
    if (i == 0) {
        strcpy(attr->name, "x_hat");
        attr->dims[1] = 3 * MLVC_BS * MLVC_BS;
        attr->dims[2] = MLVC_IMG_H / MLVC_BS;
        attr->dims[3] = MLVC_IMG_W / MLVC_BS;
    } else {
        strcpy(attr->name, "feature");
        attr->dims[1] = MLVC_REF_C;
        attr->dims[2] = MLVC_REF_H;
        attr->dims[3] = MLVC_REF_W;
    }
    attr->dims[0] = 1;
    attr->n_elems = attr->dims[1] * attr->dims[2] * attr->dims[3];
    attr->size = attr->n_elems * 2;
}

int rknn_query(rknn_context context, rknn_query_cmd cmd,
               void *info, uint32_t size) {
    rknn_tensor_attr *attr;
    if (!g_active || !context || !info)
        return -1;
    if (cmd == RKNN_QUERY_IN_OUT_NUM) {
        rknn_input_output_num *num = info;
        if (size < sizeof(*num)) return -1;
        if (g_mode == 0) {
            num->n_input = 1;
            num->n_output = 1;
        } else if (g_mode == 1) {
            num->n_input = 2;
            num->n_output = 4;
        } else {
            num->n_input = 4;
            num->n_output = 2;
        }
        return RKNN_SUCC;
    }
    attr = info;
    if (size < sizeof(*attr)) return -1;

    if (g_mode == 0) {
        memset(attr, 0, sizeof(*attr));
        attr->index = 0;
        attr->n_dims = 4;
        if (cmd == RKNN_QUERY_INPUT_ATTR) {
            attr->dims[0] = 1;
            attr->dims[1] = CORE_H;
            attr->dims[2] = CORE_W;
            attr->dims[3] = 12;
            attr->n_elems = 12 * CORE_H * CORE_W;
            attr->size = attr->n_elems;
            attr->fmt = RKNN_TENSOR_NHWC;
            attr->type = RKNN_TENSOR_UINT8;
            return RKNN_SUCC;
        }
        if (cmd == RKNN_QUERY_OUTPUT_ATTR) {
            attr->dims[0] = 1;
            attr->dims[1] = 108;
            attr->dims[2] = CORE_H;
            attr->dims[3] = CORE_W;
            attr->n_elems = 108 * CORE_H * CORE_W;
            attr->size = attr->n_elems * sizeof(float);
            attr->fmt = RKNN_TENSOR_NCHW;
            attr->type = RKNN_TENSOR_FLOAT32;
            return RKNN_SUCC;
        }
        return -1;
    }

    /* MLVC modes: every query returns a full attr for the given index. */
    {
        uint32_t idx = attr->index;
        memset(attr, 0, sizeof(*attr));
        attr->index = idx;
        uint32_t n_in = g_mode == 1 ? 2 : 4;
        uint32_t n_out = g_mode == 1 ? 4 : 2;
        if (cmd == RKNN_QUERY_INPUT_ATTR) {
            if (idx >= n_in) return -1;
            if (g_mode == 1)
                fill_mlvc_enc_input(idx, attr);
            else
                fill_mlvc_dec_input(idx, attr);
            return RKNN_SUCC;
        }
        if (cmd == RKNN_QUERY_OUTPUT_ATTR) {
            if (idx >= n_out) return -1;
            if (g_mode == 1)
                fill_mlvc_enc_output(idx, attr);
            else
                fill_mlvc_dec_output(idx, attr);
            return RKNN_SUCC;
        }
        if (cmd == RKNN_QUERY_NATIVE_NHWC_INPUT_ATTR) {
            if (g_mode != 1 || idx != 0) return -1;
            fill_mlvc_enc_input(idx, attr);
            attr->w_stride = attr->dims[2];
            attr->size_with_stride = attr->size;
            return RKNN_SUCC;
        }
        if (cmd == RKNN_QUERY_NATIVE_NC1HWC2_INPUT_ATTR) {
            if (g_mode != 1 || idx != 1) return -1;
            /* native NC1HWC2 packing of the ref feature: C=96 → C1=12,C2=8 */
            attr->n_dims = 5;
            attr->dims[0] = 1;
            attr->dims[1] = 12;
            attr->dims[2] = MLVC_REF_H;
            attr->dims[3] = MLVC_REF_W;
            attr->dims[4] = 8;
            attr->n_elems = 12 * MLVC_REF_H * MLVC_REF_W * 8;
            attr->size = attr->n_elems * 2;
            attr->size_with_stride = attr->size;
            attr->w_stride = MLVC_REF_W;
            attr->fmt = RKNN_TENSOR_NHWC;
            attr->type = RKNN_TENSOR_FLOAT16;
            strcpy(attr->name, "ref_feature");
            return RKNN_SUCC;
        }
        if (cmd == RKNN_QUERY_NATIVE_NC1HWC2_OUTPUT_ATTR) {
            if (g_mode == 1 || idx >= 2) return -1;
            if (idx == 0) {
                /* x_hat: 逻辑 NCHW [1, 3*bs*bs, H/bs, W/bs] →
                 * NC1HWC2 [1, 3*bs*bs/8, H/bs, W/bs, 8] */
                attr->n_dims = 5;
                attr->dims[0] = 1;
                attr->dims[1] = 3 * MLVC_BS * MLVC_BS / 8;
                attr->dims[2] = MLVC_IMG_H / MLVC_BS;
                attr->dims[3] = MLVC_IMG_W / MLVC_BS;
                attr->dims[4] = 8;
                attr->n_elems = attr->dims[1] * attr->dims[2] *
                                attr->dims[3] * attr->dims[4];
                attr->size = attr->n_elems * 2;
                attr->size_with_stride = attr->size;
                attr->w_stride = attr->dims[3];
                attr->fmt = RKNN_TENSOR_NHWC;
                attr->type = RKNN_TENSOR_FLOAT16;
                strcpy(attr->name, "x_hat");
            } else {
                attr->n_dims = 5;
                attr->dims[0] = 1;
                attr->dims[1] = 12;
                attr->dims[2] = MLVC_REF_H;
                attr->dims[3] = MLVC_REF_W;
                attr->dims[4] = 8;
                attr->n_elems = 12 * MLVC_REF_H * MLVC_REF_W * 8;
                attr->size = attr->n_elems * 2;
                attr->size_with_stride = attr->size;
                attr->w_stride = MLVC_REF_W;
                attr->fmt = RKNN_TENSOR_NHWC;
                attr->type = RKNN_TENSOR_FLOAT16;
                strcpy(attr->name, "feature");
            }
            return RKNN_SUCC;
        }
    }
    return -1;
}

int rknn_inputs_set(rknn_context context, uint32_t count,
                    rknn_input inputs[]) {
    const unsigned char *bytes;
    size_t pixel, channel;
    if (!g_active || !context || count != 1 || !inputs || !inputs[0].buf ||
        inputs[0].size != 12 * CORE_H * CORE_W ||
        inputs[0].fmt != RKNN_TENSOR_NHWC)
        return -1;
    bytes = inputs[0].buf;
    for (pixel = 0; pixel < CORE_H * CORE_W; ++pixel) {
        for (channel = 0; channel < 12; ++channel) {
            unsigned char expected = channel < 4 ? 80 : 128;
            if (bytes[pixel * 12 + channel] != expected)
                return -1;
        }
    }
    g_have_input = 1;
    return RKNN_SUCC;
}

int rknn_run(rknn_context context, void *extend) {
    (void)extend;
    if (!g_active || !context)
        return -1;
    if (g_mode == 0 && !g_have_input)
        return -1;
    g_run_count++;
    return RKNN_SUCC;
}

int rknn_outputs_get(rknn_context context, uint32_t count,
                     rknn_output outputs[], void *extend) {
    size_t elements = 108 * CORE_H * CORE_W;
    size_t plane = CORE_H * CORE_W;
    size_t channel, element;
    float *residual;
    (void)extend;
    if (!g_active || !context)
        return -1;
    if (g_mode != 0) {
        /* MLVC encoder standard outputs: all-zero fp16 latents. */
        uint32_t i;
        for (i = 0; i < count; i++) {
            rknn_tensor_attr a;
            memset(&a, 0, sizeof(a));
            a.index = outputs[i].index;
            if (rknn_query(context, RKNN_QUERY_OUTPUT_ATTR, &a,
                           sizeof(a)) != RKNN_SUCC)
                return -1;
            if (!outputs[i].is_prealloc) {
                outputs[i].buf = calloc(1, a.size);
                if (!outputs[i].buf)
                    return -1;
            }
            memset(outputs[i].buf, 0, a.size);
            outputs[i].size = a.size;
        }
        return RKNN_SUCC;
    }
    if (count != 1 || !outputs || !outputs[0].want_float)
        return -1;
    outputs[0].buf = calloc(elements, sizeof(float));
    if (!outputs[0].buf)
        return -1;
    residual = outputs[0].buf;
    for (channel = 0; channel < 108; ++channel) {
        float value = channel < 36 ? 1.0f : channel < 72 ? 2.0f : 3.0f;
        for (element = 0; element < plane; ++element)
            residual[channel * plane + element] = value;
    }
    outputs[0].size = (uint32_t)(elements * sizeof(float));
    return RKNN_SUCC;
}

int rknn_outputs_release(rknn_context context, uint32_t count,
                         rknn_output outputs[]) {
    uint32_t i;
    if (!context || !outputs)
        return -1;
    for (i = 0; i < count; i++) {
        if (!outputs[i].is_prealloc)
            free(outputs[i].buf);
        outputs[i].buf = NULL;
        outputs[i].size = 0;
    }
    return RKNN_SUCC;
}

int rknn_set_core_mask(rknn_context context, rknn_core_mask core_mask) {
    (void)core_mask;
    return g_active && context ? RKNN_SUCC : -1;
}

rknn_tensor_mem *rknn_create_mem(rknn_context context, uint32_t size) {
    rknn_tensor_mem *mem;
    (void)context;
    if (!size)
        return NULL;
    mem = calloc(1, sizeof(*mem));
    if (!mem)
        return NULL;
    mem->virt_addr = calloc(1, size);
    if (!mem->virt_addr) {
        free(mem);
        return NULL;
    }
    return mem;
}

int rknn_destroy_mem(rknn_context context, rknn_tensor_mem *mem) {
    (void)context;
    if (!mem)
        return -1;
    free(mem->virt_addr);
    free(mem);
    return RKNN_SUCC;
}

int rknn_set_io_mem(rknn_context context, rknn_tensor_mem *mem,
                    rknn_tensor_attr *attr) {
    if (!g_active || !context || !mem || !attr)
        return -1;
    /* Track by tensor index (input or output) for the fake run paths. */
    if (g_mode == 1 && attr->index < 2)
        g_in_mem[attr->index] = mem;
    if (g_mode == 2) {
        if (attr->index < 4)
            g_in_mem[attr->index] = mem;
        else if (attr->index < 6)
            g_out_mem[attr->index - 4] = mem;
    }
    mem->n_elems = attr->n_elems ? attr->n_elems : attr->size / 2u;
    g_io_mem_inputs++;
    return RKNN_SUCC;
}

int rknn_mem_sync(rknn_context context, rknn_tensor_mem *mem,
                  rknn_mem_sync_mode mode) {
    (void)mode;
    return g_active && context && mem ? RKNN_SUCC : -1;
}
