/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file backend_ffmpeg.c
 * @brief 面向版本化 rkvc ABI 的 FFmpeg 容器后端：demux / mux 节点。
 *
 * - ffmpeg.demux（SOURCE）：容器输入（.mp4/.mov/.mkv/.ts 等）→ 逐帧
 *   BITSTREAM 帧。H.264/HEVC 经 h264_mp4toannexb / hevc_mp4toannexb
 *   转为 Annex-B（含首帧 SPS/PPS）；AV1 直通 OBU。仅对“容器后缀”
 *   生效：原始裸流（.h264 等）返回 NULL，规划回退到 file.source。
 * - ffmpeg.mux（SINK）：BITSTREAM 帧 → 容器输出。首帧经 av_parser
 *   求尺寸、extract_extradata 提取 SPS/PPS 或 AV1 OBU，经
 *   avformat_write_header 由 muxer 生成 avcC/hvcC/av1C；仅容器后缀
 *   输出生效，否则回退 file.sink。
 *
 * 依赖 libavformat/libavcodec/libavutil；后端 DSO 独立装载失败不影响
 * 其他路径。
 */

#include "rkvc/backend.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>

/** 90 kHz 时间戳基准（MP4/MKV 常用，便于编排）。 */
#define FF_RKVC_TIMEBASE 90000

/* ── 工具 ────────────────────────────────────────────────────────── */

/** 帧释放回调：本后端分配的 HOST 缓冲直接归还。 */
static void ff_frame_release(void *ptr) {
    free(ptr);
}

/** 简单容器后缀白名单；裸码流（.h264/.hevc/.av1）不在此列。 */
static int is_container_ext(const char *uri) {
    static const char *exts[] = {
        ".mp4", ".m4v", ".mov", ".mkv", ".webm", ".ts", ".m2ts",
        ".mts", ".avi", ".flv", ".wmv", ".mpg", ".mpeg", ".ivf",
    };
    const char *dot = uri ? strrchr(uri, '.') : NULL;
    size_t i;

    if (!dot)
        return 0;
    for (i = 0; i < sizeof(exts) / sizeof(exts[0]); ++i) {
        if (strcasecmp(dot, exts[i]) == 0)
            return 1;
    }
    return 0;
}

/** 把 rkvc 编码映射为 FFmpeg codec id；不支持的返回 AV_CODEC_ID_NONE。 */
static enum AVCodecID rkvc_to_avcodec(rkvc_codec codec) {
    switch (codec) {
    case RKVC_CODEC_H264: return AV_CODEC_ID_H264;
    case RKVC_CODEC_HEVC: return AV_CODEC_ID_HEVC;
    case RKVC_CODEC_AV1:  return AV_CODEC_ID_AV1;
    default:              return AV_CODEC_ID_NONE;
    }
}

/** 把 FFmpeg 包时间戳换算到 90k；AV_NOPTS_VALUE 映射为未知。 */
static int64_t rescale_ts(int64_t ts, AVRational tb) {
    if (ts == AV_NOPTS_VALUE)
        return RKVC_FRAME_TS_UNKNOWN;
    return av_rescale_q(ts, tb, (AVRational){1, FF_RKVC_TIMEBASE});
}

/* ── ffmpeg.demux ────────────────────────────────────────────────── */

/** demux 节点私有状态。 */
struct ffmpeg_demuxer {
    rkvc_request  request;   /**< 请求副本（读取 uri） */
    AVFormatContext *fmt;    /**< 打开的输入上下文 */
    int           stream;    /**< 视频流索引 */
    enum AVCodecID codec_id; /**< 视频流编码 */
    AVRational    time_base; /**< 流时间基 */
    AVBSFContext *bsf;       /**< mp4toannexb（H.264/HEVC；AV1 为 NULL） */
    rkvc_frame_spec out;     /**< 输出格式（BITSTREAM/HOST + 流几何） */
};

/** 声明输出：BITSTREAM/HOST（几何在 open 后填充，协商期用通配）。 */
static int demux_configure(rkvc_node *node, rkvc_diag **diag) {
    struct ffmpeg_demuxer *dm = node->priv;
    rkvc_frame_spec out = {0};
    (void)diag;

    out.fmt = RKVC_FRAME_FMT_BITSTREAM;
    out.domain = RKVC_MEM_DOMAIN_HOST;
    dm->out = out;
    rkvc_port_set_desired(&node->out_ports[0], &out);
    return 0;
}

