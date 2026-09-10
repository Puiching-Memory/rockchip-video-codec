// SPDX-License-Identifier: AGPL-3.0-or-later
#include "av1/encoder.hpp"

#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>
#include <vector>

#ifdef __linux__
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "EbSvtAv1Enc.h"

namespace av1 {

namespace {

constexpr uint32_t kFps = 30;
constexpr uint32_t kDefaultGop = 60;
constexpr int64_t kTsBase = 90000;

void release_packet(void* p) noexcept {
    free(p);
}

#ifdef __linux__
void dmabuf_read_sync(int fd, unsigned long flags) noexcept {
    struct dma_buf_sync sync = {};
    sync.flags = flags;
    (void)ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}
#endif

}  // namespace

struct SvtEncoderNode::Impl {
    rkvc::Request req;
    EbComponentType* handle = nullptr;
    EbSvtAv1EncConfiguration config = {};
    EbBufferHeaderType in_hdr = {};
    EbSvtIOFormat in_io = {};
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> i420;
    uint64_t frame_index = 0;
    bool initialized = false;
    bool eos_sent = false;
    rkvc::Emit* emit = nullptr;
};

SvtEncoderNode::SvtEncoderNode(rkvc::Request req)
    : impl_(new(std::nothrow) Impl()) {
    if (impl_)
        impl_->req = std::move(req);
}

SvtEncoderNode::~SvtEncoderNode() {
    if (impl_ && impl_->handle) {
        svt_av1_enc_deinit(impl_->handle);
        svt_av1_enc_deinit_handle(impl_->handle);
    }
}

std::vector<rkvc::Port> SvtEncoderNode::make_ports() const {
    rkvc::Port in, out;
    in.name = "video";
    in.is_input = true;
    out.name = "bitstream";
    out.desired.fmt = rkvc::PixelFormat::Bitstream;
    out.desired.domain = rkvc::MemDomain::Host;
    return {in, out};
}

rkvc::Status SvtEncoderNode::configure(std::vector<rkvc::Port>& ports,
                                       rkvc::Diag* diag) {
    for (const auto& p : ports) {
        if (p.is_input && p.resolved.fmt != rkvc::PixelFormat::Nv12 &&
            p.resolved.fmt != rkvc::PixelFormat::Yuv420P) {
            if (diag)
                diag->add("configure", "svt.encode",
                          "svt accepts NV12/YUV420P input only");
            return rkvc::Status::Negotiate;
        }
    }
    return rkvc::Node::configure(ports, diag);
}

rkvc::Status SvtEncoderNode::open(rkvc::Emit* emit, rkvc::Diag* diag) {
    if (!impl_)
        return rkvc::Status::Nomem;
    if (!emit) {
        if (diag)
            diag->add("open", "svt.encode", "null emit");
        return rkvc::Status::Invalid;
    }
    impl_->emit = emit;
    return rkvc::Status::Ok;  // geometry unknown until first frame
}

namespace {

rkvc::Status emit_packet(rkvc::Emit* emit, EbBufferHeaderType* hdr) {
    if (!hdr || !hdr->p_buffer || !hdr->n_filled_len)
        return rkvc::Status::Ok;
    void* data = malloc(hdr->n_filled_len);
    if (!data)
        return rkvc::Status::Nomem;
    memcpy(data, hdr->p_buffer, hdr->n_filled_len);
    rkvc::Spec spec;
    spec.fmt = rkvc::PixelFormat::Bitstream;
    auto fr = rkvc::Frame::borrow_host(spec, data, hdr->n_filled_len,
                                       {release_packet, data});
    if (!fr) {
        free(data);
        return fr.status();
    }
    fr.value()->set_pts(hdr->pts);
    fr.value()->set_dts(hdr->dts);
    if (hdr->pic_type == EB_AV1_KEY_PICTURE)
        fr.value()->set_flags(rkvc::kFlagKeyframe);
    rkvc::FramePtr out = fr.value();
    rkvc::Status st = emit->emit(0, std::move(out));
    if (st != rkvc::Status::Ok)
        return st;
    return rkvc::Status::Ok;
}

rkvc::Status drain_packets(SvtEncoderNode::Impl* enc, rkvc::Diag* diag) {
    for (;;) {
        EbBufferHeaderType* hdr = nullptr;
        // After EOS was sent, block until the EOS-marked packet arrives;
        // before that only poll, or the call asserts / misjudges drain.
        uint8_t pic_send_done = enc->eos_sent ? 1 : 0;
        EbErrorType ret =
            svt_av1_enc_get_packet(enc->handle, &hdr, pic_send_done);
        if (ret == EB_NoErrorEmptyQueue)
            return rkvc::Status::Ok;
        if (ret != EB_ErrorNone) {
            if (diag)
                diag->add("drain", "svt.encode", "get_packet failed");
            return rkvc::Status::Hw;
        }
        if (!hdr)
            continue;
        bool packet_eos = (hdr->flags & EB_BUFFERFLAG_EOS) != 0;
        rkvc::Status st = emit_packet(enc->emit, hdr);
        svt_av1_enc_release_out_buffer(&hdr);
        if (st != rkvc::Status::Ok)
            return st;
        if (packet_eos)
            return rkvc::Status::Ok;
    }
}

rkvc::Status init_locked(SvtEncoderNode::Impl* enc, rkvc::Diag* diag) {
    EbSvtAv1EncConfiguration* cfg = &enc->config;
    // 4.x 起 init_handle 只收 handle 与 config（旧版的 app_data 参数已删），
    // 由它把库内默认值灌进 *cfg。
    if (svt_av1_enc_init_handle(&enc->handle, cfg) != EB_ErrorNone)
        return rkvc::Status::Hw;
    cfg->source_width = enc->width;
    cfg->source_height = enc->height;
    cfg->encoder_bit_depth = 8;
    cfg->encoder_color_format = EB_YUV420;
    cfg->frame_rate_numerator = kFps;
    cfg->frame_rate_denominator = 1;
    cfg->intra_period_length = enc->req.quality.gop_size
                                   ? (int32_t)enc->req.quality.gop_size - 1
                                   : (int32_t)kDefaultGop;
    // 4.x 把 pred_structure 从 uint8_t 收紧为 PredStructure 枚举。
    cfg->pred_structure = RANDOM_ACCESS;
    if (enc->req.quality.gop_size) {
        cfg->intra_refresh_type = SVT_AV1_KF_REFRESH;
        cfg->scene_change_detection = 0;
    }
    if (enc->req.quality.qp >= 0) {
        cfg->rate_control_mode = SVT_AV1_RC_MODE_CQP_OR_CRF;
        cfg->qp = (uint32_t)enc->req.quality.qp;
    } else {
        int64_t bps = enc->req.quality.bitrate_bps > 0
                          ? enc->req.quality.bitrate_bps
                          : (int64_t)enc->width * enc->height * 3;
        cfg->rate_control_mode = SVT_AV1_RC_MODE_VBR;
        cfg->target_bit_rate = (uint32_t)bps;
    }
    auto fail = [&] {
        if (diag)
            diag->add("open", "svt.encode", "svt encoder init failed");
        if (enc->handle) {
            svt_av1_enc_deinit(enc->handle);
            svt_av1_enc_deinit_handle(enc->handle);
            enc->handle = nullptr;
        }
        enc->initialized = false;
        return rkvc::Status::Hw;
    };
    if (svt_av1_enc_set_parameter(enc->handle, cfg) != EB_ErrorNone)
        return fail();
    if (svt_av1_enc_init(enc->handle) != EB_ErrorNone)
        return fail();
    enc->initialized = true;
    return rkvc::Status::Ok;
}

}  // namespace

rkvc::Status SvtEncoderNode::process(rkvc::FramePtr input, rkvc::Diag* diag) {
    Impl* enc = impl_.get();
    if (!enc)
        return rkvc::Status::Nomem;
    if (!input) {
        if (diag)
            diag->add("process", "svt.encode", "null frame");
        return rkvc::Status::Invalid;
    }
    const rkvc::Spec& spec = input->spec();
    const uint8_t* base = static_cast<const uint8_t*>(input->data());
    void* mapped = nullptr;
#ifdef __linux__
    if (spec.domain == rkvc::MemDomain::Dmabuf) {
        if (input->size() == 0 || input->fd() < 0)
            return rkvc::Status::Format;
        mapped =
            mmap(nullptr, input->size(), PROT_READ, MAP_SHARED, input->fd(), 0);
        if (mapped == MAP_FAILED)
            return rkvc::Status::Io;
        dmabuf_read_sync(input->fd(), DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
        base = static_cast<const uint8_t*>(mapped);
    } else
#endif
    {
        // Every later memcpy reads from this buffer, so a null base is fatal
        // regardless of the advertised size.
        if (!base)
            return rkvc::Status::Format;
    }
    auto unmap = [&] {
#ifdef __linux__
        if (mapped) {
            dmabuf_read_sync(input->fd(), DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
            munmap(mapped, input->size());
        }
#else
        (void)input;
#endif
    };

    if (!enc->initialized) {
        if (!spec.width || !spec.height || (spec.width & 1u) ||
            (spec.height & 1u)) {
            unmap();
            if (diag)
                diag->add("process", "svt.encode",
                          "svt requires even width/height");
            return rkvc::Status::Format;
        }
        enc->width = spec.width;
        enc->height = spec.height;
        rkvc::Status st = init_locked(enc, diag);
        if (st != rkvc::Status::Ok) {
            unmap();
            return st;
        }
    } else if (spec.width != enc->width || spec.height != enc->height) {
        unmap();
        return rkvc::Status::Format;
    }

    uint32_t w = enc->width, h = enc->height;
    uint32_t cw = w / 2, ch = h / 2;
    uint32_t ystride = spec.stride ? spec.stride : w;
    uint32_t vstride = spec.ver_stride ? spec.ver_stride : h;
    size_t need = (size_t)w * h * 3 / 2;
    if (enc->i420.size() < need)
        enc->i420.assign(need + 64 * 1024, 0);
    const uint8_t* y = base;
    const uint8_t* uv = base + (size_t)ystride * vstride;
    if (spec.fmt == rkvc::PixelFormat::Nv12) {
        for (uint32_t row = 0; row < h; ++row)
            memcpy(enc->i420.data() + (size_t)row * w,
                   y + (size_t)row * ystride, w);
        for (uint32_t row = 0; row < ch; ++row) {
            const uint8_t* s = uv + (size_t)row * ystride;
            uint8_t* u = enc->i420.data() + (size_t)w * h + (size_t)row * cw;
            uint8_t* v = u + (size_t)cw * ch;
            for (uint32_t x = 0; x < cw; ++x) {
                u[x] = s[2 * x];
                v[x] = s[2 * x + 1];
            }
        }
    } else if (spec.fmt == rkvc::PixelFormat::Yuv420P) {
        const uint8_t* u = uv;
        const uint8_t* v = u + (size_t)ystride * vstride / 4;
        uint32_t us = ystride / 2;
        uint8_t* du = enc->i420.data() + (size_t)w * h;
        uint8_t* dv = du + (size_t)cw * ch;
        for (uint32_t row = 0; row < h; ++row)
            memcpy(enc->i420.data() + (size_t)row * w,
                   y + (size_t)row * ystride, w);
        for (uint32_t row = 0; row < ch; ++row) {
            memcpy(du + (size_t)row * cw, u + (size_t)row * us, cw);
            memcpy(dv + (size_t)row * cw, v + (size_t)row * us, cw);
        }
    } else {
        unmap();
        if (diag)
            diag->add("process", "svt.encode",
                      "svt accepts NV12/YUV420P input only");
        return rkvc::Status::Format;
    }
    unmap();

    EbSvtIOFormat* io = &enc->in_io;
    memset(io, 0, sizeof(*io));
    io->luma = enc->i420.data();
    io->cb = enc->i420.data() + (size_t)w * h;
    io->cr = io->cb + (size_t)cw * ch;
    io->y_stride = w;
    io->cb_stride = cw;
    io->cr_stride = cw;

    EbBufferHeaderType* hdr = &enc->in_hdr;
    memset(hdr, 0, sizeof(*hdr));
    hdr->size = sizeof(*hdr);
    hdr->p_buffer = reinterpret_cast<uint8_t*>(io);
    hdr->n_filled_len = (uint32_t)need;
    hdr->pic_type = EB_AV1_INVALID_PICTURE;
    if (input->encode().force_idr)
        hdr->pic_type = EB_AV1_KEY_PICTURE;
    if (input->pts() != rkvc::kTsUnknown && input->pts() >= 0) {
        hdr->pts = input->pts();
        hdr->dts = input->dts() >= 0 ? input->dts() : input->pts();
    } else {
        hdr->pts = (int64_t)enc->frame_index * kTsBase / kFps;
        hdr->dts = hdr->pts;
    }
    if (svt_av1_enc_send_picture(enc->handle, hdr) != EB_ErrorNone) {
        if (diag)
            diag->add("process", "svt.encode", "send_picture failed");
        return rkvc::Status::Hw;
    }
    ++enc->frame_index;
    return drain_packets(enc, diag);
}

rkvc::Status SvtEncoderNode::flush(rkvc::Diag* diag) {
    Impl* enc = impl_.get();
    if (!enc)
        return rkvc::Status::Nomem;
    if (!enc->initialized)
        return rkvc::Status::Ok;
    memset(&enc->in_hdr, 0, sizeof(enc->in_hdr));
    enc->in_hdr.size = sizeof(enc->in_hdr);
    enc->in_hdr.flags = EB_BUFFERFLAG_EOS;
    enc->in_hdr.pic_type = EB_AV1_INVALID_PICTURE;
    if (svt_av1_enc_send_picture(enc->handle, &enc->in_hdr) != EB_ErrorNone) {
        if (diag)
            diag->add("flush", "svt.encode", "eos send failed");
        return rkvc::Status::Hw;
    }
    enc->eos_sent = true;
    return drain_packets(enc, diag);
}

void SvtEncoderNode::close() noexcept {
    Impl* enc = impl_.get();
    if (!enc)
        return;
    if (enc->handle) {
        svt_av1_enc_deinit(enc->handle);
        svt_av1_enc_deinit_handle(enc->handle);
        enc->handle = nullptr;
    }
    enc->initialized = false;
    enc->i420.clear();
    enc->i420.shrink_to_fit();
}

bool SvtEncodeFactory::matches(const rkvc::Request& r,
                               const rkvc::DeviceCaps&) const noexcept {
    return (r.operation == rkvc::Operation::Encode ||
            r.operation == rkvc::Operation::Transcode) &&
           r.codec == rkvc::Codec::Av1;
}

int SvtEncodeFactory::score(const rkvc::Request& r,
                            const rkvc::DeviceCaps&) const noexcept {
    return (r.policy == rkvc::Policy::Quality ||
            r.policy == rkvc::Policy::Offline)
               ? 50
               : 0;
}

rkvc::Result<rkvc::NodePtr> SvtEncodeFactory::create(const rkvc::Request& r,
                                                     rkvc::Diag*) const {
    rkvc::NodePtr n(new (std::nothrow) SvtEncoderNode(r));
    if (!n)
        return rkvc::Result<rkvc::NodePtr>::failure(rkvc::Status::Nomem);
    return rkvc::Result<rkvc::NodePtr>::success(std::move(n));
}

}  // namespace av1
