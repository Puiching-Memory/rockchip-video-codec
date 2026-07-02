/**
 * @file node_svt_enc.c
 * @brief SVT-AV1 软件编码节点。
 *
 * 输入帧经 EbSvtIOFormat 指针直接交给 SVT（send_picture 内部再拷贝一次），
 * 避免应用层整帧重复 memcpy；NV12 仅对 UV 做去交错写入 chroma 暂存区。
 */

#include "internal.h"

struct rkvc_svt_enc {
    EbComponentType           *handle;
    EbSvtAv1EncConfiguration  cfg;
    EbBufferHeaderType       *in_header;
    EbSvtIOFormat            *in_io;
    uint8_t                  *chroma_cb;
    uint8_t                  *chroma_cr;
    int                       width;
    int                       height;
    rkvc_pix_fmt              input_format;
    int                       flushed;
    int                       pic_done;
    int64_t                   next_pts;
};

static rkvc_err svt_check(EbErrorType ret)
{
    if (ret == EB_ErrorNone)
        return RKVC_OK;
    if (ret == EB_NoErrorEmptyQueue)
        return RKVC_ERR_AGAIN;
    if (ret == EB_ErrorInsufficientResources)
        return RKVC_ERR_AGAIN;
    return RKVC_ERR_INTERNAL;
}

static rkvc_err svt_alloc_io(rkvc_svt_enc *enc)
{
    const int w = enc->width;
    const int h = enc->height;
    const size_t chroma_w = (size_t)(w / 2);
    const size_t chroma_h = (size_t)(h / 2);

    enc->in_header = rkvc_calloc(1, sizeof(*enc->in_header));
    enc->in_io     = rkvc_calloc(1, sizeof(*enc->in_io));
    if (!enc->in_header || !enc->in_io)
        return RKVC_ERR_NOMEM;

    enc->chroma_cb = rkvc_malloc(chroma_w * chroma_h);
    enc->chroma_cr = rkvc_malloc(chroma_w * chroma_h);
    if (!enc->chroma_cb || !enc->chroma_cr)
        return RKVC_ERR_NOMEM;

    enc->in_header->size     = sizeof(*enc->in_header);
    enc->in_header->p_buffer = (uint8_t *)enc->in_io;
    return RKVC_OK;
}

static void deinterleave_nv12_chroma(const AVFrame *frame, int width, int height,
                                     uint8_t *cb, uint8_t *cr)
{
    const int chroma_w = width / 2;
    const int chroma_h = height / 2;
    const uint8_t *uv = frame->data[1];
    const int uv_stride = frame->linesize[1];

    for (int y = 0; y < chroma_h; y++) {
        const uint8_t *row = uv + y * uv_stride;
        uint8_t *cb_row = cb + y * chroma_w;
        uint8_t *cr_row = cr + y * chroma_w;
        for (int x = 0; x < chroma_w; x++) {
            cb_row[x] = row[x * 2];
            cr_row[x] = row[x * 2 + 1];
        }
    }
}

static void bind_io_from_frame(rkvc_svt_enc *enc, const AVFrame *frame)
{
    EbSvtIOFormat *io = enc->in_io;

    io->luma     = frame->data[0];
    io->y_stride = (uint32_t)frame->linesize[0];

    if (frame->format == AV_PIX_FMT_YUV420P) {
        io->cb        = frame->data[1];
        io->cr        = frame->data[2];
        io->cb_stride = (uint32_t)frame->linesize[1];
        io->cr_stride = (uint32_t)frame->linesize[2];
        return;
    }

    io->cb_stride = (uint32_t)(enc->width / 2);
    io->cr_stride = (uint32_t)(enc->width / 2);
    io->cb        = enc->chroma_cb;
    io->cr        = enc->chroma_cr;
    deinterleave_nv12_chroma(frame, enc->width, enc->height,
                             enc->chroma_cb, enc->chroma_cr);
}

static rkvc_err svt_prepare_work_frame(const rkvc_buffer *frame,
                                       rkvc_buffer **work_out,
                                       rkvc_buffer **tmp_out,
                                       int *dmabuf_mapped)
{
    *work_out = NULL;
    *tmp_out = NULL;
    *dmabuf_mapped = 0;

    if (frame->mem_type == RKVC_MEM_HOST && frame->av_frame) {
        *work_out = rkvc_buffer_ref((rkvc_buffer *)frame);
        return RKVC_OK;
    }

    if (frame->mem_type == RKVC_MEM_DMABUF && frame->av_frame &&
        frame->av_frame->format != AV_PIX_FMT_DRM_PRIME &&
        frame->av_frame->data[0]) {
        rkvc_err err = rkvc_buffer_dmabuf_begin_cpu_read(frame);
        if (err != RKVC_OK)
            return err;
        *dmabuf_mapped = 1;
        *work_out = rkvc_buffer_ref((rkvc_buffer *)frame);
        return RKVC_OK;
    }

    rkvc_buffer *host = NULL;
    rkvc_err err = rkvc_dma_to_host(frame, &host);
    if (err != RKVC_OK)
        return err;
    *tmp_out = host;
    *work_out = host;
    return RKVC_OK;
}