/** 打开容器、定位视频流并准备 mp4toannexb（容器内参数集以
 * avcC/hvcC 存储，必须转换后 MPP 才能解析）。 */
static int demux_open(rkvc_node *node, rkvc_diag **diag) {
    struct ffmpeg_demuxer *dm = node->priv;
    AVStream *st;
    const char *bsf_name = NULL;
    int ret;

    ret = avformat_open_input(&dm->fmt, dm->request.input.uri, NULL, NULL);
    if (ret < 0) {
        if (diag)
            rkvc_diag_push(diag, RKVC_STATUS_IO, 3, node->ops->id,
                           "avformat_open_input failed");
        return (int)RKVC_STATUS_IO;
    }
    ret = avformat_find_stream_info(dm->fmt, NULL);
    if (ret < 0) {
        if (diag)
            rkvc_diag_push(diag, RKVC_STATUS_FORMAT, 3, node->ops->id,
                           "avformat_find_stream_info failed");
        return (int)RKVC_STATUS_FORMAT;
    }
    dm->stream = av_find_best_stream(dm->fmt, AVMEDIA_TYPE_VIDEO, -1, -1,
                                     NULL, 0);
    if (dm->stream < 0) {
        if (diag)
            rkvc_diag_push(diag, RKVC_STATUS_NOT_FOUND, 3, node->ops->id,
                           "no video stream");
        return (int)RKVC_STATUS_NOT_FOUND;
    }
    st = dm->fmt->streams[dm->stream];
    dm->codec_id = st->codecpar->codec_id;
    dm->time_base = st->time_base;
    dm->out.width = st->codecpar->width;
    dm->out.height = st->codecpar->height;

    switch (dm->codec_id) {
    case AV_CODEC_ID_H264: bsf_name = "h264_mp4toannexb"; break;
    case AV_CODEC_ID_HEVC: bsf_name = "hevc_mp4toannexb"; break;
    case AV_CODEC_ID_AV1:  bsf_name = NULL; break;
    default:
        if (diag)
            rkvc_diag_push(diag, RKVC_STATUS_UNSUPPORTED, 3, node->ops->id,
                           "unsupported container codec");
        return (int)RKVC_STATUS_UNSUPPORTED;
    }
    if (bsf_name) {
        const AVBitStreamFilter *filter = av_bsf_get_by_name(bsf_name);
        if (!filter ||
            av_bsf_alloc(filter, &dm->bsf) < 0 ||
            avcodec_parameters_copy(dm->bsf->par_in, st->codecpar) < 0 ||
            av_bsf_init(dm->bsf) < 0) {
            if (diag)
                rkvc_diag_push(diag, RKVC_STATUS_UNSUPPORTED, 3,
                               node->ops->id, "bsf init failed");
            return (int)RKVC_STATUS_UNSUPPORTED;
        }
    }
    return 0;
}

/** 把一个 AVPacket 复制为 BITSTREAM 帧并发出。 */
static int demux_emit(rkvc_node *node, AVPacket *pkt) {
    struct ffmpeg_demuxer *dm = node->priv;
    rkvc_frame_desc desc;
    rkvc_frame *frame = NULL;
    rkvc_status st;
    void *data;
    int rc;

    if (!pkt->size)
        return 0;
    data = malloc(pkt->size);
    if (!data)
        return (int)RKVC_STATUS_NOMEM;
    memcpy(data, pkt->data, pkt->size);

    rkvc_frame_desc_init(&desc, sizeof(desc));
    desc.spec = dm->out;
    desc.data = data;
    desc.size = pkt->size;
    desc.pts = rescale_ts(pkt->pts, dm->time_base);
    desc.dts = rescale_ts(pkt->dts, dm->time_base);
    if (pkt->flags & AV_PKT_FLAG_KEY)
        desc.flags |= RKVC_FRAME_FLAG_KEYFRAME;

    st = rkvc_backend_frame_create(&desc, ff_frame_release, data, &frame);
    if (st != RKVC_STATUS_OK) {
        free(data);
        return (int)st;
    }
    rc = rkvc_node_emit(node, 0, frame);
    if (rc != 0)
        rkvc_frame_release(frame);
    return rc;
}

