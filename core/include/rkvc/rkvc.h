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
#define RKVC_NOEXCEPT noexcept
#else
#define RKVC_NOEXCEPT
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define RKVC_ABI_VERSION_MAJOR 0
#define RKVC_ABI_VERSION_MINOR 6
#define RKVC_ABI_VERSION_PATCH 0
#define RKVC_ABI_VERSION \
    ((RKVC_ABI_VERSION_MAJOR << 16) | (RKVC_ABI_VERSION_MINOR << 8) | \
     (RKVC_ABI_VERSION_PATCH))

/** 操作状态码。负值为错误；`RKVC_OK` 表示成功，`RKVC_AGAIN`/`RKVC_EOF` 是会话流控的正常终态；语义与 C++ 侧 `rkvc::Status` 一一对应。 */
typedef enum rkvc_status {
    RKVC_OK = 0,            /**< 成功 */
    RKVC_NOMEM = -1,        /**< 内存不足 */
    RKVC_INVALID = -2,      /**< 参数非法 */
    RKVC_NOT_FOUND = -3,    /**< 资源不存在 */
    RKVC_IO = -4,           /**< I/O 错误 */
    RKVC_HW = -5,           /**< 硬件错误 */
    RKVC_EOF = -6,          /**< 已到达流末尾（正常终态） */
    RKVC_AGAIN = -7,        /**< 数据暂不可用/背压（非错误） */
    RKVC_FORMAT = -8,       /**< 格式不支持或数据损坏 */
    RKVC_NEGOTIATE = -9,    /**< 端点协商失败 */
    RKVC_PERMISSION = -10,  /**< 权限不足 */
    RKVC_CANCELED = -11,    /**< 操作被取消 */
    RKVC_UNSUPPORTED = -12, /**< 能力不支持 */
    RKVC_INTERNAL = -13,    /**< 内部错误 */
    RKVC_MODEL = -14,       /**< 模型错误 */
    RKVC_LICENSE = -15,     /**< 许可证错误 */
    RKVC_INTEGRITY = -16    /**< 数据完整性校验失败 */
} rkvc_status;

/** 状态码转可读字符串（静态存储，无需释放）。 */
const char *rkvc_status_str(rkvc_status status);

/** 会话操作类型。 */
typedef enum rkvc_operation {
    RKVC_OP_TRANSCODE = 0, /**< 转码 */
    RKVC_OP_DECODE = 1,    /**< 解码 */
    RKVC_OP_ENCODE = 2,    /**< 编码 */
    RKVC_OP_UPSCALE = 3    /**< 超分 */
} rkvc_operation;

/** 编解码器。 */
typedef enum rkvc_codec {
    RKVC_CODEC_AUTO = 0, /**< 自动选择 */
    RKVC_CODEC_H264 = 1, /**< H.264 */
    RKVC_CODEC_HEVC = 2, /**< H.265/HEVC */
    RKVC_CODEC_AV1 = 3,  /**< AV1 */
    RKVC_CODEC_MLVC = 4  /**< MLVC（NPU） */
} rkvc_codec;

/** 执行策略（时延/质量权衡）。 */
typedef enum rkvc_policy {
    RKVC_POLICY_REALTIME = 0, /**< 实时优先 */
    RKVC_POLICY_BALANCED = 1, /**< 平衡 */
    RKVC_POLICY_QUALITY = 2,  /**< 质量优先 */
    RKVC_POLICY_OFFLINE = 3   /**< 离线批量 */
} rkvc_policy;

/** 帧像素格式。 */
typedef enum rkvc_frame_fmt {
    RKVC_FRAME_FMT_UNKNOWN = 0,  /**< 未知 */
    RKVC_FRAME_FMT_NV12 = 1,     /**< NV12 */
    RKVC_FRAME_FMT_NV21 = 2,     /**< NV21 */
    RKVC_FRAME_FMT_YUV420P = 3,  /**< YUV420P */
    RKVC_FRAME_FMT_NV16 = 4,     /**< NV16 */
    RKVC_FRAME_FMT_P010 = 5,     /**< P010（10-bit） */
    RKVC_FRAME_FMT_RGB24 = 6,    /**< RGB24 */
    RKVC_FRAME_FMT_BITSTREAM = 7 /**< 压缩比特流 */
} rkvc_frame_fmt;

/** 帧内存域。 */
typedef enum rkvc_mem_domain {
    RKVC_MEM_DOMAIN_HOST = 0,  /**< 普通主机内存 */
    RKVC_MEM_DOMAIN_DMABUF = 1 /**< DMA-BUF 显存 */
} rkvc_mem_domain;

/** 端点类型。 */
typedef enum rkvc_endpoint_kind {
    RKVC_ENDPOINT_FILE = 0,       /**< 文件端点 */
    RKVC_ENDPOINT_FRAME_SINK = 1, /**< 帧回调端点 */
    RKVC_ENDPOINT_STREAM = 2      /**< 流式端点 */
} rkvc_endpoint_kind;

