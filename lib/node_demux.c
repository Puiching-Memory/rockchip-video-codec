/**
 * @file node_demux.c
 * @brief FFmpeg 解封装节点（仅容器层）。
 */

#include "internal.h"

struct rkvc_demux {
    AVFormatContext  *fmt;
    int               video_idx;
    AVPacket         *pkt;
    int               eof;
};

rkvc_err rkvc_demux_open(rkvc_demux **out, const rkvc_demux_config *cfg)
{
    if (!out || !cfg || !cfg->input_path)
        return RKVC_ERR_INVALID;

    *out = NULL;
    rkvc_init();

    rkvc_demux *d = rkvc_calloc(1, sizeof(*d));
    if (!d)
        return RKVC_ERR_NOMEM;

    d->video_idx = -1;

    AVDictionary *fmt_opts = NULL;
    rkvc_err perr = rkvc_dict_parse_opts(&fmt_opts, cfg->format_opts);
    if (perr != RKVC_OK) {
        rkvc_demux_close(d);
        return perr;
    }

    rkvc_err err = rkvc_format_open_input(&d->fmt, cfg->input_path, &fmt_opts);
    rkvc_dict_free(&fmt_opts);
    if (err != RKVC_OK) {
        rkvc_demux_close(d);
        return err;
    }

    int ret = avformat_find_stream_info(d->fmt, NULL);
    if (ret < 0) {
        rkvc_demux_close(d);
        return rkvc_from_averror(ret);
    }

    for (unsigned i = 0; i < d->fmt->nb_streams; i++) {
        if (d->fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            d->video_idx = (int)i;
            break;
        }
    }

    if (d->video_idx < 0) {
        rkvc_demux_close(d);
        return RKVC_ERR_NOT_FOUND;
    }

    d->pkt = av_packet_alloc();
    if (!d->pkt) {
        rkvc_demux_close(d);
        return RKVC_ERR_NOMEM;
    }

    *out = d;
    return RKVC_OK;
}

void rkvc_demux_close(rkvc_demux *d)
{
    if (!d)
        return;
    if (d->pkt)
        av_packet_free(&d->pkt);
    if (d->fmt)
        avformat_close_input(&d->fmt);
    rkvc_free(d);
}

int rkvc_demux_video_stream_index(const rkvc_demux *d)
{
    return d ? d->video_idx : -1;
}

AVCodecParameters *rkvc_demux_video_par(rkvc_demux *d)
{
    if (!d || d->video_idx < 0)
        return NULL;
    return d->fmt->streams[d->video_idx]->codecpar;
}

rkvc_err rkvc_demux_read_packet(rkvc_demux *d, rkvc_buffer **pkt_out)
{
    if (!d || !pkt_out)
        return RKVC_ERR_INVALID;

    *pkt_out = NULL;
    if (d->eof)
        return RKVC_ERR_EOF;

    av_packet_unref(d->pkt);

    for (;;) {
        int ret = av_read_frame(d->fmt, d->pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                d->eof = 1;
                return RKVC_ERR_EOF;
            }
            return rkvc_from_averror(ret);
        }

        if (d->pkt->stream_index == d->video_idx)
            break;
        av_packet_unref(d->pkt);
    }

    rkvc_buffer *b = rkvc_calloc(1, sizeof(*b));
    if (!b)
        return RKVC_ERR_NOMEM;

    b->kind = RKVC_BUF_BITSTREAM;
    pthread_mutex_init(&b->lock, NULL);
    b->ref_count = 1;

    b->data = rkvc_malloc((size_t)d->pkt->size);
    if (!b->data) {
        rkvc_buffer_unref(b);
        return RKVC_ERR_NOMEM;
    }
    memcpy(b->data, d->pkt->data, (size_t)d->pkt->size);
    b->size      = (size_t)d->pkt->size;
    b->owns_data = 1;
    b->pts       = d->pkt->pts;
    b->dts       = d->pkt->dts;
    b->key_frame = (d->pkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0;

    *pkt_out = b;
    return RKVC_OK;
}