/** 源节点在 flush 阶段（输入 EOS 触发）读取整个容器。 */
static int demux_flush(rkvc_node *node, rkvc_diag **diag) {
    struct ffmpeg_demuxer *dm = node->priv;
    AVPacket *pkt = NULL;
    int rc = 0;

    if (!dm->fmt)
        return (int)RKVC_STATUS_INVALID;
    pkt = av_packet_alloc();
    if (!pkt)
        return (int)RKVC_STATUS_NOMEM;

    for (;;) {
        int ret = av_read_frame(dm->fmt, pkt);
        if (ret < 0)
            break; /* EOF 或读取错误：结束 */
        if (pkt->stream_index != dm->stream) {
            av_packet_unref(pkt);
            continue;
        }
        if (!dm->bsf) {
            rc = demux_emit(node, pkt);
            av_packet_unref(pkt);
            if (rc != 0)
                break;
            continue;
        }
        ret = av_bsf_send_packet(dm->bsf, pkt); /* 接管 pkt */
        if (ret < 0) {
            av_packet_unref(pkt);
            rc = (int)RKVC_STATUS_FORMAT;
            break;
        }
        for (;;) {
            AVPacket *out = av_packet_alloc();
            int r2;
            if (!out) {
                rc = (int)RKVC_STATUS_NOMEM;
                break;
            }
            r2 = av_bsf_receive_packet(dm->bsf, out);
            if (r2 == AVERROR(EAGAIN) || r2 == AVERROR_EOF) {
                av_packet_free(&out);
                break;
            }
            if (r2 < 0) {
                av_packet_free(&out);
                rc = (int)RKVC_STATUS_FORMAT;
                break;
            }
            rc = demux_emit(node, out);
            av_packet_free(&out);
            if (rc != 0)
                break;
        }
        if (rc != 0)
            break;
    }

    /* 冲刷 bsf 剩余输出（如末帧）。 */
    if (!rc && dm->bsf) {
        (void)av_bsf_send_packet(dm->bsf, NULL);
        for (;;) {
            AVPacket *out = av_packet_alloc();
            int r2;
            if (!out) {
                rc = (int)RKVC_STATUS_NOMEM;
                break;
            }
            r2 = av_bsf_receive_packet(dm->bsf, out);
            if (r2 == AVERROR(EAGAIN) || r2 == AVERROR_EOF) {
                av_packet_free(&out);
                break;
            }
            if (r2 < 0) {
                av_packet_free(&out);
                rc = (int)RKVC_STATUS_FORMAT;
                break;
            }
            rc = demux_emit(node, out);
            av_packet_free(&out);
            if (rc != 0)
                break;
        }
    }
    av_packet_free(&pkt);
    return rc;
}

/** 释放 demux 资源（幂等）。 */
static void demux_close(rkvc_node *node) {
    struct ffmpeg_demuxer *dm = node->priv;
    if (!dm)
        return;
    if (dm->bsf) {
        av_bsf_free(&dm->bsf);
        dm->bsf = NULL;
    }
    if (dm->fmt) {
        avformat_close_input(&dm->fmt);
        dm->fmt = NULL;
    }
}

/** 通用节点析构（demux/mux 共用）。 */
static void ff_destroy_node(rkvc_node *node) {
    if (!node)
        return;
    if (node->ops->close)
        node->ops->close(node);
    free(node->priv);
    free(node->in_ports);
    free(node->out_ports);
    free(node);
}

static const rkvc_node_ops demux_ops = {
    "ffmpeg.demux", demux_configure, demux_open, NULL, demux_flush,
    demux_close, ff_destroy_node,
};

/** demux 门控：DECODE/TRANSCODE 且输入非 MLVC（MLVC 有独立容器）。 */
static int demux_matches(rkvc_operation op, rkvc_codec codec,
                         const rkvc_device_caps *caps) {
    (void)caps;
    return (op == RKVC_OPERATION_DECODE || op == RKVC_OPERATION_TRANSCODE) &&
           codec != RKVC_CODEC_MLVC;
}

