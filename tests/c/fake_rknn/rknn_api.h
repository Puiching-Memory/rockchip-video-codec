/* Minimal test-only RKNN Runtime ABI used by test_backend_rknn. */
#ifndef TEST_FAKE_RKNN_API_H
#define TEST_FAKE_RKNN_API_H

#include <stdint.h>

#define RKNN_SUCC 0

typedef uint64_t rknn_context;

typedef enum rknn_tensor_format {
    RKNN_TENSOR_NCHW = 0,
    RKNN_TENSOR_NHWC = 1,
    RKNN_TENSOR_UNDEFINED = 2,
} rknn_tensor_format;

typedef enum rknn_tensor_type {
    RKNN_TENSOR_FLOAT32 = 0,
    RKNN_TENSOR_UINT8 = 3,
} rknn_tensor_type;

typedef enum rknn_query_cmd {
    RKNN_QUERY_IN_OUT_NUM = 0,
    RKNN_QUERY_INPUT_ATTR = 1,
    RKNN_QUERY_OUTPUT_ATTR = 2,
} rknn_query_cmd;

typedef struct rknn_input_output_num {
    uint32_t n_input;
    uint32_t n_output;
} rknn_input_output_num;

typedef struct rknn_tensor_attr {
    uint32_t index;
    uint32_t n_dims;
    uint32_t dims[16];
    uint32_t n_elems;
    uint32_t size;
    rknn_tensor_format fmt;
    rknn_tensor_type type;
} rknn_tensor_attr;

typedef struct rknn_input {
    uint32_t index;
    void *buf;
    uint32_t size;
    uint8_t pass_through;
    rknn_tensor_type type;
    rknn_tensor_format fmt;
} rknn_input;

typedef struct rknn_output {
    uint32_t index;
    void *buf;
    uint32_t size;
    uint8_t want_float;
    uint8_t is_prealloc;
} rknn_output;

int rknn_init(rknn_context *context, void *model, uint32_t size,
              uint32_t flags, void *extend);
int rknn_destroy(rknn_context context);
int rknn_query(rknn_context context, rknn_query_cmd cmd,
               void *info, uint32_t size);
int rknn_inputs_set(rknn_context context, uint32_t count,
                    rknn_input inputs[]);
int rknn_run(rknn_context context, void *extend);
int rknn_outputs_get(rknn_context context, uint32_t count,
                     rknn_output outputs[], void *extend);
int rknn_outputs_release(rknn_context context, uint32_t count,
                         rknn_output outputs[]);

#endif
