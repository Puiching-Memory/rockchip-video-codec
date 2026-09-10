// SPDX-License-Identifier: AGPL-3.0-or-later
#include "h264h265/encoder.hpp"

#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>

#ifdef __linux__
#include <sys/mman.h>
#endif

#include "h264h265/detail.hpp"
#include "rk_mpi.h"

namespace h264h265 {

namespace {

constexpr uint32_t kDefaultFps = 30;
constexpr uint32_t kDefaultGop = 60;

void free_packet(void* p) noexcept {
    free(p);
}

MppCodingType to_mpp_coding(rkvc::Codec c) noexcept {
    switch (c) {
        case rkvc::Codec::H264:
            return MPP_VIDEO_CodingAVC;
        case rkvc::Codec::Hevc:
            return MPP_VIDEO_CodingHEVC;
        default:
            return MPP_VIDEO_CodingUnused;
    }
}

}  // namespace

struct MppEncoderNode::Impl {
    rkvc::Request req;
    MppCtx ctx = nullptr;
    MppApi* mpi = nullptr;
    MppCodingType coding = MPP_VIDEO_CodingUnused;
    MppBufferGroup group = nullptr;
    // Caller-owned encode output buffer. MPP falls back to an internal buffer
    // of only width * height bytes when the caller supplies no output packet,
    // which an incompressible frame can exceed.
    MppBuffer out_buf = nullptr;
    MppEncCfg cfg = nullptr;
    MppEncROICfg roi_cfg = {};
    MppEncROIRegion roi_regions[rkvc::kRoiMaxRegions] = {};
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t hor_stride = 0;
    uint32_t ver_stride = 0;
    MppFrameFormat format = MPP_FMT_YUV420SP;
    int32_t applied_bps = 0;
    uint32_t applied_gop = 0;
    bool fixqp = false;
    rkvc::Emit* emit = nullptr;
    rkvc::Spec in_spec;
};

MppEncoderNode::MppEncoderNode(rkvc::Request req)
    : impl_(new(std::nothrow) Impl()) {
    if (impl_)
        impl_->req = std::move(req);
}

// Qualified: cleanup must not depend on virtual dispatch from a destructor.
MppEncoderNode::~MppEncoderNode() {
    MppEncoderNode::close();
}

std::vector<rkvc::Port> MppEncoderNode::make_ports() const {
    rkvc::Port in, out;
    in.name = "video";
    in.is_input = true;
    out.name = "bitstream";
    out.desired.fmt = rkvc::PixelFormat::Bitstream;
    out.desired.domain = rkvc::MemDomain::Host;
    return {in, out};
}

rkvc::Status MppEncoderNode::configure(std::vector<rkvc::Port>& ports,
                                       rkvc::Diag* diag) {
    rkvc::Status st = rkvc::Node::configure(ports, diag);
    if (st != rkvc::Status::Ok)
        return st;
    Impl* enc = impl_.get();
    if (!enc)
        return rkvc::Status::Nomem;
    for (const auto& p : ports) {
        if (p.is_input) {
            if (p.resolved.fmt != rkvc::PixelFormat::Nv12 &&
                p.resolved.fmt != rkvc::PixelFormat::Yuv420P) {
                if (diag)
                    diag->add("configure", "mpp.encode",
                              "NV12/YUV420P input only");
                return rkvc::Status::Negotiate;
            }
            enc->in_spec = p.resolved;
        }
    }
    return rkvc::Status::Ok;
}

namespace {

// Full hardware init from a concrete input spec. No-op success when the
// geometry is still unknown (deferred to the first frame).
rkvc::Status init_from_spec(MppEncoderNode::Impl* enc, const rkvc::Spec& in,
                            rkvc::Diag* diag) {
    enc->coding = to_mpp_coding(enc->req.codec);
    if (enc->coding != MPP_VIDEO_CodingAVC &&
        enc->coding != MPP_VIDEO_CodingHEVC)
        return rkvc::Status::Format;
    enc->width = in.width ? in.width : enc->req.width;
    enc->height = in.height ? in.height : enc->req.height;
    if (!enc->width || !enc->height)
        return rkvc::Status::Ok;  // TRANSCODE: wait for decoded frames
    enc->hor_stride = in.stride ? in.stride : enc->width;
    enc->ver_stride = in.ver_stride ? in.ver_stride : enc->height;
    enc->format = (in.fmt == rkvc::PixelFormat::Nv12) ? MPP_FMT_YUV420SP
                                                      : MPP_FMT_YUV420P;

    RK_S64 in_timeout = 5000;
    // Output is drained up to the frame's EOI packet right after every
    // put_frame, so get_packet must wait for that frame's output. The value
    // only caps a stalled encoder.
    RK_S64 out_timeout = 5000;
    MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
    if (mpp_create(&enc->ctx, &enc->mpi) != MPP_OK)
        return rkvc::Status::Hw;
    MppEncCfg cfg = nullptr;
    if (mpp_enc_cfg_init(&cfg) != MPP_OK) {
        mpp_destroy(enc->ctx);
        enc->ctx = nullptr;
        enc->mpi = nullptr;
        return rkvc::Status::Hw;
    }
    int64_t bps = 0;
    mpp_enc_cfg_set_s32(cfg, "prep:width", enc->width);
    mpp_enc_cfg_set_s32(cfg, "prep:height", enc->height);
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", enc->hor_stride);
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", enc->ver_stride);
    mpp_enc_cfg_set_s32(cfg, "prep:format", enc->format);
    mpp_enc_cfg_set_s32(cfg, "codec:type", enc->coding);
    int32_t fps =
        enc->req.quality.fps ? (int32_t)enc->req.quality.fps : kDefaultFps;
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denom", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denom", 1);
    mpp_enc_cfg_set_s32(
        cfg, "rc:gop",
        enc->req.quality.gop_size ? enc->req.quality.gop_size : kDefaultGop);
    if (enc->req.quality.qp >= 0) {
        int32_t qp = enc->req.quality.qp;
        mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_FIXQP);
        mpp_enc_cfg_set_s32(cfg, "rc:qp_init", qp);
        mpp_enc_cfg_set_s32(cfg, "rc:qp_min", qp);
        mpp_enc_cfg_set_s32(cfg, "rc:qp_max", qp);
        mpp_enc_cfg_set_s32(cfg, "rc:qp_min_i", qp);
        mpp_enc_cfg_set_s32(cfg, "rc:qp_max_i", qp);
        mpp_enc_cfg_set_s32(cfg, "rc:fqp_min_i", qp);
        mpp_enc_cfg_set_s32(cfg, "rc:fqp_max_i", qp);
        mpp_enc_cfg_set_s32(cfg, "rc:fqp_min_p", qp);
        mpp_enc_cfg_set_s32(cfg, "rc:fqp_max_p", qp);
        enc->fixqp = true;
    } else {
        bps = enc->req.quality.bitrate_bps > 0
                  ? enc->req.quality.bitrate_bps
                  : (int64_t)enc->width * enc->height * 3;
        mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
        mpp_enc_cfg_set_s32(cfg, "rc:bps_target", (RK_S32)bps);
        mpp_enc_cfg_set_s32(cfg, "rc:bps_max", (RK_S32)(bps * 17 / 16));
        mpp_enc_cfg_set_s32(cfg, "rc:bps_min", (RK_S32)(bps * 15 / 16));
        enc->applied_bps = (int32_t)bps;
    }
    enc->applied_gop =
        enc->req.quality.gop_size ? enc->req.quality.gop_size : kDefaultGop;
    auto fail = [&] {
        if (diag)
            diag->add("open", "mpp.encode", "mpp encoder init failed");
        mpp_enc_cfg_deinit(cfg);
        mpp_destroy(enc->ctx);
        enc->ctx = nullptr;
        enc->mpi = nullptr;
        return rkvc::Status::Hw;
    };
    if (mpp_init(enc->ctx, MPP_CTX_ENC, enc->coding) != MPP_OK)
        return fail();
    if (enc->mpi->control(enc->ctx, MPP_ENC_SET_CFG, cfg) != MPP_OK)
        return fail();
    if (enc->mpi->control(enc->ctx, MPP_ENC_SET_HEADER_MODE, &header_mode) !=
        MPP_OK)
        return fail();
    if (enc->mpi->control(enc->ctx, MPP_SET_INPUT_TIMEOUT, &in_timeout) !=
        MPP_OK)
        return fail();
    if (enc->mpi->control(enc->ctx, MPP_SET_OUTPUT_TIMEOUT, &out_timeout) !=
        MPP_OK)
        return fail();
    enc->cfg = cfg;
    if (mpp_buffer_group_get_internal(&enc->group, MPP_BUFFER_TYPE_DMA_HEAP) !=
            MPP_OK &&
        mpp_buffer_group_get_internal(&enc->group, MPP_BUFFER_TYPE_ION) !=
            MPP_OK) {
        if (diag)
            diag->add("open", "mpp.encode", "buffer group alloc failed");
        mpp_enc_cfg_deinit(cfg);
        enc->cfg = nullptr;
        mpp_destroy(enc->ctx);
        enc->ctx = nullptr;
        enc->mpi = nullptr;
        return rkvc::Status::Hw;
    }
    // MPP's own output packet buffer is only width * height bytes (see
    // mpp_enc_check_pkt_buf), yet a high-entropy frame compresses to nearly its
    // raw size. Without a caller buffer the encoder overruns that allocation
    // and reports a packet whose position/length run past the end of the
    // dmabuf, so copying the packet reads out of bounds. Hand it a full frame
    // sized buffer like the MPP test encoders do.
    uint32_t out_w = (enc->hor_stride + 63u) & ~63u;
    uint32_t out_h = (enc->ver_stride + 63u) & ~63u;
    size_t out_size = (size_t)out_w * out_h * 3 / 2;
    if (mpp_buffer_get(enc->group, &enc->out_buf, out_size) != MPP_OK) {
        if (diag)
            diag->add("open", "mpp.encode", "output buffer alloc failed");
        mpp_enc_cfg_deinit(cfg);
        enc->cfg = nullptr;
        mpp_destroy(enc->ctx);
        enc->ctx = nullptr;
        enc->mpi = nullptr;
        return rkvc::Status::Hw;
    }
    return rkvc::Status::Ok;
}