/** create：仅对容器后缀输入返回节点；裸流返回 NULL 触发计划回退。 */
static rkvc_node *demux_create(const rkvc_node_factory *factory,
                               const rkvc_request *request,
                               void *create_ctx) {
    rkvc_node *node;
    struct ffmpeg_demuxer *dm;
    (void)factory;
    (void)create_ctx;

    if (!request->input.uri ||
        request->input.kind != RKVC_ENDPOINT_FILE ||
        !is_container_ext(request->input.uri))
        return NULL;
    node = calloc(1, sizeof(*node));
    dm = calloc(1, sizeof(*dm));
    if (!node || !dm) {
        free(node);
        free(dm);
        return NULL;
    }
    dm->request = *request;
    node->ops = &demux_ops;
    node->priv = dm;
    node->out_ports = calloc(1, sizeof(*node->out_ports));
    if (!node->out_ports) {
        ff_destroy_node(node);
        return NULL;
    }
    node->out_count = 1;
    node->out_ports[0].name = "out";
    return node;
}

/* ── ffmpeg.mux ──────────────────────────────────────────────────── */

/** mux 节点私有状态。 */
struct ffmpeg_muxer {
    rkvc_request request;     /**< 请求副本（读取 output.uri/codec） */
    AVFormatContext *oc;      /**< 输出上下文；首帧创建 */
    AVStream      *st;        /**< 视频流 */
    enum AVCodecID codec_id;  /**< 目标编码 */
    AVCodecParserContext *parser; /**< 求几何（请求未给尺寸时） */
    AVBSFContext  *extract;   /**< extract_extradata：提取 SPS/PPS/OBU */
    int           header_written; /**< avformat_write_header 已调用 */
    uint32_t      width;      /**< 输出几何 */
    uint32_t      height;
    uint8_t      *extradata;  /**< Annex-B SPS/PPS 或 AV1 OBU */
    size_t        extradata_size;
    uint64_t      frame_index; /**< 帧计数（未知时间戳回退） */
};

/** 接受上游任意已解析格式（UNKNOWN 通配）。 */
static int mux_configure(rkvc_node *node, rkvc_diag **diag) {
    rkvc_frame_spec in = {0};
    (void)diag;

    rkvc_port_set_desired(&node->in_ports[0], &in);
    return 0;
}

/** 通过 extract_extradata 从首帧包提取参数集（Annex-B/OBU）。 */
static int mux_collect_extradata(struct ffmpeg_muxer *mx,
                                 const rkvc_frame_desc *desc) {
    AVPacket *pkt = NULL;
    int ret = 0;

    if (!mx->extract || desc->size == 0)
        return 0;
    pkt = av_packet_alloc();
    if (!pkt)
        return (int)RKVC_STATUS_NOMEM;
    if (av_new_packet(pkt, (int)desc->size) < 0) {
        av_packet_free(&pkt);
        return (int)RKVC_STATUS_NOMEM;
    }
    memcpy(pkt->data, desc->data, desc->size);
    pkt->pts = desc->pts == RKVC_FRAME_TS_UNKNOWN ? AV_NOPTS_VALUE
                                                  : desc->pts;
    pkt->dts = desc->dts == RKVC_FRAME_TS_UNKNOWN ? AV_NOPTS_VALUE
                                                  : desc->dts;
    pkt->flags = (desc->flags & RKVC_FRAME_FLAG_KEYFRAME) ? AV_PKT_FLAG_KEY : 0;

    ret = av_bsf_send_packet(mx->extract, pkt); /* 接管 pkt */
    if (ret < 0) {
        av_packet_free(&pkt);
        return (int)RKVC_STATUS_FORMAT;
    }
    for (;;) {
        AVPacket *out = av_packet_alloc();
        uint8_t *side = NULL;
        size_t side_size = 0;
        int r2;
        if (!out) {
            return (int)RKVC_STATUS_NOMEM;
        }
        r2 = av_bsf_receive_packet(mx->extract, out);
        if (r2 == AVERROR(EAGAIN) || r2 == AVERROR_EOF) {
            av_packet_free(&out);
            break;
        }
        if (r2 < 0) {
            av_packet_free(&out);
            return (int)RKVC_STATUS_FORMAT;
        }
        side = av_packet_get_side_data(out, AV_PKT_DATA_NEW_EXTRADATA,
                                       &side_size);
        if (side && side_size && !mx->extradata) {
            mx->extradata = malloc(side_size);
            if (!mx->extradata) {
                av_packet_free(&out);
                return (int)RKVC_STATUS_NOMEM;
            }
            memcpy(mx->extradata, side, side_size);
            mx->extradata_size = side_size;
        }
        av_packet_free(&out);
        if (mx->extradata)
            break;
    }
    return 0;
}

