/* SPDX-License-Identifier: AGPL-3.0-or-later */
/** @file test_backend_mlvc.c Fake-runtime E2E tests for backend_mlvc.c.
 *
 * 1. encode：NV12 帧 → .mlvc BITSTREAM 记录（首帧附容器头），检查
 *    magic/qp/记录尺寸，且 fake NPU 全零 latent 下 rANS 码流可再解。
 * 2. decode：encode 输出记录回灌 → 流式 demux → 惰性 RKNN init →
 *    NV12 帧输出（全零 latent → 全灰帧，U=V=128）。
 */

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

#include "rkvc/backend.h"
#include "rkvc/context.h"
#include "rkvc/job.h"
#include "context_internal.h"
#include "rkmodel_layout.h"
#include "mlvc/container.h"
#include "mlvc/mlvc_pixel.h"

#define TEST_ROOT RKVC_TEST_TMPDIR "/rkvc_test_backend_mlvc"
#define MODEL_DIR TEST_ROOT "/models"
#define ENC_MODEL_PATH MODEL_DIR "/mlvc-enc.rkmodel"
#define DEC_MODEL_PATH MODEL_DIR "/mlvc-dec.rkmodel"

extern const rkvc_backend *rkvc_backend_query(void);
/* fake_rknn q_*_row 喂入捕获钩子 */
extern uint32_t fake_rknn_qrow_feed_count(void);
extern void fake_rknn_qrow_bitmap(uint64_t *lo, uint64_t *hi);

/* 小整数 → fp16 位型（0..71 精确） */
static uint16_t f16_of_uint(uint32_t v)
{
    uint32_t e = 0, m;
    if (!v)
        return 0;
    while ((1u << (e + 1)) <= v)
        e++;
    m = (v - (1u << e)) << (10 - e);
    return (uint16_t)(((15 + e) << 10) | m);
}

#define IMG_W 64
#define IMG_H 64
#define ZC 24
#define ZH 8
#define ZW 8
#define YC 24
#define YH 64
#define YW 64

/* ── .rkmodel blob 组装（多载荷：RKNN + PMF_GAUSSIAN + PMF_BITEST）── */

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