rkvc::Status apply_control(MppEncoderNode::Impl* enc,
                           const rkvc::EncodeControl& c) {
    bool changed = false;
    if (c.gop_size && c.gop_size != enc->applied_gop) {
        mpp_enc_cfg_set_s32(enc->cfg, "rc:gop", c.gop_size);
        changed = true;
    }
    if (!enc->fixqp && c.bitrate_bps > 0 && c.bitrate_bps != enc->applied_bps) {
        int64_t bps = c.bitrate_bps;
        mpp_enc_cfg_set_s32(enc->cfg, "rc:bps_target", (RK_S32)bps);
        mpp_enc_cfg_set_s32(enc->cfg, "rc:bps_max", (RK_S32)(bps * 17 / 16));
        mpp_enc_cfg_set_s32(enc->cfg, "rc:bps_min", (RK_S32)(bps * 15 / 16));
        changed = true;
    }
    if (changed &&
        enc->mpi->control(enc->ctx, MPP_ENC_SET_CFG, enc->cfg) != MPP_OK)
        return rkvc::Status::Hw;
    if (c.force_idr &&
        enc->mpi->control(enc->ctx, MPP_ENC_SET_IDR_FRAME, nullptr) != MPP_OK)
        return rkvc::Status::Hw;
    if (c.gop_size)
        enc->applied_gop = c.gop_size;
    if (!enc->fixqp && c.bitrate_bps > 0)
        enc->applied_bps = c.bitrate_bps;
    return rkvc::Status::Ok;
}