/** 首帧几何未知时用解析器求尺寸；返回 0 表示已确定。 */
static int mux_resolve_geometry(struct ffmpeg_muxer *mx,
                                const rkvc_frame_desc *desc) {
    uint8_t *out = NULL;
    int out_size = 0;
    int ret;

    if (mx->width && mx->height)
        return 0;
    if (!mx->parser)
        return (int)RKVC_STATUS_FORMAT;
    ret = av_parser_parse2(mx->parser, NULL, &out, &out_size,
                           desc->data, (int)desc->size,
                           desc->pts == RKVC_FRAME_TS_UNKNOWN
                               ? AV_NOPTS_VALUE : desc->pts,
                           desc->dts == RKVC_FRAME_TS_UNKNOWN
                               ? AV_NOPTS_VALUE : desc->dts,
                           0);
    if (ret < 0 || out_size < 0)
        return (int)RKVC_STATUS_FORMAT;
    if (mx->parser->width > 0 && mx->parser->height > 0) {
        mx->width = (uint32_t)mx->parser->width;
        mx->height = (uint32_t)mx->parser->height;
        return 0;
    }
    return (int)RKVC_STATUS_FORMAT;
}

/** 首帧时创建输出上下文、流与头（含 avcC/hvcC/av1C 生成）。 */
static int mux_open_header(rkvc_node *node, rkvc_diag **diag) {
    struct ffmpeg_muxer *mx = node->priv;
    AVRational tb = {1, FF_RKVC_TIMEBASE};
    int ret;

    ret = avformat_alloc_output_context2(&mx->oc, NULL, NULL,
                                         mx->request.output.uri);
    if (!mx->oc) {
        /* 后缀不识别的兜底：按裸流判断失败 */
        if (diag)
            rkvc_diag_push(diag, RKVC_STATUS_UNSUPPORTED, 3,
                           node->ops->id, "no muxer for output uri");
        return (int)RKVC_STATUS_UNSUPPORTED;
    }
    mx->st = avformat_new_stream(mx->oc, NULL);
    if (!mx->st)
        return (int)RKVC_STATUS_NOMEM;
    mx->st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    mx->st->codecpar->codec_id = mx->codec_id;
    mx->st->codecpar->width = (int)mx->width;
    mx->st->codecpar->height = (int)mx->height;
    mx->st->time_base = tb;
    if (mx->extradata && mx->extradata_size) {
        mx->st->codecpar->extradata = av_malloc(mx->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!mx->st->codecpar->extradata)
            return (int)RKVC_STATUS_NOMEM;
        memcpy(mx->st->codecpar->extradata, mx->extradata,
               mx->extradata_size);
        memset(mx->st->codecpar->extradata + mx->extradata_size, 0,
               AV_INPUT_BUFFER_PADDING_SIZE);
        mx->st->codecpar->extradata_size = (int)mx->extradata_size;
    }
    if (avio_open(&mx->oc->pb, mx->request.output.uri, AVIO_FLAG_WRITE) < 0) {
        if (diag)
            rkvc_diag_push(diag, RKVC_STATUS_IO, 3, node->ops->id,
                           "avio_open failed");
        return (int)RKVC_STATUS_IO;
    }
    ret = avformat_write_header(mx->oc, NULL);
    if (ret < 0) {
        if (diag)
            rkvc_diag_push(diag, RKVC_STATUS_FORMAT, 3, node->ops->id,
                           "avformat_write_header failed");
        return (int)RKVC_STATUS_FORMAT;
    }
    mx->header_written = 1;
    return 0;
}

