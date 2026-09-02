/* Minimal test-only RKNN Runtime ABI used by test_backend_rknn and
 * test_backend_mlvc (with native zero-copy memory API). */
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
    RKNN_TENSOR_FLOAT16 = 4,
    RKNN_TENSOR_INT8 = 5,
    RKNN_TENSOR_UINT8 = 3,
} rknn_tensor_type;

typedef enum rknn_qnt_type {
    RKNN_TENSOR_QNT_NONE = 0,
    RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC = 1,
} rknn_qnt_type;

typedef enum rknn_query_cmd {
    RKNN_QUERY_IN_OUT_NUM = 0,
    RKNN_QUERY_INPUT_ATTR = 1,
    RKNN_QUERY_OUTPUT_ATTR = 2,
    RKNN_QUERY_NATIVE_NHWC_INPUT_ATTR = 3,
    RKNN_QUERY_NATIVE_NC1HWC2_INPUT_ATTR = 4,
    RKNN_QUERY_NATIVE_NC1HWC2_OUTPUT_ATTR = 5,
} rknn_query_cmd;

typedef enum rknn_mem_sync_mode {
    RKNN_MEMORY_SYNC_TO_DEVICE = 0,
    RKNN_MEMORY_SYNC_FROM_DEVICE = 1,
} rknn_mem_sync_mode;

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
    uint32_t w_stride;
    uint32_t size_with_stride;
    uint8_t pass_through;
    uint8_t h_stride;
    rknn_tensor_format fmt;
    rknn_tensor_type type;
    rknn_qnt_type qnt_type;
    int32_t fl;
    int32_t zp;
    float scale;
    uint32_t w_pad;
    uint32_t h_pad;
    char name[256];
} rknn_tensor_attr;

typedef struct rknn_tensor_mem {
    void *virt_addr;
    uint32_t n_elems;
    uint64_t physical_address;
} rknn_tensor_mem;

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

typedef enum rknn_core_mask {
    RKNN_NPU_CORE_AUTO = 0,
    RKNN_NPU_CORE_0 = 1,
    RKNN_NPU_CORE_1 = 2,
    RKNN_NPU_CORE_0_1 = 3,
    RKNN_NPU_CORE_2 = 4,
    RKNN_NPU_CORE_0_1_2 = 7,
} rknn_core_mask;

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
int rknn_set_core_mask(rknn_context context, rknn_core_mask core_mask);
rknn_tensor_mem *rknn_create_mem(rknn_context context, uint32_t size);
int rknn_destroy_mem(rknn_context context, rknn_tensor_mem *mem);
int rknn_set_io_mem(rknn_context context, rknn_tensor_mem *mem,
                    rknn_tensor_attr *attr);
int rknn_mem_sync(rknn_context context, rknn_tensor_mem *mem,
                  rknn_mem_sync_mode mode);

#endif