rkvc::Status attach_roi(MppEncoderNode::Impl* enc, MppFrame frame,
                        const rkvc::FramePtr& input) {
    if (input->roi_count() == 0)
        return rkvc::Status::Ok;
    auto clamped = detail::clamp_roi(input->roi_regions(), input->roi_count(),
                                     enc->width, enc->height);
    if (!clamped)
        return clamped.status();
    memset(enc->roi_regions, 0, sizeof(enc->roi_regions));
    memset(&enc->roi_cfg, 0, sizeof(enc->roi_cfg));
    for (size_t i = 0; i < clamped.value().size(); ++i) {
        const detail::ClampedRoi& src = clamped.value()[i];
        MppEncROIRegion& dst = enc->roi_regions[i];
        dst.x = src.x;
        dst.y = src.y;
        dst.w = src.w;
        dst.h = src.h;
        dst.intra = src.force_intra ? 1 : 0;
        dst.quality = src.qp_delta;
        dst.qp_area_idx = 0;
        dst.area_map_en = 1;
        dst.abs_qp_en = 0;
    }
    enc->roi_cfg.number = (RK_U32)clamped.value().size();
    enc->roi_cfg.regions = enc->roi_regions;
    MppMeta meta = mpp_frame_get_meta(frame);
    if (!meta || mpp_meta_set_ptr(meta, KEY_ROI_DATA, &enc->roi_cfg) != MPP_OK)
        return rkvc::Status::Hw;
    return rkvc::Status::Ok;
}