/** 处理一帧：首帧定几何/取参数集并写头，随后 av_write_frame。 */
static int mux_process(rkvc_node *node, rkvc_frame *input,
                       rkvc_diag **diag) {
    struct ffmpeg_muxer *mx = node->priv;
    rkvc_frame_desc desc;
    AVPacket *pkt = NULL;
    int64_t pts;
    int rc;

    if (rkvc_frame_get_desc(input, &desc) != RKVC_STATUS_OK)
        return (int)RKVC_STATUS_FORMAT;
    if (desc.spec.fmt != RKVC_FRAME_FMT_BITSTREAM || !desc.data ||
        !desc.size)
        return (int)RKVC_STATUS_FORMAT;

    if (!mx->header_written) {
        rc = mux_resolve_geometry(mx, &desc);
        if (rc != 0) {
            if (diag)
                rkvc_diag_push(diag, RKVC_STATUS_FORMAT, 3, node->ops->id,
                               "cannot determine output geometry");
            return rc;
        }
        rc = mux_collect_extradata(mx, &desc);
        if (rc != 0)
            return rc;
        rc = mux_open_header(node, diag);
        if (rc != 0)
            return rc;
    }

    if (desc.pts != RKVC_FRAME_TS_UNKNOWN && desc.pts >= 0)
        pts = desc.pts;
    else
        pts = (int64_t)mx->frame_index *
              (FF_RKVC_TIMEBASE / 30); /* 无时间戳：按 30fps 走带 */

    pkt = av_packet_alloc();
    if (!pkt)
        return (int)RKVC_STATUS_NOMEM;
    if (av_new_packet(pkt, (int)desc.size) < 0) {
        av_packet_free(&pkt);
        return (int)RKVC_STATUS_NOMEM;
    }
    memcpy(pkt->data, desc.data, desc.size);
    pkt->stream_index = 0;
    pkt->pts = pts;
    pkt->dts = pts; /* 线性输出：DTS == PTS（B 帧重排序不做） */
    pkt->flags = (desc.flags & RKVC_FRAME_FLAG_KEYFRAME) ? AV_PKT_FLAG_KEY : 0;

    rc = av_write_frame(mx->oc, pkt);
    av_packet_free(&pkt);
    if (rc < 0) {
        if (diag)
            rkvc_diag_push(diag, RKVC_STATUS_FORMAT, 3, node->ops->id,
                           "av_write_frame failed");
        return (int)RKVC_STATUS_FORMAT;
    }
    mx->frame_index++;
    return 0;
}

/** 写出 trailer 并关闭输出。 */
static int mux_flush(rkvc_node *node, rkvc_diag **diag) {
    struct ffmpeg_muxer *mx = node->priv;
    int ret = 0;

    if (!mx->header_written)
        return 0; /* 空流 */
    ret = av_write_trailer(mx->oc);
    if (ret < 0) {
        if (diag)
            rkvc_diag_push(diag, RKVC_STATUS_FORMAT, 3, node->ops->id,
                           "av_write_trailer failed");
        return (int)RKVC_STATUS_FORMAT;
    }
    if (mx->oc->pb) {
        avio_close(mx->oc->pb);
        mx->oc->pb = NULL;
    }
    avformat_free_context(mx->oc);
    mx->oc = NULL;
    mx->header_written = 0;
    return 0;
}

/** 释放 mux 资源（幂等）。 */
static void mux_close(rkvc_node *node) {
    struct ffmpeg_muxer *mx = node->priv;
    if (!mx)
        return;
    if (mx->oc) {
        if (mx->oc->pb) {
            avio_close(mx->oc->pb);
            mx->oc->pb = NULL;
        }
        avformat_free_context(mx->oc);
        mx->oc = NULL;
    }
    if (mx->parser)
        av_parser_close(mx->parser);
    if (mx->extract)
        av_bsf_free(&mx->extract);
    free(mx->extradata);
    mx->parser = NULL;
    mx->extract = NULL;
    mx->extradata = NULL;
    mx->extradata_size = 0;
}

static const rkvc_node_ops mux_ops = {
    "ffmpeg.mux", mux_configure, NULL, mux_process, mux_flush,
    mux_close, ff_destroy_node,
};