static void put_u32_le(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/** 构造最小合法 gaussian PMF1：128 个尺度分布 + 1 个 bypass 项。 */
static void put_pmf_gaussian(blob *pmf)
{
    static const int NUM_SCALES = 128; /* MLVC_PX_SCALE_MAX_IDX + 1 */
    int32_t lengths[NUM_SCALES + 1];
    int32_t offsets[NUM_SCALES + 1];
    int32_t table[(NUM_SCALES + 1) * 2];
    int i;
    unsigned char hdr[16];
    unsigned char tail[4 + 24];
    double scale_min = -3.0, scale_max = 3.0;
    uint32_t tail_u32;

    /* 每分布 2 个符号：数值符号 freq=65533，bypass 符号 freq=3。 */
    for (i = 0; i < NUM_SCALES + 1; i++) {
        lengths[i] = 2;
        offsets[i] = 0;
        table[2 * i] = 65533;
        table[2 * i + 1] = 3;
    }

    memcpy(hdr, "PMF1", 4);
    put_u32_le(hdr + 4, NUM_SCALES + 1);
    put_u32_le(hdr + 8, NUM_SCALES + 1);
    put_u32_le(hdr + 12, (NUM_SCALES + 1) * 2);
    put(pmf, hdr, sizeof(hdr));
    for (i = 0; i < NUM_SCALES + 1; i++)
        put(pmf, &lengths[i], 4);
    for (i = 0; i < NUM_SCALES + 1; i++)
        put(pmf, &offsets[i], 4);
    for (i = 0; i < (NUM_SCALES + 1) * 2; i++)
        put(pmf, &table[i], 4);
    /* tag=1 gaussian：scale_min/max f64 + scale_levels + index_space=1 */
    put_u32_le(tail, 1);
    put(pmf, tail, 4);
    put(pmf, &scale_min, 8);
    put(pmf, &scale_max, 8);
    tail_u32 = 128;
    put(pmf, &tail_u32, 4);
    tail_u32 = 1; /* index_space = true（encoder 要求） */
    put(pmf, &tail_u32, 4);
}

/** 构造最小合法 bitest PMF1：qp_num × channels 个双符号分布
 *  （符号 0 = 数值 0，符号 1 = bypass 哨兵；各半概率）。 */
static void put_pmf_bitest(blob *pmf)
{
    static const int QP_NUM = 64;   /* qp 0..63 */
    static const int CHANNELS = ZC; /* z 通道数 */
    int n = QP_NUM * CHANNELS;
    int32_t length = 2;
    int32_t offset = 0;
    int32_t freq[2] = {32768, 32768};
    unsigned char hdr[16];
    unsigned char tail[4 + 8];
    uint32_t tail_u32;
    int i;

    memcpy(hdr, "PMF1", 4);
    put_u32_le(hdr + 4, n);
    put_u32_le(hdr + 8, n);
    put_u32_le(hdr + 12, n * 2);
    put(pmf, hdr, sizeof(hdr));
    for (i = 0; i < n; i++)
        put(pmf, &length, 4);
    for (i = 0; i < n; i++)
        put(pmf, &offset, 4);
    for (i = 0; i < n; i++)
        put(pmf, freq, 8);
    /* tag=2 bitest */
    put_u32_le(tail, 2);
    put(pmf, tail, 4);
    tail_u32 = QP_NUM;
    put(pmf, &tail_u32, 4);
    tail_u32 = CHANNELS;
    put(pmf, &tail_u32, 4);
}

/** 构造 QPT1 行表载荷：每表 72 行 × 4 列 fp16；第 r 行全填 fp16(r)，
 *  fake 捕获行首值即可反推喂入的 q。 */
static void put_qptab(blob *q, const char *const *names, int n_tables)
{
    unsigned char hdr[8];
    unsigned char meta[8];
    int t, r, c;

    memcpy(hdr, "QPT1", 4);
    put_u32_le(hdr + 4, (uint32_t)n_tables);
    put(q, hdr, sizeof(hdr));
    for (t = 0; t < n_tables; t++) {
        put_u32(q, (uint32_t)strlen(names[t]));
        put(q, names[t], strlen(names[t]));
        put_u32_le(meta, 72);
        put_u32_le(meta + 4, 4);
        put(q, meta, sizeof(meta));
        for (r = 0; r < 72; r++) {
            uint16_t row[4];
            for (c = 0; c < 4; c++)
                row[c] = f16_of_uint((uint32_t)r);
            put(q, row, sizeof(row));
        }
    }
}

static void write_model(const char *path, const char *role,
                        const char *model_id,
                        const void *rknn_payload, size_t rknn_size,
                        const blob *qptab)
{
    blob tlv = {0}, pmf_g = {0}, pmf_b = {0}, file = {0};
    rkmodel_fixed fixed;
    rkmodel_payload_entry entries[4];
    size_t entries_off;
    uint32_t count = qptab ? 4u : 3u;
    FILE *fp;

    put_tlv(&tlv, RKMODEL_TAG_FAMILY, "mlvc");
    put_tlv(&tlv, RKMODEL_TAG_ROLE, role);
    put_tlv(&tlv, RKMODEL_TAG_ID, model_id);
    put_tlv(&tlv, RKMODEL_TAG_VERSION, "1.0.0");
    put_pmf_gaussian(&pmf_g);
    put_pmf_bitest(&pmf_b);

    memset(&fixed, 0, sizeof(fixed));
    fixed.magic = RKMODEL_MAGIC;
    fixed.format_version = RKMODEL_VERSION;
    fixed.header_len = (uint32_t)tlv.len;
    fixed.payload_count = count;
    fixed.payload_entry_size = sizeof(rkmodel_payload_entry);

    entries_off = RKMODEL_FIXED_SIZE + tlv.len +
                  sizeof(rkmodel_payload_entry) * count;
    memset(entries, 0, sizeof(entries));
    entries[0].kind = RKMODEL_PAYLOAD_RKNN;
    entries[0].offset = (uint32_t)entries_off;
    entries[0].length = (uint32_t)rknn_size;
    entries[1].kind = RKMODEL_PAYLOAD_PMF_GAUSSIAN;
    entries[1].offset = (uint32_t)(entries_off + rknn_size);
    entries[1].length = (uint32_t)pmf_g.len;
    entries[2].kind = RKMODEL_PAYLOAD_PMF_BITEST;
    entries[2].offset = (uint32_t)(entries_off + rknn_size + pmf_g.len);
    entries[2].length = (uint32_t)pmf_b.len;
    if (qptab) {
        entries[3].kind = RKMODEL_PAYLOAD_QPTAB;
        entries[3].offset =
            (uint32_t)(entries_off + rknn_size + pmf_g.len + pmf_b.len);
        entries[3].length = (uint32_t)qptab->len;
    }

    put(&file, &fixed, sizeof(fixed));
    put(&file, tlv.data, tlv.len);
    put(&file, entries, sizeof(rkmodel_payload_entry) * count);
    put(&file, rknn_payload, rknn_size);
    put(&file, pmf_g.data, pmf_g.len);
    put(&file, pmf_b.data, pmf_b.len);
    if (qptab)
        put(&file, qptab->data, qptab->len);

    (void)mkdir(TEST_ROOT, 0755);
    (void)mkdir(MODEL_DIR, 0755);
    fp = fopen(path, "wb");
    assert_non_null(fp);
    assert_int_equal(fwrite(file.data, 1, file.len, fp), file.len);
    fclose(fp);
    free(tlv.data);
    free(pmf_g.data);
    free(pmf_b.data);
    free(file.data);
}

static void write_models(void)
{
    /* fake RKNN 模型以标记字节区分 encoder/decoder。 */
    static const unsigned char enc_payload[16] = "MLVC-ENC-fake\0\0\0";
    static const unsigned char dec_payload[16] = "MLVC-DEC-fake\0\0\0";
    write_model(ENC_MODEL_PATH, "encoder", "mlvc-enc-test", enc_payload,
                sizeof(enc_payload), NULL);
    write_model(DEC_MODEL_PATH, "decoder", "mlvc-dec-test", dec_payload,
                sizeof(dec_payload), NULL);
}

/** qp-dynamic 模型：标记字节带 D 后缀，附 QPTAB 行表载荷。 */
static void write_models_dyn(void)
{
    static const unsigned char enc_payload[16] = "MLVC-ENCD-fake\0\0";
    static const unsigned char dec_payload[16] = "MLVC-DECD-fake\0\0";
    static const char *const enc_rows[3] = {
        "q_encoder_row", "q_decoder_row", "q_feature_row"
    };
    static const char *const dec_rows[3] = {
        "q_feature_row", "q_decoder_row", "q_recon_row"
    };
    blob qe = {0}, qd = {0};
    put_qptab(&qe, enc_rows, 3);
    put_qptab(&qd, dec_rows, 3);
    write_model(ENC_MODEL_PATH, "encoder", "mlvc-enc-test", enc_payload,
                sizeof(enc_payload), &qe);
    write_model(DEC_MODEL_PATH, "decoder", "mlvc-dec-test", dec_payload,
                sizeof(dec_payload), &qd);
    free(qe.data);
    free(qd.data);
}

/* ── 公共 job 装配 ── */

static rkvc_context *make_context(void)
{
    rkvc_context_options context_options;
    rkvc_context *context = NULL;
    rkvc_context_options_init(&context_options, sizeof(context_options));
    context_options.model_dir_override = MODEL_DIR;
    assert_int_equal(rkvc_context_create(&context_options, &context),
                     RKVC_STATUS_OK);
    assert_int_equal(rkvc_registry_add_backend(context, rkvc_backend_query()),
                     RKVC_STATUS_OK);
    return context;
}

#if defined(__GNUC__)
__attribute__((noinline))
#endif
static void clobber_released_stack(void)
{
    volatile unsigned char scratch[4096];
    size_t i;

    for (i = 0; i < sizeof(scratch); ++i)
        scratch[i] = (unsigned char)(i ^ 0xa5u);
}

/* 标准 MLVC 和 MLVC-S 的 channel/spatial repeat 不同，两者都必须
 * 保持与上游 repeat_interleave + 棋盘格展开一致。 */
static void test_mlvc_scale_profiles(void **state)
{
    int32_t z_standard[2 * 2 * 2] = {1, 2, 3, 4, 5, 6, 7, INT32_MIN};
    int32_t standard_s0[2 * 9 * 9];
    int32_t standard_s1[2 * 9 * 9];
    int32_t z_small[2 * 2 * 2] = {-2, -3, -4, -5, 9, 10, 11, 12};
    int32_t small_s0[4 * 5 * 5];
    int32_t small_s1[4 * 5 * 5];
    size_t standard_plane = 9 * 9;
    size_t small_plane = 5 * 5;
    (void)state;

    assert_int_equal(
        mlvc_px_extract_scales(z_standard, standard_s0, standard_s1,
                               2, 9, 9, 2, 2, 2, 2, 8, 7),
        0);
    assert_int_equal(standard_s0[0], 1);
    assert_int_equal(standard_s1[0], 5);
    assert_int_equal(standard_s0[1], 5);
    assert_int_equal(standard_s1[1], 1);
    assert_int_equal(standard_s0[8 * 9 + 8], 4);
    assert_int_equal(standard_s1[8 * 9 + 8], 7);
    assert_int_equal(standard_s0[standard_plane + 8 * 9 + 8], 4);
    assert_int_equal(
        mlvc_px_extract_scales(z_standard, standard_s0, standard_s1,
                               2, 9, 9, 1, 2, 2, 2, 8, 7),
        -1);

    assert_int_equal(
        mlvc_px_extract_scales(z_small, small_s0, small_s1,
                               4, 5, 5, 2, 2, 2, 4, 4, 10),
        0);
    assert_int_equal(small_s0[4 * 5 + 4], 5);
    assert_int_equal(small_s1[4 * 5 + 4], 10);
    assert_int_equal(small_s0[3 * small_plane + 4 * 5 + 4], 5);
    assert_int_equal(small_s1[3 * small_plane + 4 * 5 + 4], 10);
}

/* ── 测试 1：encode 产出合法 .mlvc 容器记录 ── */

static void test_mlvc_encode_writes_container_records(void **state) {
    rkvc_context *context;
    rkvc_request request;
    rkvc_job *job = NULL;
    rkvc_diag *diag = NULL;
    rkvc_frame_spec input_spec;
    rkvc_frame *input = NULL;
    rkvc_frame *output = NULL;
    rkvc_frame_desc desc;
    uint8_t *nv12;
    size_t i;
    uint32_t rec_size;
    (void)state;

    write_models();
    context = make_context();

    rkvc_request_init(&request, sizeof(request));
    request.operation = RKVC_OPERATION_ENCODE;
    request.codec = RKVC_CODEC_MLVC;
    request.input.kind = RKVC_ENDPOINT_FRAME_SINK;
    request.input.fmt = RKVC_FRAME_FMT_NV12;
    request.output.kind = RKVC_ENDPOINT_FRAME_SINK;
    request.output.fmt = RKVC_FRAME_FMT_BITSTREAM;
    request.width = IMG_W;
    request.height = IMG_H;
    request.quality.qp = 21;
    request.model_id = "mlvc-enc-test";
    assert_int_equal(rkvc_job_create(context, &request, &diag, &job),
                     RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_start(job, &diag), RKVC_STATUS_OK);

    nv12 = malloc((size_t)IMG_W * IMG_H * 3 / 2);
    assert_non_null(nv12);
    memset(nv12, 80, (size_t)IMG_W * IMG_H);
    memset(nv12 + (size_t)IMG_W * IMG_H, 128, (size_t)IMG_W * IMG_H / 2);

    memset(&input_spec, 0, sizeof(input_spec));
    input_spec.width = IMG_W;
    input_spec.height = IMG_H;
    input_spec.fmt = RKVC_FRAME_FMT_NV12;
    input_spec.domain = RKVC_MEM_DOMAIN_HOST;
    input_spec.stride = IMG_W;
    input_spec.ver_stride = IMG_H;
    assert_int_equal(rkvc_frame_wrap_host(&input_spec, nv12,
                                          (size_t)IMG_W * IMG_H * 3 / 2,
                                          &input), RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_push(job, input), RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_push_eos(job), RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_pull(job, &output), RKVC_STATUS_OK);

    assert_int_equal(rkvc_frame_get_desc(output, &desc), RKVC_STATUS_OK);
    assert_int_equal(desc.spec.fmt, RKVC_FRAME_FMT_BITSTREAM);
    assert_int_equal(desc.spec.domain, RKVC_MEM_DOMAIN_HOST);
    assert_true(desc.size > MLVC_HDR_SIZE + MLVC_REC_SIZE);
    assert_true(desc.flags & RKVC_FRAME_FLAG_KEYFRAME);

    /* 首帧输出 = 64B 头 + 16B 记录 + rANS 载荷 */
    assert_int_equal(memcmp(desc.data, "MLVC1", 5), 0);
    {
        mlvc_container_header hdr;
        assert_int_equal(mlvc_container_parse_header(desc.data, desc.size,
                                                     &hdr),
                         MLVC_HDR_SIZE);
        assert_int_equal(hdr.width, IMG_W);
        assert_int_equal(hdr.height, IMG_H);
        assert_int_equal(hdr.qp, 21);
        /* fake 几何 ZH=YH/8 → std 变体：gop 默认 64、LTR 关闭 */
        assert_int_equal(hdr.iframe_period, 64);
        assert_int_equal(hdr.ltr_period, 0);
        /* LTR 关闭时 PROACTIVE_LTR 不置位；无码控 → 无 CBR 位 */
        assert_int_equal(hdr.flags, 0);
    }
    {
        const uint8_t *rec = (const uint8_t *)desc.data + MLVC_HDR_SIZE;
        int32_t rec_q;
        uint32_t rec_flags;
        memcpy(&rec_size, rec, 4);
        memcpy(&rec_q, rec + 4, 4);
        memcpy(&rec_flags, rec + 8, 4);
        assert_true(rec_size > 0);
        assert_true((size_t)rec_size + MLVC_HDR_SIZE + MLVC_REC_SIZE ==
                    desc.size);
        assert_int_equal(rec_q, 21);
        assert_int_equal(rec_flags, MLVC_REC_KEYFRAME);
    }
    /* 载荷非全零（rANS flush 至少写状态字节） */
    {
        int nonzero = 0;
        const uint8_t *payload =
            (const uint8_t *)desc.data + MLVC_HDR_SIZE + MLVC_REC_SIZE;
        for (i = 0; i < rec_size; i++)
            if (payload[i]) {
                nonzero = 1;
                break;
            }
        assert_true(nonzero);
    }

    rkvc_frame_release(output);
    assert_int_equal(rkvc_job_pull(job, &output), RKVC_STATUS_EOF);
    assert_int_equal(rkvc_job_wait(job), RKVC_STATUS_OK);
    rkvc_job_destroy(job);
    rkvc_diag_release(diag);
    rkvc_context_destroy(context);
    free(nv12);
    /* input 帧所有权已随 job_push 转移给执行器，勿重复释放 */
}

/* ── 测试 2：encode → decode 往返 ── */

static void test_mlvc_encode_decode_roundtrip(void **state) {
    rkvc_context *context;
    rkvc_request enc_req, dec_req;
    rkvc_job *enc_job = NULL, *dec_job = NULL;
    rkvc_diag *enc_diag = NULL, *dec_diag = NULL;
    rkvc_frame_spec input_spec;
    rkvc_frame *input = NULL;
    rkvc_frame *bitstream = NULL, *frame = NULL;
    rkvc_frame_desc desc;
    uint8_t *nv12;
    size_t i;
    (void)state;

    write_models();
    context = make_context();

    /* encoder job */
    rkvc_request_init(&enc_req, sizeof(enc_req));
    enc_req.operation = RKVC_OPERATION_ENCODE;
    enc_req.codec = RKVC_CODEC_MLVC;
    enc_req.input.kind = RKVC_ENDPOINT_FRAME_SINK;
    enc_req.input.fmt = RKVC_FRAME_FMT_NV12;
    enc_req.output.kind = RKVC_ENDPOINT_FRAME_SINK;
    enc_req.output.fmt = RKVC_FRAME_FMT_BITSTREAM;
    enc_req.width = IMG_W;
    enc_req.height = IMG_H;
    enc_req.quality.qp = 21;
    enc_req.model_id = "mlvc-enc-test";
    assert_int_equal(rkvc_job_create(context, &enc_req, &enc_diag, &enc_job),
                     RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_start(enc_job, &enc_diag), RKVC_STATUS_OK);

    nv12 = malloc((size_t)IMG_W * IMG_H * 3 / 2);
    assert_non_null(nv12);
    memset(nv12, 100, (size_t)IMG_W * IMG_H);
    memset(nv12 + (size_t)IMG_W * IMG_H, 128, (size_t)IMG_W * IMG_H / 2);

    memset(&input_spec, 0, sizeof(input_spec));
    input_spec.width = IMG_W;
    input_spec.height = IMG_H;
    input_spec.fmt = RKVC_FRAME_FMT_NV12;
    input_spec.domain = RKVC_MEM_DOMAIN_HOST;
    input_spec.stride = IMG_W;
    input_spec.ver_stride = IMG_H;
    assert_int_equal(rkvc_frame_wrap_host(&input_spec, nv12,
                                          (size_t)IMG_W * IMG_H * 3 / 2,
                                          &input), RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_push(enc_job, input), RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_push_eos(enc_job), RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_pull(enc_job, &bitstream), RKVC_STATUS_OK);

    /* decoder job：输入 BITSTREAM 帧，输出 NV12 */
    rkvc_request_init(&dec_req, sizeof(dec_req));
    dec_req.operation = RKVC_OPERATION_DECODE;
    dec_req.codec = RKVC_CODEC_MLVC;
    dec_req.input.kind = RKVC_ENDPOINT_FRAME_SINK;
    dec_req.input.fmt = RKVC_FRAME_FMT_BITSTREAM;
    dec_req.output.kind = RKVC_ENDPOINT_FRAME_SINK;
    dec_req.output.fmt = RKVC_FRAME_FMT_NV12;
    dec_req.model_id = "mlvc-dec-test";
    assert_int_equal(rkvc_job_create(context, &dec_req, &dec_diag, &dec_job),
                     RKVC_STATUS_OK);
    /* bind_model() 的描述符只在回调期间有效；惰性初始化不得借用它。 */
    clobber_released_stack();
    assert_int_equal(rkvc_job_start(dec_job, &dec_diag), RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_push(dec_job, bitstream), RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_push_eos(dec_job), RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_pull(dec_job, &frame), RKVC_STATUS_OK);

    assert_int_equal(rkvc_frame_get_desc(frame, &desc), RKVC_STATUS_OK);
    assert_int_equal(desc.spec.fmt, RKVC_FRAME_FMT_NV12);
    assert_int_equal(desc.spec.domain, RKVC_MEM_DOMAIN_HOST);
    assert_int_equal(desc.spec.width, IMG_W);
    assert_int_equal(desc.spec.height, IMG_H);
    assert_int_equal(desc.size, (size_t)IMG_W * IMG_H * 3 / 2);

    /* fake NPU 全零 latent → x_hat 全零 → Y/U/V fp16 均 0 → NV12 全 0 */
    for (i = 0; i < desc.size; i++)
        assert_int_equal(((const uint8_t *)desc.data)[i], 0);

    rkvc_frame_release(frame);
    assert_int_equal(rkvc_job_pull(dec_job, &frame), RKVC_STATUS_EOF);
    assert_int_equal(rkvc_job_wait(dec_job), RKVC_STATUS_OK);

    rkvc_job_destroy(enc_job);
    rkvc_job_destroy(dec_job);
    rkvc_diag_release(enc_diag);
    rkvc_diag_release(dec_diag);
    rkvc_context_destroy(context);
    free(nv12);
    /* input/bitstream 帧所有权已随 job_push 转移给执行器 */
}

/* 解码器必须验证 rANS 严格 EOF，不能把带尾随机数据的载荷当成成功帧。 */
static void test_mlvc_decode_rejects_trailing_rans_data(void **state)
{
    rkvc_context *context;
    rkvc_request enc_req, dec_req;
    rkvc_job *enc_job = NULL, *dec_job = NULL;
    rkvc_diag *enc_diag = NULL, *dec_diag = NULL;
    rkvc_frame_spec spec;
    rkvc_frame *input = NULL, *bitstream = NULL, *corrupt_frame = NULL;
    rkvc_frame_desc desc;
    uint8_t *nv12, *corrupt;
    uint32_t payload_size;
    size_t corrupt_size;
    (void)state;

    write_models();
    context = make_context();

    rkvc_request_init(&enc_req, sizeof(enc_req));
    enc_req.operation = RKVC_OPERATION_ENCODE;
    enc_req.codec = RKVC_CODEC_MLVC;
    enc_req.input.kind = RKVC_ENDPOINT_FRAME_SINK;
    enc_req.input.fmt = RKVC_FRAME_FMT_NV12;
    enc_req.output.kind = RKVC_ENDPOINT_FRAME_SINK;
    enc_req.output.fmt = RKVC_FRAME_FMT_BITSTREAM;
    enc_req.width = IMG_W;
    enc_req.height = IMG_H;
    enc_req.quality.qp = 21;
    enc_req.model_id = "mlvc-enc-test";
    assert_int_equal(rkvc_job_create(context, &enc_req, &enc_diag, &enc_job),
                     RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_start(enc_job, &enc_diag), RKVC_STATUS_OK);

    nv12 = malloc((size_t)IMG_W * IMG_H * 3 / 2);
    assert_non_null(nv12);
    memset(nv12, 90, (size_t)IMG_W * IMG_H);
    memset(nv12 + (size_t)IMG_W * IMG_H, 128,
           (size_t)IMG_W * IMG_H / 2);
    memset(&spec, 0, sizeof(spec));
    spec.width = IMG_W;
    spec.height = IMG_H;
    spec.fmt = RKVC_FRAME_FMT_NV12;
    spec.domain = RKVC_MEM_DOMAIN_HOST;
    spec.stride = IMG_W;
    spec.ver_stride = IMG_H;
    assert_int_equal(rkvc_frame_wrap_host(
                         &spec, nv12, (size_t)IMG_W * IMG_H * 3 / 2, &input),
                     RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_push(enc_job, input), RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_push_eos(enc_job), RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_pull(enc_job, &bitstream), RKVC_STATUS_OK);
    assert_int_equal(rkvc_frame_get_desc(bitstream, &desc), RKVC_STATUS_OK);

    corrupt_size = desc.size + 1;
    corrupt = malloc(corrupt_size);
    assert_non_null(corrupt);
    memcpy(corrupt, desc.data, desc.size);
    corrupt[desc.size] = 0xa5;
    memcpy(&payload_size, corrupt + MLVC_HDR_SIZE, sizeof(payload_size));
    payload_size++;
    memcpy(corrupt + MLVC_HDR_SIZE, &payload_size, sizeof(payload_size));
    rkvc_frame_release(bitstream);

    memset(&spec, 0, sizeof(spec));
    spec.fmt = RKVC_FRAME_FMT_BITSTREAM;
    spec.domain = RKVC_MEM_DOMAIN_HOST;
    assert_int_equal(rkvc_frame_wrap_host(&spec, corrupt, corrupt_size,
                                          &corrupt_frame),
                     RKVC_STATUS_OK);

    rkvc_request_init(&dec_req, sizeof(dec_req));
    dec_req.operation = RKVC_OPERATION_DECODE;
    dec_req.codec = RKVC_CODEC_MLVC;
    dec_req.input.kind = RKVC_ENDPOINT_FRAME_SINK;
    dec_req.input.fmt = RKVC_FRAME_FMT_BITSTREAM;
    dec_req.output.kind = RKVC_ENDPOINT_FRAME_SINK;
    dec_req.output.fmt = RKVC_FRAME_FMT_NV12;
    dec_req.model_id = "mlvc-dec-test";
    assert_int_equal(rkvc_job_create(context, &dec_req, &dec_diag, &dec_job),
                     RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_start(dec_job, &dec_diag), RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_push(dec_job, corrupt_frame), RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_push_eos(dec_job), RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_wait(dec_job), RKVC_STATUS_FORMAT);
    assert_int_equal(rkvc_job_wait(enc_job), RKVC_STATUS_OK);

    rkvc_job_destroy(enc_job);
    rkvc_job_destroy(dec_job);
    rkvc_diag_release(enc_diag);
    rkvc_diag_release(dec_diag);
    rkvc_context_destroy(context);
    free(corrupt);
    free(nv12);
}

/* ── 测试 4：多帧 IDR 周期 + LTR 标记/恢复 flags ── */

static void parse_record(const uint8_t *rec, uint32_t *payload_size,
                         int32_t *q_index, uint32_t *flags)
{
    memcpy(payload_size, rec, 4);
    memcpy(q_index, rec + 4, 4);
    memcpy(flags, rec + 8, 4);
}

static rkvc_frame *wrap_nv12_frame(uint8_t *nv12)
{
    rkvc_frame_spec spec;
    rkvc_frame *frame = NULL;
    memset(&spec, 0, sizeof(spec));
    spec.width = IMG_W;
    spec.height = IMG_H;
    spec.fmt = RKVC_FRAME_FMT_NV12;
    spec.domain = RKVC_MEM_DOMAIN_HOST;
    spec.stride = IMG_W;
    spec.ver_stride = IMG_H;
    assert_int_equal(rkvc_frame_wrap_host(&spec, nv12,
                                          (size_t)IMG_W * IMG_H * 3 / 2,
                                          &frame), RKVC_STATUS_OK);
    return frame;
}

static void test_mlvc_idr_ltr_flags(void **state)
{
    enum { N = 7 };
    /* 期望 flags：f0 IDR；f2 MARK；f4/f6 MARK|RECOVERY（proactive） */
    static const uint32_t expect[N] = {
        MLVC_REC_KEYFRAME,
        0,
        MLVC_REC_LTR_MARK,
        0,
        MLVC_REC_LTR_MARK | MLVC_REC_LTR_RECOVERY,
        0,
        MLVC_REC_LTR_MARK | MLVC_REC_LTR_RECOVERY,
    };
    rkvc_context *context;
    rkvc_request request;
    rkvc_job *job = NULL;
    rkvc_diag *diag = NULL;
    rkvc_frame *output = NULL;
    rkvc_frame_desc desc;
    uint8_t *nv12;
    int i;
    (void)state;

    write_models();
    context = make_context();

    rkvc_request_init(&request, sizeof(request));
    request.operation = RKVC_OPERATION_ENCODE;
    request.codec = RKVC_CODEC_MLVC;
    request.input.kind = RKVC_ENDPOINT_FRAME_SINK;
    request.input.fmt = RKVC_FRAME_FMT_NV12;
    request.output.kind = RKVC_ENDPOINT_FRAME_SINK;
    request.output.fmt = RKVC_FRAME_FMT_BITSTREAM;
    request.width = IMG_W;
    request.height = IMG_H;
    request.quality.qp = 21;
    request.quality.ltr_period = 2;
    request.quality.ltr_start_idx = 2;
    request.model_id = "mlvc-enc-test";
    assert_int_equal(rkvc_job_create(context, &request, &diag, &job),
                     RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_start(job, &diag), RKVC_STATUS_OK);

    nv12 = malloc((size_t)IMG_W * IMG_H * 3 / 2);
    assert_non_null(nv12);
    memset(nv12, 70, (size_t)IMG_W * IMG_H);
    memset(nv12 + (size_t)IMG_W * IMG_H, 128, (size_t)IMG_W * IMG_H / 2);

    for (i = 0; i < N; i++) {
        rkvc_frame *input = wrap_nv12_frame(nv12);
        int st;
        do {
            st = rkvc_job_push(job, input);
        } while (st == RKVC_STATUS_AGAIN);
        assert_int_equal(st, RKVC_STATUS_OK);
    }
    assert_int_equal(rkvc_job_push_eos(job), RKVC_STATUS_OK);

    for (i = 0; i < N; i++) {
        const uint8_t *rec;
        uint32_t payload_size, flags;
        int32_t q_index;
        assert_int_equal(rkvc_job_pull(job, &output), RKVC_STATUS_OK);
        assert_int_equal(rkvc_frame_get_desc(output, &desc),
                         RKVC_STATUS_OK);
        rec = (const uint8_t *)desc.data;
        if (i == 0) {
            /* 首帧附容器头：LTR 配置在头内可见 */
            mlvc_container_header hdr;
            assert_int_equal(mlvc_container_parse_header(rec, desc.size,
                                                         &hdr),
                             MLVC_HDR_SIZE);
            assert_int_equal(hdr.ltr_period, 2);
            assert_int_equal(hdr.ltr_start_idx, 2);
            rec += MLVC_HDR_SIZE;
        }
        parse_record(rec, &payload_size, &q_index, &flags);
        assert_true(payload_size > 0);
        assert_int_equal(q_index, 21);
        assert_int_equal(flags, expect[i]);
        rkvc_frame_release(output);
    }
    assert_int_equal(rkvc_job_pull(job, &output), RKVC_STATUS_EOF);
    assert_int_equal(rkvc_job_wait(job), RKVC_STATUS_OK);
    rkvc_job_destroy(job);
    rkvc_diag_release(diag);
    rkvc_context_destroy(context);
    free(nv12);
}

/* ── 测试 5：CBR 极低码率 → P 帧丢帧，解码端重复帧保持帧数守恒 ── */

static void test_mlvc_drop_roundtrip(void **state)
{
    enum { N = 4 };
    rkvc_context *context;
    rkvc_request enc_req, dec_req;
    rkvc_job *enc_job = NULL, *dec_job = NULL;
    rkvc_diag *enc_diag = NULL, *dec_diag = NULL;
    rkvc_frame *input = NULL, *record = NULL, *frame = NULL;
    rkvc_frame_desc desc;
    uint8_t *nv12;
    int i;
    (void)state;

    write_models();
    context = make_context();

    rkvc_request_init(&enc_req, sizeof(enc_req));
    enc_req.operation = RKVC_OPERATION_ENCODE;
    enc_req.codec = RKVC_CODEC_MLVC;
    enc_req.input.kind = RKVC_ENDPOINT_FRAME_SINK;
    enc_req.input.fmt = RKVC_FRAME_FMT_NV12;
    enc_req.output.kind = RKVC_ENDPOINT_FRAME_SINK;
    enc_req.output.fmt = RKVC_FRAME_FMT_BITSTREAM;
    enc_req.width = IMG_W;
    enc_req.height = IMG_H;
    enc_req.quality.qp = 21;
    enc_req.quality.bitrate_bps = 1; /* 1 bps：所有 P 帧必丢 */
    enc_req.model_id = "mlvc-enc-test";
    assert_int_equal(rkvc_job_create(context, &enc_req, &enc_diag, &enc_job),
                     RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_start(enc_job, &enc_diag), RKVC_STATUS_OK);

    nv12 = malloc((size_t)IMG_W * IMG_H * 3 / 2);
    assert_non_null(nv12);
    memset(nv12, 60, (size_t)IMG_W * IMG_H);
    memset(nv12 + (size_t)IMG_W * IMG_H, 128, (size_t)IMG_W * IMG_H / 2);

    rkvc_request_init(&dec_req, sizeof(dec_req));
    dec_req.operation = RKVC_OPERATION_DECODE;
    dec_req.codec = RKVC_CODEC_MLVC;
    dec_req.input.kind = RKVC_ENDPOINT_FRAME_SINK;
    dec_req.input.fmt = RKVC_FRAME_FMT_BITSTREAM;
    dec_req.output.kind = RKVC_ENDPOINT_FRAME_SINK;
    dec_req.output.fmt = RKVC_FRAME_FMT_NV12;
    dec_req.model_id = "mlvc-dec-test";
    assert_int_equal(rkvc_job_create(context, &dec_req, &dec_diag, &dec_job),
                     RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_start(dec_job, &dec_diag), RKVC_STATUS_OK);

    for (i = 0; i < N; i++) {
        input = wrap_nv12_frame(nv12);
        assert_int_equal(rkvc_job_push(enc_job, input), RKVC_STATUS_OK);
        input = NULL;
        assert_int_equal(rkvc_job_pull(enc_job, &record), RKVC_STATUS_OK);
        assert_int_equal(rkvc_frame_get_desc(record, &desc), RKVC_STATUS_OK);
        if (i == 0) {
            /* I 帧不可丢：完整记录（头 + 记录 + 载荷） */
            assert_true(desc.size >
                        MLVC_HDR_SIZE + MLVC_REC_SIZE);
        } else {
            /* 丢帧标记：仅 16B 记录头，q_index = -1 */
            uint32_t payload_size, flags;
            int32_t q_index;
            assert_int_equal(desc.size, MLVC_REC_SIZE);
            parse_record((const uint8_t *)desc.data, &payload_size,
                         &q_index, &flags);
            assert_int_equal(payload_size, 0);
            assert_int_equal(q_index, MLVC_QINDEX_DROPPED);
            assert_int_equal(flags, 0);
        }
        /* 逐条喂给解码器 */
        assert_int_equal(rkvc_job_push(dec_job, record), RKVC_STATUS_OK);
        record = NULL;
    }
    assert_int_equal(rkvc_job_push_eos(enc_job), RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_push_eos(dec_job), RKVC_STATUS_OK);

    /* 帧数守恒：丢帧也各产出一帧（重复上一 NV12） */
    for (i = 0; i < N; i++) {
        assert_int_equal(rkvc_job_pull(dec_job, &frame), RKVC_STATUS_OK);
        assert_int_equal(rkvc_frame_get_desc(frame, &desc), RKVC_STATUS_OK);
        assert_int_equal(desc.spec.fmt, RKVC_FRAME_FMT_NV12);
        assert_int_equal(desc.size, (size_t)IMG_W * IMG_H * 3 / 2);
        rkvc_frame_release(frame);
    }
    assert_int_equal(rkvc_job_pull(dec_job, &frame), RKVC_STATUS_EOF);
    assert_int_equal(rkvc_job_wait(enc_job), RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_wait(dec_job), RKVC_STATUS_OK);

    rkvc_job_destroy(enc_job);
    rkvc_job_destroy(dec_job);
    rkvc_diag_release(enc_diag);
    rkvc_diag_release(dec_diag);
    rkvc_context_destroy(context);
    free(nv12);
}

/* ── 测试 6：容器头严格性格式校验 ── */

static void test_mlvc_header_strict_format(void **state)
{
    uint8_t hdr[MLVC_HDR_SIZE];
    mlvc_container_header out;
    (void)state;

    /* 合法头：由写入函数构造，解析回读字段一致 */
    {
        mlvc_container_header in = {0};
        in.width = IMG_W;
        in.height = IMG_H;
        in.fps_num = 30;
        in.fps_den = 1;
        in.qp = 21;
        in.iframe_period = 64;
        in.ltr_start_idx = 8;
        in.ltr_period = 64;
        in.flags = MLVC_HDR_FLAG_PROACTIVE_LTR;
        in.target_bitrate_bps = 500000;
        mlvc_container_write_header(hdr, &in);
        assert_int_equal(mlvc_container_parse_header(hdr, sizeof(hdr), &out),
                         MLVC_HDR_SIZE);
        assert_int_equal(out.width, IMG_W);
        assert_int_equal(out.height, IMG_H);
        assert_int_equal(out.qp, 21);
        assert_int_equal(out.iframe_period, 64);
        assert_int_equal(out.ltr_start_idx, 8);
        assert_int_equal(out.ltr_period, 64);
        assert_int_equal(out.flags, MLVC_HDR_FLAG_PROACTIVE_LTR);
        assert_int_equal(out.target_bitrate_bps, 500000);
    }
    /* 缓冲不足 → 0 */
    assert_int_equal(mlvc_container_parse_header(hdr, MLVC_HDR_SIZE - 1,
                                                 &out),
                     0);
    /* 格式标识字节不符 → 拒绝 */
    hdr[5] = 1;
    assert_int_equal(mlvc_container_parse_header(hdr, sizeof(hdr), &out), -1);
    hdr[5] = 3;
    assert_int_equal(mlvc_container_parse_header(hdr, sizeof(hdr), &out), -1);
    hdr[5] = MLVC_FORMAT;
    /* magic 不符 → 拒绝 */
    hdr[0] = 'X';
    assert_int_equal(mlvc_container_parse_header(hdr, sizeof(hdr), &out), -1);
    hdr[0] = 'M';
    /* 宽高为 0 → 拒绝 */
    memset(hdr + 8, 0, 4);
    assert_int_equal(mlvc_container_parse_header(hdr, sizeof(hdr), &out), -1);
}

/* ── 测试 7-9：qp-dynamic（q_*_row 图输入 + QPTAB 行表）────────── */

/* 恒 qp 往返：io_mem 路径按 q 喂行，无 rung 切换 */
static void test_mlvc_qp_dynamic_roundtrip(void **state)
{
    enum { N = 2 };
    rkvc_context *context;
    rkvc_request enc_req, dec_req;
    rkvc_job *enc_job = NULL, *dec_job = NULL;
    rkvc_diag *enc_diag = NULL, *dec_diag = NULL;
    rkvc_frame *input = NULL, *record = NULL, *frame = NULL;
    rkvc_frame_desc desc;
    uint8_t *nv12;
    uint64_t lo = 0, hi = 0;
    int i;
    (void)state;

    write_models_dyn();
    context = make_context();

    rkvc_request_init(&enc_req, sizeof(enc_req));
    enc_req.operation = RKVC_OPERATION_ENCODE;
    enc_req.codec = RKVC_CODEC_MLVC;
    enc_req.input.kind = RKVC_ENDPOINT_FRAME_SINK;
    enc_req.input.fmt = RKVC_FRAME_FMT_NV12;
    enc_req.output.kind = RKVC_ENDPOINT_FRAME_SINK;
    enc_req.output.fmt = RKVC_FRAME_FMT_BITSTREAM;
    enc_req.width = IMG_W;
    enc_req.height = IMG_H;
    enc_req.quality.qp = 21;
    enc_req.model_id = "mlvc-enc-test";
    assert_int_equal(rkvc_job_create(context, &enc_req, &enc_diag, &enc_job),
                     RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_start(enc_job, &enc_diag), RKVC_STATUS_OK);

    nv12 = malloc((size_t)IMG_W * IMG_H * 3 / 2);
    assert_non_null(nv12);
    memset(nv12, 100, (size_t)IMG_W * IMG_H);
    memset(nv12 + (size_t)IMG_W * IMG_H, 128, (size_t)IMG_W * IMG_H / 2);

    for (i = 0; i < N; i++) {
        input = wrap_nv12_frame(nv12);
        assert_int_equal(rkvc_job_push(enc_job, input), RKVC_STATUS_OK);
        input = NULL;
        assert_int_equal(rkvc_job_pull(enc_job, &record), RKVC_STATUS_OK);
        rkvc_frame_release(record);
        record = NULL;
    }
    assert_int_equal(rkvc_job_push_eos(enc_job), RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_wait(enc_job), RKVC_STATUS_OK);

    /* 编码端：恒 qp=21 → 仅首帧喂行（q 不变不重喂），位图只有 bit21 */
    fake_rknn_qrow_bitmap(&lo, &hi);
    assert_int_equal(lo, 1ull << 21);
    assert_int_equal(hi, 0);
    assert_int_equal(fake_rknn_qrow_feed_count(), 3);
    rkvc_job_destroy(enc_job);
    rkvc_diag_release(enc_diag);
    enc_job = NULL;
    enc_diag = NULL;

    /* 解码端：按记录头 q=21 喂行 */
    {
        rkvc_frame *stream = NULL;
        uint8_t *head;
        /* 重新编码一条完整流（容器头 + 2 记录） */
        rkvc_request_init(&enc_req, sizeof(enc_req));
        enc_req.operation = RKVC_OPERATION_ENCODE;
        enc_req.codec = RKVC_CODEC_MLVC;
        enc_req.input.kind = RKVC_ENDPOINT_FRAME_SINK;
        enc_req.input.fmt = RKVC_FRAME_FMT_NV12;
        enc_req.output.kind = RKVC_ENDPOINT_FRAME_SINK;
        enc_req.output.fmt = RKVC_FRAME_FMT_BITSTREAM;
        enc_req.width = IMG_W;
        enc_req.height = IMG_H;
        enc_req.quality.qp = 21;
        enc_req.model_id = "mlvc-enc-test";
        assert_int_equal(rkvc_job_create(context, &enc_req, &enc_diag,
                                         &enc_job), RKVC_STATUS_OK);
        assert_int_equal(rkvc_job_start(enc_job, &enc_diag),
                         RKVC_STATUS_OK);
        blob whole = {0};
        for (i = 0; i < N; i++) {
            input = wrap_nv12_frame(nv12);
            assert_int_equal(rkvc_job_push(enc_job, input),
                             RKVC_STATUS_OK);
            input = NULL;
            assert_int_equal(rkvc_job_pull(enc_job, &record),
                             RKVC_STATUS_OK);
            assert_int_equal(rkvc_frame_get_desc(record, &desc),
                             RKVC_STATUS_OK);
            put(&whole, desc.data, desc.size);
            rkvc_frame_release(record);
            record = NULL;
        }
        assert_int_equal(rkvc_job_push_eos(enc_job), RKVC_STATUS_OK);
        assert_int_equal(rkvc_job_wait(enc_job), RKVC_STATUS_OK);

        rkvc_request_init(&dec_req, sizeof(dec_req));
        dec_req.operation = RKVC_OPERATION_DECODE;
        dec_req.codec = RKVC_CODEC_MLVC;
        dec_req.input.kind = RKVC_ENDPOINT_FRAME_SINK;
        dec_req.input.fmt = RKVC_FRAME_FMT_BITSTREAM;
        dec_req.output.kind = RKVC_ENDPOINT_FRAME_SINK;
        dec_req.output.fmt = RKVC_FRAME_FMT_NV12;
        dec_req.model_id = "mlvc-dec-test";
        assert_int_equal(rkvc_job_create(context, &dec_req, &dec_diag,
                                         &dec_job), RKVC_STATUS_OK);
        assert_int_equal(rkvc_job_start(dec_job, &dec_diag),
                         RKVC_STATUS_OK);

        {
            rkvc_frame_spec spec;
            memset(&spec, 0, sizeof(spec));
            spec.fmt = RKVC_FRAME_FMT_BITSTREAM;
            spec.domain = RKVC_MEM_DOMAIN_HOST;
            head = whole.data;
            assert_int_equal(rkvc_frame_wrap_host(&spec, head, whole.len,
                                                  &stream), RKVC_STATUS_OK);
        }
        assert_int_equal(rkvc_job_push(dec_job, stream), RKVC_STATUS_OK);
        assert_int_equal(rkvc_job_push_eos(dec_job), RKVC_STATUS_OK);
        for (i = 0; i < N; i++) {
            assert_int_equal(rkvc_job_pull(dec_job, &frame),
                             RKVC_STATUS_OK);
            assert_int_equal(rkvc_frame_get_desc(frame, &desc),
                             RKVC_STATUS_OK);
            assert_int_equal(desc.spec.fmt, RKVC_FRAME_FMT_NV12);
            assert_int_equal(desc.size, (size_t)IMG_W * IMG_H * 3 / 2);
            rkvc_frame_release(frame);
        }
        assert_int_equal(rkvc_job_pull(dec_job, &frame), RKVC_STATUS_EOF);
        assert_int_equal(rkvc_job_wait(dec_job), RKVC_STATUS_OK);
        fake_rknn_qrow_bitmap(&lo, &hi);
        assert_int_equal(lo, 1ull << 21);
        assert_int_equal(hi, 0);
        /* stream 帧所有权已随 job_push 转移给执行器，不再 release */
        free(whole.data);
    }

    rkvc_job_destroy(enc_job);
    rkvc_job_destroy(dec_job);
    rkvc_diag_release(enc_diag);
    rkvc_diag_release(dec_diag);
    rkvc_context_destroy(context);
    free(nv12);
}

/* RC 逐帧 q：喂入位图必须与记录头 q_index 位图一致（host 输入路径） */
static void test_mlvc_qp_dynamic_rc_matches_records(void **state)
{
    enum { N = 6 };
    rkvc_context *context;
    rkvc_request enc_req;
    rkvc_job *enc_job = NULL;
    rkvc_diag *enc_diag = NULL;
    rkvc_frame *input = NULL, *record = NULL;
    rkvc_frame_desc desc;
    uint8_t *nv12;
    uint64_t expect = 0, lo = 0, hi = 0;
    int i, encoded = 0;
    (void)state;

    setenv("RKVC_MLVC_ENCODER_ZERO_COPY", "0", 1); /* 走 rknn_inputs_set */
    write_models_dyn();
    context = make_context();

    rkvc_request_init(&enc_req, sizeof(enc_req));
    enc_req.operation = RKVC_OPERATION_ENCODE;
    enc_req.codec = RKVC_CODEC_MLVC;
    enc_req.input.kind = RKVC_ENDPOINT_FRAME_SINK;
    enc_req.input.fmt = RKVC_FRAME_FMT_NV12;
    enc_req.output.kind = RKVC_ENDPOINT_FRAME_SINK;
    enc_req.output.fmt = RKVC_FRAME_FMT_BITSTREAM;
    enc_req.width = IMG_W;
    enc_req.height = IMG_H;
    enc_req.quality.qp = 21;
    enc_req.quality.bitrate_bps = 200000;
    enc_req.model_id = "mlvc-enc-test";
    assert_int_equal(rkvc_job_create(context, &enc_req, &enc_diag, &enc_job),
                     RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_start(enc_job, &enc_diag), RKVC_STATUS_OK);

    nv12 = malloc((size_t)IMG_W * IMG_H * 3 / 2);
    assert_non_null(nv12);
    memset(nv12, 80, (size_t)IMG_W * IMG_H);
    memset(nv12 + (size_t)IMG_W * IMG_H, 128, (size_t)IMG_W * IMG_H / 2);

    for (i = 0; i < N; i++) {
        uint32_t payload_size, flags;
        int32_t q_index;
        input = wrap_nv12_frame(nv12);
        assert_int_equal(rkvc_job_push(enc_job, input), RKVC_STATUS_OK);
        input = NULL;
        assert_int_equal(rkvc_job_pull(enc_job, &record), RKVC_STATUS_OK);
        assert_int_equal(rkvc_frame_get_desc(record, &desc), RKVC_STATUS_OK);
        /* 首帧记录前有 64B 容器头 */
        parse_record((const uint8_t *)desc.data +
                         (i == 0 ? MLVC_HDR_SIZE : 0),
                     &payload_size, &q_index, &flags);
        if (q_index >= 0) {
            expect |= 1ull << q_index;
            encoded++;
        }
        rkvc_frame_release(record);
        record = NULL;
    }
    assert_int_equal(rkvc_job_push_eos(enc_job), RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_wait(enc_job), RKVC_STATUS_OK);

    fake_rknn_qrow_bitmap(&lo, &hi);
    assert_int_equal(hi, 0);
    assert_int_equal(lo, expect); /* 喂入的 q 集合 == 记录头 q 集合 */
    /* host 路径每个编码帧喂 3 张行表 */
    assert_int_equal(fake_rknn_qrow_feed_count(), (uint32_t)encoded * 3);

    unsetenv("RKVC_MLVC_ENCODER_ZERO_COPY");
    rkvc_job_destroy(enc_job);
    rkvc_diag_release(enc_diag);
    rkvc_context_destroy(context);
    free(nv12);
}

/* 解码端逐帧 q 切换：篡改记录头 q_index，解码器必须按记录喂行 */
static void test_mlvc_qp_dynamic_decode_tracks_record_q(void **state)
{
    enum { N = 3 };
    /* 帧 0/1/2 的记录 q 分别改为 21/30/5 */
    static const int patch_q[N] = {21, 30, 5};
    rkvc_context *context;
    rkvc_request enc_req, dec_req;
    rkvc_job *enc_job = NULL, *dec_job = NULL;
    rkvc_diag *enc_diag = NULL, *dec_diag = NULL;
    rkvc_frame *input = NULL, *record = NULL, *frame = NULL;
    rkvc_frame_desc desc;
    uint8_t *nv12;
    uint64_t lo = 0, hi = 0;
    int i;
    (void)state;

    write_models_dyn();
    context = make_context();

    rkvc_request_init(&enc_req, sizeof(enc_req));
    enc_req.operation = RKVC_OPERATION_ENCODE;
    enc_req.codec = RKVC_CODEC_MLVC;
    enc_req.input.kind = RKVC_ENDPOINT_FRAME_SINK;
    enc_req.input.fmt = RKVC_FRAME_FMT_NV12;
    enc_req.output.kind = RKVC_ENDPOINT_FRAME_SINK;
    enc_req.output.fmt = RKVC_FRAME_FMT_BITSTREAM;
    enc_req.width = IMG_W;
    enc_req.height = IMG_H;
    enc_req.quality.qp = 21;
    enc_req.model_id = "mlvc-enc-test";
    assert_int_equal(rkvc_job_create(context, &enc_req, &enc_diag, &enc_job),
                     RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_start(enc_job, &enc_diag), RKVC_STATUS_OK);

    rkvc_request_init(&dec_req, sizeof(dec_req));
    dec_req.operation = RKVC_OPERATION_DECODE;
    dec_req.codec = RKVC_CODEC_MLVC;
    dec_req.input.kind = RKVC_ENDPOINT_FRAME_SINK;
    dec_req.input.fmt = RKVC_FRAME_FMT_BITSTREAM;
    dec_req.output.kind = RKVC_ENDPOINT_FRAME_SINK;
    dec_req.output.fmt = RKVC_FRAME_FMT_NV12;
    dec_req.model_id = "mlvc-dec-test";
    assert_int_equal(rkvc_job_create(context, &dec_req, &dec_diag, &dec_job),
                     RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_start(dec_job, &dec_diag), RKVC_STATUS_OK);

    nv12 = malloc((size_t)IMG_W * IMG_H * 3 / 2);
    assert_non_null(nv12);
    memset(nv12, 70, (size_t)IMG_W * IMG_H);
    memset(nv12 + (size_t)IMG_W * IMG_H, 128, (size_t)IMG_W * IMG_H / 2);

    for (i = 0; i < N; i++) {
        int32_t q32 = patch_q[i];
        input = wrap_nv12_frame(nv12);
        assert_int_equal(rkvc_job_push(enc_job, input), RKVC_STATUS_OK);
        input = NULL;
        assert_int_equal(rkvc_job_pull(enc_job, &record), RKVC_STATUS_OK);
        assert_int_equal(rkvc_frame_get_desc(record, &desc), RKVC_STATUS_OK);
        /* 篡改记录头 q_index（首帧跳过 64B 容器头） */
        memcpy((uint8_t *)desc.data + (i == 0 ? MLVC_HDR_SIZE : 0) + 4,
               &q32, 4);
        assert_int_equal(rkvc_job_push(dec_job, record), RKVC_STATUS_OK);
        record = NULL;
    }
    assert_int_equal(rkvc_job_push_eos(enc_job), RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_push_eos(dec_job), RKVC_STATUS_OK);

    for (i = 0; i < N; i++) {
        assert_int_equal(rkvc_job_pull(dec_job, &frame), RKVC_STATUS_OK);
        rkvc_frame_release(frame);
    }
    assert_int_equal(rkvc_job_pull(dec_job, &frame), RKVC_STATUS_EOF);
    assert_int_equal(rkvc_job_wait(enc_job), RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_wait(dec_job), RKVC_STATUS_OK);

    fake_rknn_qrow_bitmap(&lo, &hi);
    assert_int_equal(hi, 0);
    assert_int_equal(lo, (1ull << 21) | (1ull << 30) | (1ull << 5));

    rkvc_job_destroy(enc_job);
    rkvc_job_destroy(dec_job);
    rkvc_diag_release(enc_diag);
    rkvc_diag_release(dec_diag);
    rkvc_context_destroy(context);
    free(nv12);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_mlvc_scale_profiles),
        cmocka_unit_test(test_mlvc_encode_writes_container_records),
        cmocka_unit_test(test_mlvc_encode_decode_roundtrip),
        cmocka_unit_test(test_mlvc_decode_rejects_trailing_rans_data),
        cmocka_unit_test(test_mlvc_idr_ltr_flags),
        cmocka_unit_test(test_mlvc_drop_roundtrip),
        cmocka_unit_test(test_mlvc_header_strict_format),
        cmocka_unit_test(test_mlvc_qp_dynamic_roundtrip),
        cmocka_unit_test(test_mlvc_qp_dynamic_rc_matches_records),
        cmocka_unit_test(test_mlvc_qp_dynamic_decode_tracks_record_q),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