// Attaches the caller-owned output packet to the frame. MPP encodes into that
// packet's buffer and returns the very same packet from encode_get_packet.
rkvc::Status attach_output_packet(MppEncoderNode::Impl* enc, MppFrame frame) {
    if (!enc->out_buf)
        return rkvc::Status::Hw;
    MppPacket packet = nullptr;
    if (mpp_packet_init_with_buffer(&packet, enc->out_buf) != MPP_OK)
        return rkvc::Status::Hw;
    // NOTE: the reused packet must be reported as empty for the new frame.
    mpp_packet_set_length(packet, 0);
    MppMeta meta = mpp_frame_get_meta(frame);
    if (!meta ||
        mpp_meta_set_packet(meta, KEY_OUTPUT_PACKET, packet) != MPP_OK) {
        mpp_packet_deinit(&packet);
        return rkvc::Status::Hw;
    }
    return rkvc::Status::Ok;
}

// HOST input copied plane-by-plane into an MPP buffer.
MppBuffer host_copy(MppEncoderNode::Impl* enc, const uint8_t* src,
                    uint32_t src_row, uint32_t src_ver) {
    size_t need = (size_t)enc->hor_stride * enc->ver_stride * 3 / 2;
    MppBuffer buf = nullptr;
    if (mpp_buffer_get(enc->group, &buf, need) != MPP_OK || !buf)
        return nullptr;
    uint8_t* dst = static_cast<uint8_t*>(mpp_buffer_get_ptr(buf));
    for (uint32_t y = 0; y < enc->height; ++y)
        memcpy(dst + (size_t)y * enc->hor_stride, src + (size_t)y * src_row,
               enc->width);
    dst += (size_t)enc->hor_stride * enc->ver_stride;
    src += (size_t)src_row * src_ver;
    for (uint32_t y = 0; y < enc->height / 2; ++y)
        memcpy(dst + (size_t)y * enc->hor_stride, src + (size_t)y * src_row,
               enc->width);
    return buf;
}

MppBuffer dmabuf_wrap(MppEncoderNode::Impl* enc, const rkvc::FramePtr& input) {
    MppBufferInfo info = {};
    info.type = MPP_BUFFER_TYPE_DMA_HEAP;
    info.size = input->size();
    info.fd = input->fd();
    MppBuffer buf = nullptr;
    if (mpp_buffer_import(&buf, &info) == MPP_OK && buf)
        return buf;
#ifdef __linux__
    void* mapped =
        mmap(nullptr, input->size(), PROT_READ, MAP_SHARED, input->fd(), 0);
    if (mapped != MAP_FAILED) {
        const rkvc::Spec& spec = input->spec();
        uint32_t src_row = spec.stride ? spec.stride : spec.width;
        uint32_t src_ver = spec.ver_stride ? spec.ver_stride : spec.height;
        buf = host_copy(enc, static_cast<const uint8_t*>(mapped), src_row,
                        src_ver);
        munmap(mapped, input->size());
    }
#endif
    return buf;
}

rkvc::Status emit_packet(rkvc::Emit* emit, MppPacket packet) {
    size_t len = mpp_packet_get_length(packet);
    if (!len)
        return rkvc::Status::Ok;
    void* copy = malloc(len);
    if (!copy)
        return rkvc::Status::Nomem;
    memcpy(copy, mpp_packet_get_pos(packet), len);
    rkvc::Spec spec;
    spec.fmt = rkvc::PixelFormat::Bitstream;
    auto fr = rkvc::Frame::borrow_host(spec, copy, len, {free_packet, copy});
    if (!fr) {
        free(copy);
        return fr.status();
    }
    fr.value()->set_pts(mpp_packet_get_pts(packet));
    fr.value()->set_dts(mpp_packet_get_dts(packet));
    return emit->emit(0, fr.value());
}

