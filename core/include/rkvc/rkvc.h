/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef RKVC_RKVC_H
#define RKVC_RKVC_H

/* rkvc C ABI (first stable surface, cpp-rewrite). Handle-style minimal
 * interface: context / session / frame / enums / status strings / probe.
 * Structures carry size/version first for evolution. The implementation is
 * exception-free; OOM surfaces as RKVC_NOMEM. */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RKVC_ABI_VERSION_MAJOR 0
#define RKVC_ABI_VERSION_MINOR 5
#define RKVC_ABI_VERSION_PATCH 0
#define RKVC_ABI_VERSION \
    ((RKVC_ABI_VERSION_MAJOR << 16) | (RKVC_ABI_VERSION_MINOR << 8) | \
     (RKVC_ABI_VERSION_PATCH))

typedef enum rkvc_status {
    RKVC_OK = 0,
    RKVC_NOMEM = -1,
    RKVC_INVALID = -2,
    RKVC_NOT_FOUND = -3,
    RKVC_IO = -4,
    RKVC_HW = -5,
    RKVC_EOF = -6,
    RKVC_AGAIN = -7,
    RKVC_FORMAT = -8,
    RKVC_NEGOTIATE = -9,
    RKVC_PERMISSION = -10,
    RKVC_CANCELED = -11,
    RKVC_UNSUPPORTED = -12,
    RKVC_INTERNAL = -13,
    RKVC_MODEL = -14,
    RKVC_LICENSE = -15,
    RKVC_INTEGRITY = -16
} rkvc_status;

const char *rkvc_status_str(rkvc_status status);

typedef enum rkvc_operation {
    RKVC_OP_TRANSCODE = 0,
    RKVC_OP_DECODE = 1,
    RKVC_OP_ENCODE = 2,
    RKVC_OP_UPSCALE = 3
} rkvc_operation;

typedef enum rkvc_codec {
    RKVC_CODEC_AUTO = 0,
    RKVC_CODEC_H264 = 1,
    RKVC_CODEC_HEVC = 2,
    RKVC_CODEC_AV1 = 3,
    RKVC_CODEC_MLVC = 4
} rkvc_codec;

typedef enum rkvc_policy {
    RKVC_POLICY_REALTIME = 0,
    RKVC_POLICY_BALANCED = 1,
    RKVC_POLICY_QUALITY = 2,
    RKVC_POLICY_OFFLINE = 3
} rkvc_policy;

typedef enum rkvc_frame_fmt {
    RKVC_FRAME_FMT_UNKNOWN = 0,
    RKVC_FRAME_FMT_NV12 = 1,
    RKVC_FRAME_FMT_NV21 = 2,
    RKVC_FRAME_FMT_YUV420P = 3,
    RKVC_FRAME_FMT_NV16 = 4,
    RKVC_FRAME_FMT_P010 = 5,
    RKVC_FRAME_FMT_RGB24 = 6,
    RKVC_FRAME_FMT_BITSTREAM = 7
} rkvc_frame_fmt;

typedef enum rkvc_mem_domain {
    RKVC_MEM_DOMAIN_HOST = 0,
    RKVC_MEM_DOMAIN_DMABUF = 1
} rkvc_mem_domain;

typedef enum rkvc_endpoint_kind {
    RKVC_ENDPOINT_FILE = 0,
    RKVC_ENDPOINT_FRAME_SINK = 1,
    RKVC_ENDPOINT_STREAM = 2
} rkvc_endpoint_kind;

#define RKVC_FRAME_TS_UNKNOWN INT64_MIN

enum {
    RKVC_FRAME_FLAG_KEYFRAME = 1u << 0,
    RKVC_FRAME_FLAG_DISCONTINUITY = 1u << 1,
    RKVC_FRAME_FLAG_CORRUPT = 1u << 2
};

typedef struct rkvc_context_options {
    size_t struct_size;
    uint32_t version;
    const char *const *backend_dirs;
    size_t backend_dir_count;
} rkvc_context_options;

typedef struct rkvc_endpoint {
    rkvc_endpoint_kind kind;
    const char *uri;
    rkvc_frame_fmt fmt;
    uint32_t width;
    uint32_t height;
} rkvc_endpoint;

typedef struct rkvc_quality {
    int32_t bitrate_bps;
    int32_t qp;
    uint32_t gop_size;
} rkvc_quality;

typedef struct rkvc_session_request {
    size_t struct_size;
    uint32_t version;
    rkvc_operation operation;
    rkvc_codec codec;
    rkvc_policy policy;
    rkvc_quality quality;
    rkvc_endpoint input;
    rkvc_endpoint output;
    const char *model_id;
    uint32_t queue_capacity;
} rkvc_session_request;

typedef struct rkvc_frame_spec {
    uint32_t width;
    uint32_t height;
    rkvc_frame_fmt fmt;
    rkvc_mem_domain domain;
    uint32_t stride;
    uint32_t ver_stride;
    uint64_t modifier;
} rkvc_frame_spec;

typedef struct rkvc_frame_desc {
    size_t struct_size;
    uint32_t version;
    rkvc_frame_spec spec;
    void *data;
    size_t size;
    int fd;
    int64_t pts;
    int64_t dts;
    uint32_t flags;
} rkvc_frame_desc;

typedef struct rkvc_caps {
    size_t struct_size;
    uint32_t version;
    char soc[64];
    int has_mpp_encoder;
    int has_mpp_decoder;
    int has_rknn;
    uint32_t npu_cores;
} rkvc_caps;

typedef struct rkvc_context rkvc_context;
typedef struct rkvc_session rkvc_session;
typedef struct rkvc_frame rkvc_frame;
typedef struct rkvc_diagnostic rkvc_diagnostic;

void rkvc_context_options_init(rkvc_context_options *opts, size_t size);
rkvc_status rkvc_context_create(const rkvc_context_options *opts,
                                rkvc_context **out);
void rkvc_context_destroy(rkvc_context *ctx);
rkvc_status rkvc_probe_device(rkvc_context *ctx, rkvc_caps *caps);

void rkvc_session_request_init(rkvc_session_request *req, size_t size);
rkvc_status rkvc_session_create(rkvc_context *ctx,
                                const rkvc_session_request *req,
                                rkvc_session **out, rkvc_diagnostic **diag);
rkvc_status rkvc_session_start(rkvc_session *s, rkvc_diagnostic **diag);
rkvc_status rkvc_session_push(rkvc_session *s, rkvc_frame *f);
rkvc_status rkvc_session_try_pull(rkvc_session *s, rkvc_frame **out);
rkvc_status rkvc_session_pull(rkvc_session *s, rkvc_frame **out);
rkvc_status rkvc_session_push_eos(rkvc_session *s);
void rkvc_session_destroy(rkvc_session *s);

void rkvc_frame_desc_init(rkvc_frame_desc *desc, size_t size);
rkvc_status rkvc_frame_wrap(const rkvc_frame_desc *desc, rkvc_frame **out);
rkvc_status rkvc_frame_get_desc(const rkvc_frame *f, rkvc_frame_desc *desc);
void rkvc_frame_release(rkvc_frame *f);

void rkvc_diag_fmt_text(const rkvc_diagnostic *diag, char *buf, size_t size);
void rkvc_diag_release(rkvc_diagnostic *diag);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_RKVC_H */