#define RKVC_FRAME_TS_UNKNOWN INT64_MIN /**< 未知时间戳标记 */

/** 帧标志位。 */
enum {
    RKVC_FRAME_FLAG_KEYFRAME = 1u << 0,      /**< 关键帧 */
    RKVC_FRAME_FLAG_DISCONTINUITY = 1u << 1, /**< 流不连续 */
    RKVC_FRAME_FLAG_CORRUPT = 1u << 2        /**< 数据损坏 */
};

/** 上下文创建选项。结构体自带 `struct_size`/`version` 用于前后向兼容演进。 */
typedef struct rkvc_context_options {
    size_t struct_size;              /**< `sizeof` 校验 */
    uint32_t version;                /**< 结构版本 */
    const char *const *backend_dirs; /**< 插件搜索目录数组 */
    size_t backend_dir_count;        /**< 目录数量 */
} rkvc_context_options;

/** 流端点描述（会话输入或输出）。 */
typedef struct rkvc_endpoint {
    rkvc_endpoint_kind kind; /**< 端点类型 */
    const char *uri;         /**< 文件路径或流地址 */
    rkvc_frame_fmt fmt;      /**< 期望格式 */
    uint32_t width;          /**< 宽度（0 = 自动） */
    uint32_t height;         /**< 高度（0 = 自动） */
} rkvc_endpoint;

/** 编码质量参数。 */
typedef struct rkvc_quality {
    int32_t bitrate_bps; /**< 目标码率 bps；0 = 未指定 */
    int32_t qp;          /**< 固定 QP；0 = 未指定 */
    uint32_t gop_size;   /**< GOP 大小 */
    uint32_t fps;        /**< 目标帧率 */
} rkvc_quality;

/** 会话创建请求。 */
typedef struct rkvc_session_request {
    size_t struct_size;       /**< `sizeof` 校验 */
    uint32_t version;         /**< 结构版本 */
    rkvc_operation operation; /**< 操作类型 */
    rkvc_codec codec;         /**< 编解码器 */
    rkvc_policy policy;       /**< 执行策略 */
    rkvc_quality quality;     /**< 质量参数 */
    rkvc_endpoint input;      /**< 输入端点 */
    rkvc_endpoint output;     /**< 输出端点 */
    const char *model_id;     /**< 模型 ID（MLVC/SR 用） */
    uint32_t queue_capacity;  /**< 队列容量 */
} rkvc_session_request;

/** 帧规格（宽/高/stride 为 0 表示通配或待探测）。 */
typedef struct rkvc_frame_spec {
    uint32_t width;         /**< 宽度 */
    uint32_t height;        /**< 高度 */
    rkvc_frame_fmt fmt;     /**< 像素格式 */
    rkvc_mem_domain domain; /**< 内存域 */
    uint32_t stride;        /**< 行距 */
    uint32_t ver_stride;    /**< 垂直行距（第二平面） */
    uint64_t modifier;      /**< 内存修饰符 */
} rkvc_frame_spec;

/** 帧描述（包装与查询用）。 */
typedef struct rkvc_frame_desc {
    size_t struct_size;   /**< `sizeof` 校验 */
    uint32_t version;     /**< 结构版本 */
    rkvc_frame_spec spec; /**< 帧规格 */
    void *data;           /**< 数据指针（Host 域） */
    size_t size;          /**< 数据大小 */
    int fd;               /**< DMA-BUF fd（Dmabuf 域，可为 -1） */
    int64_t pts;          /**< 显示时间戳 */
    int64_t dts;          /**< 解码时间戳 */
    uint32_t flags;       /**< RKVC_FRAME_FLAG_* 位标志 */
} rkvc_frame_desc;

/** 设备能力探测结果。 */
typedef struct rkvc_caps {
    size_t struct_size;  /**< `sizeof` 校验 */
    uint32_t version;    /**< 结构版本 */
    char soc[64];        /**< SoC 型号字符串 */
    int has_mpp_encoder; /**< 是否支持硬件编码 */
    int has_mpp_decoder; /**< 是否支持硬件解码 */
    int has_rknn;        /**< 是否支持 RKNN NPU */
    uint32_t npu_cores;  /**< NPU 核心数 */
} rkvc_caps;

typedef struct rkvc_context rkvc_context;
typedef struct rkvc_session rkvc_session;
typedef struct rkvc_frame rkvc_frame;
typedef struct rkvc_diagnostic rkvc_diagnostic;

/** 初始化上下文选项（填写 `struct_size`/`version`）。
 * @param opts 选项结构。
 * @param size 传入的缓冲区大小，用于向后兼容校验。 */
void rkvc_context_options_init(rkvc_context_options *opts, size_t size);