// Drains the output of the frame just submitted, up to and including the
// packet flagged EOI. A packet that is not part of a partition sequence is a
// complete frame on its own.
rkvc::Status drain_frame_packets(MppEncoderNode::Impl* enc) {
    for (;;) {
        MppPacket packet = nullptr;
        MPP_RET ret = enc->mpi->encode_get_packet(enc->ctx, &packet);
        if (ret != MPP_OK || !packet)
            return rkvc::Status::Ok;
        bool last =
            !mpp_packet_is_partition(packet) || mpp_packet_is_eoi(packet);
        rkvc::Status st = emit_packet(enc->emit, packet);
        mpp_packet_deinit(&packet);
        if (st != rkvc::Status::Ok || last)
            return st;
    }
}

// Drain ready packets; until_eos must observe the EOS packet.
rkvc::Status drain_packets(MppEncoderNode::Impl* enc, bool until_eos) {
    for (;;) {
        MppPacket packet = nullptr;
        MPP_RET ret = enc->mpi->encode_get_packet(enc->ctx, &packet);
        if (ret != MPP_OK) {
            // Legacy async API reports an empty non-blocking list as
            // MPP_NOK instead of OK + NULL packet.
            if (ret == MPP_NOK && !until_eos)
                return rkvc::Status::Ok;
            return rkvc::Status::Hw;
        }
        if (!packet)
            return until_eos ? rkvc::Status::Hw : rkvc::Status::Ok;
        bool eos = !!mpp_packet_get_eos(packet);
        rkvc::Status st = emit_packet(enc->emit, packet);
        mpp_packet_deinit(&packet);
        if (st != rkvc::Status::Ok)
            return st;
        if (eos || !until_eos)
            return rkvc::Status::Ok;
    }
}

}  // namespace

rkvc::Status MppEncoderNode::open(rkvc::Emit* emit, rkvc::Diag* diag) {
    Impl* enc = impl_.get();
    if (!enc)
        return rkvc::Status::Nomem;
    if (!emit) {
        if (diag)
            diag->add("open", "mpp.encode", "null emit");
        return rkvc::Status::Invalid;
    }
    enc->emit = emit;
    return init_from_spec(enc, enc->in_spec, diag);
}

rkvc::Status MppEncoderNode::process(rkvc::FramePtr input, rkvc::Diag* diag) {
    Impl* enc = impl_.get();
    if (!enc)
        return rkvc::Status::Nomem;
    if (!input)
        return rkvc::Status::Format;
    const rkvc::Spec& spec = input->spec();
    if (spec.fmt != rkvc::PixelFormat::Nv12 &&
        spec.fmt != rkvc::PixelFormat::Yuv420P)
        return rkvc::Status::Format;
    if (!enc->ctx) {
        if (!spec.width || !spec.height)
            return rkvc::Status::Format;
        enc->in_spec = spec;
        rkvc::Status st = init_from_spec(enc, spec, diag);
        if (st != rkvc::Status::Ok)
            return st;
        if (!enc->ctx)
            return rkvc::Status::Format;  // still no geometry
    }
    if ((spec.width && spec.width != enc->width) ||
        (spec.height && spec.height != enc->height))
        return rkvc::Status::Format;

    rkvc::Status st = apply_control(enc, input->encode());
    if (st != rkvc::Status::Ok) {
        if (diag)
            diag->add("process", "mpp.encode", "runtime control failed");
        return st;
    }

    MppBuffer buf = nullptr;
    if (spec.domain == rkvc::MemDomain::Dmabuf) {
        if (input->fd() < 0 || !input->size())
            return rkvc::Status::Format;
        buf = dmabuf_wrap(enc, input);
    } else {
        if (!input->data())
            return rkvc::Status::Format;
        uint32_t src_row = spec.stride ? spec.stride : spec.width;
        uint32_t src_ver = spec.ver_stride ? spec.ver_stride : spec.height;
        buf = host_copy(enc, static_cast<const uint8_t*>(input->data()),
                        src_row, src_ver);
    }
    if (!buf)
        return rkvc::Status::Nomem;

    MppFrame frame = nullptr;
    if (mpp_frame_init(&frame) != MPP_OK) {
        mpp_buffer_put(buf);
        return rkvc::Status::Hw;
    }
    mpp_frame_set_width(frame, enc->width);
    mpp_frame_set_height(frame, enc->height);
    mpp_frame_set_hor_stride(frame, enc->hor_stride);
    mpp_frame_set_ver_stride(frame, enc->ver_stride);
    mpp_frame_set_fmt(frame, enc->format);
    mpp_frame_set_buffer(frame, buf);
    mpp_frame_set_pts(frame, input->pts());
    rkvc::Status rc = attach_output_packet(enc, frame);
    if (rc == rkvc::Status::Ok)
        rc = attach_roi(enc, frame, input);
    if (rc == rkvc::Status::Ok &&
        enc->mpi->encode_put_frame(enc->ctx, frame) != MPP_OK)
        rc = rkvc::Status::Hw;
    mpp_frame_deinit(&frame);
    if (rc != rkvc::Status::Ok) {
        mpp_buffer_put(buf);
        return rc;
    }
    rc = drain_frame_packets(enc);
    mpp_buffer_put(buf);
    return rc;
}

