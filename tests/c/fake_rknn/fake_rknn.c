/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "rknn_api.h"

#include <stdlib.h>
#include <string.h>

#define CORE_W 4u
#define CORE_H 4u

static int g_active;
static int g_have_input;

int rknn_init(rknn_context *context, void *model, uint32_t size,
              uint32_t flags, void *extend) {
    (void)flags;
    (void)extend;
    if (!context || !model || !size)
        return -1;
    *context = 1;
    g_active = 1;
    g_have_input = 0;
    return RKNN_SUCC;
}

int rknn_destroy(rknn_context context) {
    if (!context)
        return -1;
    g_active = 0;
    g_have_input = 0;
    return RKNN_SUCC;
}

int rknn_query(rknn_context context, rknn_query_cmd cmd,
               void *info, uint32_t size) {
    rknn_tensor_attr *attr;
    if (!g_active || !context || !info)
        return -1;
    if (cmd == RKNN_QUERY_IN_OUT_NUM) {
        rknn_input_output_num *num = info;
        if (size < sizeof(*num)) return -1;
        num->n_input = 1;
        num->n_output = 1;
        return RKNN_SUCC;
    }
    attr = info;
    if (size < sizeof(*attr)) return -1;
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
    return g_active && context && g_have_input ? RKNN_SUCC : -1;
}

int rknn_outputs_get(rknn_context context, uint32_t count,
                     rknn_output outputs[], void *extend) {
    size_t elements = 108 * CORE_H * CORE_W;
    size_t plane = CORE_H * CORE_W;
    size_t channel, element;
    float *residual;
    (void)extend;
    if (!g_active || !context || count != 1 || !outputs ||
        !outputs[0].want_float)
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
    if (!context || count != 1 || !outputs)
        return -1;
    free(outputs[0].buf);
    outputs[0].buf = NULL;
    outputs[0].size = 0;
    return RKNN_SUCC;
}