/** mux 门控：ENCODE/TRANSCODE 且输出非 MLVC（MLVC 有独立容器）。 */
static int mux_matches(rkvc_operation op, rkvc_codec codec,
                       const rkvc_device_caps *caps) {
    (void)caps;
    return (op == RKVC_OPERATION_ENCODE || op == RKVC_OPERATION_TRANSCODE) &&
           codec != RKVC_CODEC_MLVC;
}

/** create：仅对容器后缀输出返回节点；裸流回退 file.sink。 */
static rkvc_node *mux_create(const rkvc_node_factory *factory,
                             const rkvc_request *request,
                             void *create_ctx) {
    rkvc_node *node;
    struct ffmpeg_muxer *mx;
    (void)factory;
    (void)create_ctx;

    if (!request->output.uri ||
        request->output.kind != RKVC_ENDPOINT_FILE ||
        !is_container_ext(request->output.uri))
        return NULL;
    node = calloc(1, sizeof(*node));
    mx = calloc(1, sizeof(*mx));
    if (!node || !mx) {
        free(node);
        free(mx);
        return NULL;
    }
    mx->request = *request;
    mx->codec_id = rkvc_to_avcodec(request->codec);
    if (mx->codec_id == AV_CODEC_ID_NONE) {
        free(node);
        free(mx);
        return NULL;
    }
    if (request->width && request->height) {
        mx->width = request->width;
        mx->height = request->height;
    }
    mx->parser = av_parser_init(mx->codec_id);
    {
        const AVBitStreamFilter *filter = av_bsf_get_by_name("extract_extradata");
        AVCodecParameters *par = avcodec_parameters_alloc();
        if (filter && par) {
            par->codec_type = AVMEDIA_TYPE_VIDEO;
            par->codec_id = mx->codec_id;
            if (av_bsf_alloc(filter, &mx->extract) >= 0 &&
                avcodec_parameters_copy(mx->extract->par_in, par) >= 0 &&
                av_bsf_init(mx->extract) >= 0) {
                /* ok */
            } else {
                av_bsf_free(&mx->extract);
            }
        }
        if (par)
            avcodec_parameters_free(&par);
    }
    node->ops = &mux_ops;
    node->priv = mx;
    node->in_ports = calloc(1, sizeof(*node->in_ports));
    if (!node->in_ports) {
        ff_destroy_node(node);
        return NULL;
    }
    node->in_count = 1;
    node->in_ports[0].name = "in";
    node->in_ports[0].is_input = 1;
    return node;
}

/* ── 后端描述符与注册 ─────────────────────────────────────────────── */

/** probe：FFmpeg 库存在即可用（装载失败由 DSO 加载器隔离）。 */
static int ff_probe(const rkvc_device_caps *caps, void *probe_ctx,
                    rkvc_diag **diag) {
    (void)caps;
    (void)probe_ctx;
    (void)diag;
    return 0;
}

/** 工厂表：demux 与 mux。 */
static const rkvc_node_factory ff_factories[] = {
    {
        .id = "ffmpeg.demux",
        .backend_id = "ffmpeg",
        .stage = RKVC_NODE_STAGE_SOURCE,
        .priority = 1000,
        .matches = demux_matches,
        .create = demux_create,
    },
    {
        .id = "ffmpeg.mux",
        .backend_id = "ffmpeg",
        .stage = RKVC_NODE_STAGE_SINK,
        .priority = 1000,
        .matches = mux_matches,
        .create = mux_create,
    },
};

static const rkvc_node_factory *ff_factory_list(void *probe_ctx,
                                                size_t *count) {
    (void)probe_ctx;
    *count = sizeof(ff_factories) / sizeof(ff_factories[0]);
    return ff_factories;
}

/** 经 rkvc_backend_query() 导出的后端描述符。 */
static const rkvc_backend ff_backend = {
    .abi_version = RKVC_ABI_VERSION,
    .id = "ffmpeg",
    .capability_flags = 0,
    .probe = ff_probe,
    .factories = ff_factory_list,
};

/** DSO 入口：返回静态后端描述符。 */
const rkvc_backend *rkvc_backend_query(void) { return &ff_backend; }