/** 创建运行上下文。
 * @param opts 选项；可传 NULL 使用默认值。
 * @param out 接收上下文句柄。
 * @return 成功返回 `RKVC_OK`。 */
rkvc_status rkvc_context_create(const rkvc_context_options *opts,
                                rkvc_context **out);

/** 销毁上下文，并释放其持有的一切资源。 */
void rkvc_context_destroy(rkvc_context *ctx);

/** 探测设备能力。
 * @param ctx 上下文句柄。
 * @param caps 输出的能力结构。
 * @return 成功返回 `RKVC_OK`。 */
rkvc_status rkvc_probe_device(rkvc_context *ctx, rkvc_caps *caps);

/** 按导出器约定扫描模型目录并注册整组原生文件（.rknn/.bin/.qppatch，
 * id 去重；失败时可选填 diag）。一个 bundle 目录可产出 encoder/decoder
 * 等多个模型，全部注册。 */
rkvc_status rkvc_context_add_model_dir(rkvc_context *ctx, const char *dir,
                                       rkvc_diagnostic **diag);

/** 初始化会话请求（填写 `struct_size`/`version` 与默认值）。
 * @param req 请求结构。
 * @param size 传入的缓冲区大小。 */
void rkvc_session_request_init(rkvc_session_request *req, size_t size);

/** 创建会话。
 * @param req 会话请求。
 * @param out 接收会话句柄。
 * @param diag 可选诊断输出。
 * @return 成功返回 `RKVC_OK`。 */
rkvc_status rkvc_session_create(rkvc_context *ctx,
                                const rkvc_session_request *req,
                                rkvc_session **out, rkvc_diagnostic **diag);

/** 启动会话。
 * @param diag 可选诊断输出。
 * @return 成功返回 `RKVC_OK`。 */
rkvc_status rkvc_session_start(rkvc_session *s, rkvc_diagnostic **diag);

/** 非阻塞入队一帧。 */
rkvc_status rkvc_session_push(rkvc_session *s, rkvc_frame *f);

/** 非阻塞拉取一帧；无可用数据时返回 `RKVC_AGAIN`。 */
rkvc_status rkvc_session_try_pull(rkvc_session *s, rkvc_frame **out);

/** 阻塞拉取一帧。 */
rkvc_status rkvc_session_pull(rkvc_session *s, rkvc_frame **out);

/** 推送流结束标记。 */
rkvc_status rkvc_session_push_eos(rkvc_session *s);

/** 等待管道排空，返回首个错误或 `RKVC_OK`。 */
rkvc_status rkvc_session_wait(rkvc_session *s);

/** 销毁会话。 */
void rkvc_session_destroy(rkvc_session *s);

/** 取终端错误的阶段明细（wait 已排空后调用，立即返回；Ok 时 buf 置空）。
 * @param buf 输出缓冲区。
 * @param size 缓冲区大小。 */
rkvc_status rkvc_session_error_text(rkvc_session *s, char *buf, size_t size);

/** 初始化帧描述（填写 `struct_size`/`version`）。 */
void rkvc_frame_desc_init(rkvc_frame_desc *desc, size_t size);

/** 按描述包装一帧。
 * @param desc 帧描述。
 * @param out 接收帧句柄。
 * @return 成功返回 `RKVC_OK`。 */
rkvc_status rkvc_frame_wrap(const rkvc_frame_desc *desc, rkvc_frame **out);

/** 包装一帧并持有载荷所有权：最后一个帧引用释放时回调
 * `release(release_ctx)`（典型用途：释放宿主的深拷贝副本）。
 * wrap 失败时所有权不转移，调用方自释放。域语义与 `rkvc_frame_wrap`
 * 相同（Host 用 `data`，Dmabuf 用 `fd`）。
 * @param desc 帧描述。
 * @param release 载荷释放回调（不可为 NULL）。
 * @param release_ctx 传给回调的上下文（通常即载荷指针）。
 * @param out 接收帧句柄。
 * @return 成功返回 `RKVC_OK`。 */
rkvc_status rkvc_frame_wrap_owned(const rkvc_frame_desc *desc,
                                  void (*release)(void *release_ctx) noexcept,
                                  void *release_ctx, rkvc_frame **out);

/** 查询帧的当前描述。 */
rkvc_status rkvc_frame_get_desc(const rkvc_frame *f, rkvc_frame_desc *desc);

/** 释放帧句柄。 */
void rkvc_frame_release(rkvc_frame *f);

/** 将诊断信息格式化为文本。
 * @param buf 输出缓冲区。
 * @param size 缓冲区大小。 */
void rkvc_diag_fmt_text(const rkvc_diagnostic *diag, char *buf, size_t size);

/** 释放诊断对象。 */
void rkvc_diag_release(rkvc_diagnostic *diag);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_RKVC_H */
