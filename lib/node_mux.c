/**
 * @file node_mux.c
 * @brief FFmpeg 封装节点。
 */

#include "internal.h"

struct rkvc_mux {
    AVFormatContext  *fmt;
    AVStream         *stream;
    AVPacket         *pkt;
    int               header_written;
    /* 编码器侧时间基（帧序号：1/fps）；write_header 后 stream->time_base 可能被 muxer 改写 */
    AVRational        pkt_time_base;
    AVRational        frame_rate;
};

static const char *guess_muxer(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot)
        return "mp4";
    dot++;
    if (strcasecmp(dot, "mp4") == 0)  return "mp4";
    if (strcasecmp(dot, "mkv") == 0)  return "matroska";
    if (strcasecmp(dot, "ts") == 0)   return "mpegts";
    if (strcasecmp(dot, "ivf") == 0)  return "ivf";
    return "mp4";
}

static enum AVCodecID route_to_av_codec(const rkvc_route_plan *route)
{
    if (!route)
        return AV_CODEC_ID_H264;
    switch (route->codec) {
    case RKVC_CODEC_HEVC: return AV_CODEC_ID_HEVC;
    case RKVC_CODEC_AV1:  return AV_CODEC_ID_AV1;
    default:              return AV_CODEC_ID_H264;
    }
}

rkvc_err rkvc_mux_open(rkvc_mux **out, const rkvc_mux_config *cfg,
                       AVCodecParameters *src_par)
{
    if (!out || !cfg || !cfg->output_path || !cfg->route)
        return RKVC_ERR_INVALID;

    *out = NULL;
    rkvc_mux *m = rkvc_calloc(1, sizeof(*m));
    if (!m)
        return RKVC_ERR_NOMEM;

    const char *muxer = guess_muxer(cfg->output_path);
    int ret = avformat_alloc_output_context2(&m->fmt, NULL, muxer,
                                             cfg->output_path);
    if (ret < 0) {
        rkvc_mux_close(m);
        return rkvc_from_averror(ret);
    }

    m->stream = avformat_new_stream(m->fmt, NULL);
    if (!m->stream) {
        rkvc_mux_close(m);
        return RKVC_ERR_MUX;
    }

    if (src_par && src_par->codec_id != AV_CODEC_ID_NONE) {
        ret = avcodec_parameters_copy(m->stream->codecpar, src_par);
    } else {
        m->stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        m->stream->codecpar->codec_id   = route_to_av_codec(cfg->route);
        m->stream->codecpar->width      = cfg->width;
        m->stream->codecpar->height     = cfg->height;
        m->stream->codecpar->bit_rate   = cfg->bitrate;
        m->stream->codecpar->format     = AV_PIX_FMT_YUV420P;
    }

    if (ret < 0) {
        rkvc_mux_close(m);
        return rkvc_from_averror(ret);
    }

    int fps_num = cfg->fps_num > 0 ? cfg->fps_num : 30;
    int fps_den = cfg->fps_den > 0 ? cfg->fps_den : 1;
    m->pkt_time_base = (AVRational){fps_den, fps_num};
    m->frame_rate = (AVRational){fps_num, fps_den};
    m->stream->time_base = m->pkt_time_base;
    m->stream->avg_frame_rate = m->frame_rate;
    m->stream->r_frame_rate = m->frame_rate;

    if (!(m->fmt->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&m->fmt->pb, cfg->output_path, AVIO_FLAG_WRITE);
        if (ret < 0) {
            rkvc_mux_close(m);
            return rkvc_from_averror(ret);
        }
    }

    ret = avformat_write_header(m->fmt, NULL);
    if (ret < 0) {
        rkvc_mux_close(m);
        return rkvc_from_averror(ret);
    }
    m->header_written = 1;

    m->pkt = av_packet_alloc();
    if (!m->pkt) {
        rkvc_mux_close(m);
        return RKVC_ERR_NOMEM;
    }

    *out = m;
    return RKVC_OK;
}

void rkvc_mux_close(rkvc_mux *m)
{
    if (!m)
        return;

    if (m->fmt) {
        if (m->header_written)
            av_write_trailer(m->fmt);
        if (!(m->fmt->oformat->flags & AVFMT_NOFILE) && m->fmt->pb)
            avio_closep(&m->fmt->pb);
        avformat_free_context(m->fmt);
    }
    if (m->pkt)
        av_packet_free(&m->pkt);
    rkvc_free(m);
}

rkvc_err rkvc_mux_write_packet(rkvc_mux *m, const rkvc_buffer *pkt)
{
    if (!m || !pkt || pkt->kind != RKVC_BUF_BITSTREAM)
        return RKVC_ERR_INVALID;

    av_packet_unref(m->pkt);
    if (av_new_packet(m->pkt, (int)pkt->size) < 0)
        return RKVC_ERR_NOMEM;

    memcpy(m->pkt->data, pkt->data, pkt->size);
    m->pkt->pts = av_rescale_q(pkt->pts, m->pkt_time_base, m->stream->time_base);
    {
        int64_t dts_src = pkt->dts >= 0 ? pkt->dts : pkt->pts;
        m->pkt->dts = av_rescale_q(dts_src, m->pkt_time_base, m->stream->time_base);
    }
    m->pkt->duration = av_rescale_q(1, av_inv_q(m->frame_rate), m->stream->time_base);
    m->pkt->stream_index = m->stream->index;
    if (pkt->key_frame)
        m->pkt->flags |= AV_PKT_FLAG_KEY;

    int ret = av_interleaved_write_frame(m->fmt, m->pkt);
    av_packet_unref(m->pkt);
    return rkvc_from_averror(ret);
}