rkvc_err rkvc_svt_enc_open(rkvc_svt_enc **out, const rkvc_svt_enc_config *cfg)
{
    if (!out || !cfg || cfg->width <= 0 || cfg->height <= 0)
        return RKVC_ERR_INVALID;

    *out = NULL;
    rkvc_init();

    rkvc_svt_enc *enc = rkvc_calloc(1, sizeof(*enc));
    if (!enc)
        return RKVC_ERR_NOMEM;

    enc->width  = cfg->width;
    enc->height = cfg->height;
    enc->input_format = cfg->input_format;

    EbErrorType ret = svt_av1_enc_init_handle(&enc->handle, &enc->cfg);
    if (ret != EB_ErrorNone) {
        rkvc_svt_enc_close(enc);
        return RKVC_ERR_NOT_FOUND;
    }

    enc->cfg.source_width  = (uint32_t)cfg->width;
    enc->cfg.source_height = (uint32_t)cfg->height;
    enc->cfg.frame_rate_numerator   = (uint32_t)cfg->fps_num;
    enc->cfg.frame_rate_denominator = (uint32_t)cfg->fps_den;
    enc->cfg.encoder_color_format   = EB_YUV420;
    enc->cfg.encoder_bit_depth      = 8;
    enc->cfg.enc_mode = (uint8_t)(cfg->svt_preset > 0 ? cfg->svt_preset
                                                       : RKVC_SVT_PRESET_PERF);
    enc->cfg.level_of_parallelism = (uint32_t)(cfg->svt_lp >= 0 ? cfg->svt_lp
                                                                 : RKVC_SVT_LP_AUTO);
    enc->cfg.rtc = cfg->svt_rtc ? true : false;
    enc->cfg.target_bit_rate        = (uint32_t)cfg->bitrate;
    switch (cfg->rc_mode) {
    case RKVC_RC_CBR:
        /* SVT 文件/转码为 random-access，CBR 会初始化失败；对齐为 VBR。 */
        enc->cfg.rate_control_mode = SVT_AV1_RC_MODE_VBR;
        break;
    case RKVC_RC_CQP:
        enc->cfg.rate_control_mode = SVT_AV1_RC_MODE_CQP_OR_CRF;
        break;
    case RKVC_RC_VBR:
    default:
        enc->cfg.rate_control_mode = SVT_AV1_RC_MODE_VBR;
        break;
    }
    if (cfg->gop_size > 1)
        enc->cfg.intra_period_length = cfg->gop_size - 1;

    ret = svt_av1_enc_set_parameter(enc->handle, &enc->cfg);
    if (ret != EB_ErrorNone) {
        rkvc_svt_enc_close(enc);
        return RKVC_ERR_INVALID;
    }

    ret = svt_av1_enc_init(enc->handle);
    if (ret != EB_ErrorNone) {
        rkvc_svt_enc_close(enc);
        return RKVC_ERR_INTERNAL;
    }

    if (svt_alloc_io(enc) != RKVC_OK) {
        rkvc_svt_enc_close(enc);
        return RKVC_ERR_NOMEM;
    }

    *out = enc;
    return RKVC_OK;
}

void rkvc_svt_enc_close(rkvc_svt_enc *enc)
{
    if (!enc)
        return;

    if (enc->handle) {
        svt_av1_enc_deinit(enc->handle);
        svt_av1_enc_deinit_handle(enc->handle);
    }
    rkvc_free(enc->chroma_cb);
    rkvc_free(enc->chroma_cr);
    rkvc_free(enc->in_io);
    rkvc_free(enc->in_header);
    rkvc_free(enc);
}

rkvc_err rkvc_svt_enc_write_header(rkvc_svt_enc *enc, AVCodecParameters *par)
{
    if (!enc || !enc->handle || !par)
        return RKVC_ERR_INVALID;

    EbBufferHeaderType *hdr = NULL;
    EbErrorType ret = svt_av1_enc_stream_header(enc->handle, &hdr);
    if (ret != EB_ErrorNone || !hdr || hdr->n_filled_len == 0)
        return RKVC_ERR_MUX;

    par->extradata = av_mallocz(hdr->n_filled_len + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!par->extradata) {
        svt_av1_enc_stream_header_release(hdr);
        return RKVC_ERR_NOMEM;
    }
    memcpy(par->extradata, hdr->p_buffer, hdr->n_filled_len);
    par->extradata_size = (int)hdr->n_filled_len;
    svt_av1_enc_stream_header_release(hdr);
    return RKVC_OK;
}

