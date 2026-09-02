/* 查询 .rknn 模型的输入/输出 tensor 属性 */
#include <stdio.h>
#include <stdlib.h>
#include "rknn_api.h"

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 3;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *data = malloc(sz);
    if (fread(data, 1, sz, f) != (size_t)sz) return 4;
    fclose(f);

    rknn_context ctx;
    int ret = rknn_init(&ctx, data, (uint32_t)sz, 0, NULL);
    if (ret != RKNN_SUCC) { printf("rknn_init failed: %d\n", ret); return 5; }

    rknn_input_output_num io;
    rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));
    printf("inputs=%u outputs=%u\n", io.n_input, io.n_output);

    for (uint32_t i = 0; i < io.n_input; ++i) {
        rknn_tensor_attr a;
        memset(&a, 0, sizeof(a));
        a.index = i;
        rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &a, sizeof(a));
        printf("in[%u] %s: n_dims=%d dims=[%d,%d,%d,%d] fmt=%d type=%d qnt=%d fl=%d zp=%d scale=%.6f n_elems=%u size=%u\n",
               i, a.name, a.n_dims, a.dims[0], a.dims[1], a.dims[2], a.dims[3],
               (int)a.fmt, (int)a.type, (int)a.qnt_type, a.fl, a.zp, a.scale,
               a.n_elems, a.size);
    }
    for (uint32_t i = 0; i < io.n_output; ++i) {
        rknn_tensor_attr a;
        memset(&a, 0, sizeof(a));
        a.index = i;
        rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &a, sizeof(a));
        printf("out[%u] %s: n_dims=%d dims=[%d,%d,%d,%d] fmt=%d type=%d qnt=%d fl=%d zp=%d scale=%.6f n_elems=%u size=%u\n",
               i, a.name, a.n_dims, a.dims[0], a.dims[1], a.dims[2], a.dims[3],
               (int)a.fmt, (int)a.type, (int)a.qnt_type, a.fl, a.zp, a.scale,
               a.n_elems, a.size);
    }
    rknn_destroy(ctx);
    free(data);
    return 0;
}