rkvc::Status MppEncoderNode::flush(rkvc::Diag* /*diag*/) {
    Impl* enc = impl_.get();
    if (!enc)
        return rkvc::Status::Nomem;
    if (!enc->ctx)
        return rkvc::Status::Ok;
    MppFrame frame = nullptr;
    if (mpp_frame_init(&frame) != MPP_OK)
        return rkvc::Status::Hw;
    mpp_frame_set_width(frame, enc->width);
    mpp_frame_set_height(frame, enc->height);
    mpp_frame_set_hor_stride(frame, enc->hor_stride);
    mpp_frame_set_ver_stride(frame, enc->ver_stride);
    mpp_frame_set_fmt(frame, enc->format);
    mpp_frame_set_eos(frame, 1);
    rkvc::Status rc = attach_output_packet(enc, frame);
    if (rc == rkvc::Status::Ok &&
        enc->mpi->encode_put_frame(enc->ctx, frame) != MPP_OK)
        rc = rkvc::Status::Hw;
    mpp_frame_deinit(&frame);
    if (rc != rkvc::Status::Ok)
        return rc;
    return drain_packets(enc, true);
}

void MppEncoderNode::close() noexcept {
    Impl* enc = impl_.get();
    if (!enc)
        return;
    if (enc->out_buf) {
        mpp_buffer_put(enc->out_buf);
        enc->out_buf = nullptr;
    }
    if (enc->cfg) {
        mpp_enc_cfg_deinit(enc->cfg);
        enc->cfg = nullptr;
    }
    if (enc->ctx) {
        mpp_destroy(enc->ctx);
        enc->ctx = nullptr;
        enc->mpi = nullptr;
    }
    if (enc->group) {
        mpp_buffer_group_put(enc->group);
        enc->group = nullptr;
    }
}

bool MppEncodeFactory::matches(const rkvc::Request& r,
                               const rkvc::DeviceCaps&) const noexcept {
    if (!detail::mpp_device_present())
        return false;
    if (r.operation != rkvc::Operation::Encode &&
        r.operation != rkvc::Operation::Transcode)
        return false;
    return r.codec == rkvc::Codec::H264 || r.codec == rkvc::Codec::Hevc;
}

int MppEncodeFactory::score(const rkvc::Request& r,
                            const rkvc::DeviceCaps&) const noexcept {
    return r.policy == rkvc::Policy::Realtime ? 100 : 50;
}

rkvc::Result<rkvc::NodePtr> MppEncodeFactory::create(const rkvc::Request& r,
                                                     rkvc::Diag*) const {
    rkvc::NodePtr n(new (std::nothrow) MppEncoderNode(r));
    if (!n)
        return rkvc::Result<rkvc::NodePtr>::failure(rkvc::Status::Nomem);
    return rkvc::Result<rkvc::NodePtr>::success(std::move(n));
}

}  // namespace h264h265