rkvc_err rkvc_svt_enc_send_frame(rkvc_svt_enc *enc, rkvc_buffer *frame)
{
    if (!enc || !enc->handle)
        return RKVC_ERR_INVALID;
    if (enc->flushed)
        return RKVC_ERR_EOF;
    if (!frame)
        return rkvc_svt_enc_drain(enc);

    rkvc_buffer *work = NULL;
    rkvc_buffer *tmp = NULL;
    int dmabuf_mapped = 0;
    rkvc_err err = svt_prepare_work_frame(frame, &work, &tmp, &dmabuf_mapped);
    if (err != RKVC_OK)
        return err;

    if (!work->av_frame) {
        rkvc_buffer_unref(tmp);
        if (dmabuf_mapped)
            rkvc_buffer_dmabuf_end_cpu_read(frame);
        return RKVC_ERR_INVALID;
    }

    bind_io_from_frame(enc, work->av_frame);

    const uint32_t y_size = (uint32_t)((size_t)enc->width * (size_t)enc->height);
    enc->in_header->n_filled_len = y_size + 2 * (y_size / 4);
    enc->in_header->pts = work->pts >= 0 ? work->pts : enc->next_pts++;
    enc->in_header->dts = enc->in_header->pts;

    EbErrorType ret = svt_av1_enc_send_picture(enc->handle, enc->in_header);
    err = svt_check(ret);

    rkvc_buffer_unref(tmp);
    if (dmabuf_mapped)
        rkvc_buffer_dmabuf_end_cpu_read(frame);

    return err;
}

rkvc_err rkvc_svt_enc_receive_packet(rkvc_svt_enc *enc, rkvc_buffer **out)
{
    if (!enc || !enc->handle || !out)
        return RKVC_ERR_INVALID;

    *out = NULL;
    EbBufferHeaderType *pkt = NULL;
    EbErrorType ret = svt_av1_enc_get_packet(enc->handle, &pkt,
                                             (uint8_t)enc->pic_done);
    rkvc_err err = svt_check(ret);
    if (err != RKVC_OK)
        return err;
    if (!pkt)
        return enc->pic_done ? RKVC_ERR_EOF : RKVC_ERR_AGAIN;

    if (pkt->flags & EB_BUFFERFLAG_EOS) {
        svt_av1_enc_release_out_buffer(&pkt);
        return RKVC_ERR_EOF;
    }
    if (pkt->n_filled_len == 0) {
        svt_av1_enc_release_out_buffer(&pkt);
        return RKVC_ERR_AGAIN;
    }

    rkvc_buffer *b = rkvc_calloc(1, sizeof(*b));
    if (!b) {
        svt_av1_enc_release_out_buffer(&pkt);
        return RKVC_ERR_NOMEM;
    }
    b->kind = RKVC_BUF_BITSTREAM;
    pthread_mutex_init(&b->lock, NULL);
    b->ref_count = 1;
    b->data = rkvc_malloc(pkt->n_filled_len);
    if (!b->data) {
        svt_av1_enc_release_out_buffer(&pkt);
        rkvc_buffer_unref(b);
        return RKVC_ERR_NOMEM;
    }
    memcpy(b->data, pkt->p_buffer, pkt->n_filled_len);
    b->size = pkt->n_filled_len;
    b->owns_data = 1;
    b->pts = pkt->pts;
    b->dts = pkt->dts;
    b->key_frame = (pkt->pic_type == EB_AV1_KEY_PICTURE ||
                      pkt->pic_type == EB_AV1_INTRA_ONLY_PICTURE) ? 1 : 0;

    svt_av1_enc_release_out_buffer(&pkt);
    *out = b;
    return RKVC_OK;
}

rkvc_err rkvc_svt_enc_drain(rkvc_svt_enc *enc)
{
    if (!enc || !enc->handle)
        return RKVC_ERR_INVALID;
    if (enc->flushed)
        return RKVC_OK;

    EbBufferHeaderType eos = {0};
    eos.flags = EB_BUFFERFLAG_EOS;
    EbErrorType ret = svt_av1_enc_send_picture(enc->handle, &eos);
    enc->flushed = 1;
    enc->pic_done = 1;
    return svt_check(ret);
}
