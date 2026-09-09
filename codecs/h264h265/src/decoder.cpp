// SPDX-License-Identifier: AGPL-3.0-or-later
#include "h264h265/decoder.hpp"

#include <ctime>
#include <new>
#include <unistd.h>
#include <utility>

#include "h264h265/detail.hpp"
#include "rk_mpi.h"

namespace h264h265 {

namespace {

rkvc::PixelFormat mpp_to_pixel(MppFrameFormat f) noexcept {
    switch (f & MPP_FRAME_FMT_MASK) {
        case MPP_FMT_YUV420SP:
            return rkvc::PixelFormat::Nv12;
        case MPP_FMT_YUV420SP_10BIT:
            return rkvc::PixelFormat::P010;
        default:
            return rkvc::PixelFormat::Unknown;
    }
}

MppCodingType to_mpp_coding(rkvc::Codec c) noexcept {
    switch (c) {
        case rkvc::Codec::H264:
            return MPP_VIDEO_CodingAVC;
        case rkvc::Codec::Hevc:
            return MPP_VIDEO_CodingHEVC;
        case rkvc::Codec::Av1:
            return MPP_VIDEO_CodingAV1;
        default:
            return MPP_VIDEO_CodingUnused;
    }
}

void close_fd(void* p) noexcept {
    int fd = static_cast<int>(reinterpret_cast<intptr_t>(p));
    if (fd >= 0)
        close(fd);
}

}  // namespace

struct MppDecoderNode::Impl {
    rkvc::Request req;
    MppCtx ctx = nullptr;
    MppApi* mpi = nullptr;
    MppCodingType coding = MPP_VIDEO_CodingUnused;
    bool initialized = false;
    uint64_t emitted = 0;
    rkvc::Emit* emit = nullptr;
};

MppDecoderNode::MppDecoderNode(rkvc::Request req)
    : impl_(new (std::nothrow) Impl()) {
    if (impl_)
        impl_->req = std::move(req);
}

MppDecoderNode::~MppDecoderNode() { close(); }

std::vector<rkvc::Port> MppDecoderNode::make_ports() const {
    rkvc::Port in, out;
    in.name = "bitstream";
    in.is_input = true;
    in.desired.fmt = rkvc::PixelFormat::Bitstream;
    in.desired.domain = rkvc::MemDomain::Host;
    out.name = "video";
    out.desired.fmt = rkvc::PixelFormat::Nv12;
    out.desired.domain = rkvc::MemDomain::Dmabuf;
    if (impl_) {
        out.desired.width = impl_->req.width;
        out.desired.height = impl_->req.height;
    }
    return {in, out};
}

rkvc::Status MppDecoderNode::configure(std::vector<rkvc::Port>& ports,
                                       rkvc::Diag* diag) {
    for (const auto& p : ports) {
        if (p.is_input && p.resolved.fmt != rkvc::PixelFormat::Bitstream) {
            if (diag)
                diag->add("configure", "mpp.decode", "bitstream input only");
            return rkvc::Status::Negotiate;
        }
    }
    return rkvc::Node::configure(ports, diag);
}

namespace {

rkvc::Status decoder_init_locked(MppDecoderNode::Impl* dec) {
    RK_S64 in_timeout = MPP_TIMEOUT_NON_BLOCK;
    RK_S64 out_timeout = MPP_TIMEOUT_NON_BLOCK;
    MppFrameFormat out_fmt = MPP_FMT_YUV420SP;
    if (mpp_init(dec->ctx, MPP_CTX_DEC, dec->coding) != MPP_OK)
        return rkvc::Status::Hw;
    if (dec->mpi->control(dec->ctx, MPP_SET_INPUT_TIMEOUT, &in_timeout) !=
        MPP_OK)
        return rkvc::Status::Hw;
    if (dec->mpi->control(dec->ctx, MPP_SET_OUTPUT_TIMEOUT, &out_timeout) !=
        MPP_OK)
        return rkvc::Status::Hw;
    if (dec->mpi->control(dec->ctx, MPP_DEC_SET_OUTPUT_FORMAT, &out_fmt) !=
        MPP_OK)
        return rkvc::Status::Hw;
    dec->initialized = true;
    return rkvc::Status::Ok;
}

rkvc::Status emit_mpp_frame(rkvc::Emit* emit, MppFrame frame) {
    MppBuffer buffer = mpp_frame_get_buffer(frame);
    MppFrameFormat format = mpp_frame_get_fmt(frame);
    // Compressed/tiled layouts need an explicit DRM modifier contract;
    // only linear frames are exposed.
    if (!buffer || MPP_FRAME_FMT_IS_FBC(format) ||
        MPP_FRAME_FMT_IS_TILE(format))
        return rkvc::Status::Unsupported;
    rkvc::Spec spec;
    spec.width = mpp_frame_get_width(frame);
    spec.height = mpp_frame_get_height(frame);
    spec.fmt = mpp_to_pixel(format);
    spec.domain = rkvc::MemDomain::Dmabuf;
    spec.stride = mpp_frame_get_hor_stride(frame);
    spec.ver_stride = mpp_frame_get_ver_stride(frame);
    if (spec.fmt == rkvc::PixelFormat::Unknown)
        return rkvc::Status::Format;
    int fd = mpp_buffer_get_fd(buffer);
    if (fd < 0)
        return rkvc::Status::Format;
    // dup: the MPP frame (and its fd) dies at deinit, the dup outlives it.
    int owned = dup(fd);
    if (owned < 0)
        return rkvc::Status::Io;
    auto fr = rkvc::Frame::borrow_dmabuf(
        spec, owned, mpp_buffer_get_size(buffer),
        {close_fd, reinterpret_cast<void*>(static_cast<intptr_t>(owned))});
    if (!fr) {
        close(owned);
        return fr.status();
    }
    if (mpp_frame_get_errinfo(frame) || mpp_frame_get_discard(frame))
        fr.value()->set_flags(rkvc::kFlagCorrupt);
    fr.value()->set_pts(mpp_frame_get_pts(frame));
    fr.value()->set_dts(mpp_frame_get_dts(frame));
    return emit->emit(0, fr.value());
}

// Drain decoded frames; until_eos must observe the EOS frame.
rkvc::Status drain_decoder(MppDecoderNode::Impl* dec, bool until_eos) {
    for (;;) {
        MppFrame frame = nullptr;
        if (dec->mpi->decode_get_frame(dec->ctx, &frame) != MPP_OK)
            return rkvc::Status::Hw;
        if (!frame)
            return until_eos ? rkvc::Status::Hw : rkvc::Status::Ok;
        if (mpp_frame_get_info_change(frame)) {
            rkvc::Status st =
                dec->mpi->control(dec->ctx, MPP_DEC_SET_INFO_CHANGE_READY,
                                  nullptr) == MPP_OK
                    ? rkvc::Status::Ok
                    : rkvc::Status::Hw;
            mpp_frame_deinit(&frame);
            if (st != rkvc::Status::Ok)
                return st;
            continue;
        }
        bool eos = !!mpp_frame_get_eos(frame);
        rkvc::Status st = rkvc::Status::Ok;
        if (mpp_frame_get_buffer(frame)) {
            st = emit_mpp_frame(dec->emit, frame);
            if (st == rkvc::Status::Ok)
                ++dec->emitted;
        }
        mpp_frame_deinit(&frame);
        if (st != rkvc::Status::Ok)
            return st;
        if (eos)
            return rkvc::Status::Ok;
    }
}

}  // namespace

rkvc::Status MppDecoderNode::open(rkvc::Emit* emit, rkvc::Diag* diag) {
    Impl* dec = impl_.get();
    if (!dec)
        return rkvc::Status::Nomem;
    if (!emit) {
        if (diag)
            diag->add("open", "mpp.decode", "null emit");
        return rkvc::Status::Invalid;
    }
    if (dec->req.operation == rkvc::Operation::Decode)
        dec->coding = to_mpp_coding(dec->req.codec);
    RK_U32 split = 1;
    if (mpp_create(&dec->ctx, &dec->mpi) != MPP_OK)
        return rkvc::Status::Hw;
    if (dec->mpi->control(dec->ctx, MPP_DEC_SET_PARSER_SPLIT_MODE, &split) !=
        MPP_OK) {
        close();
        return rkvc::Status::Hw;
    }
    dec->emit = emit;
    if (dec->coding != MPP_VIDEO_CodingUnused) {
        rkvc::Status st = decoder_init_locked(dec);
        if (st != rkvc::Status::Ok) {
            close();
            return st;
        }
    }
    return rkvc::Status::Ok;
}

rkvc::Status MppDecoderNode::process(rkvc::FramePtr input, rkvc::Diag* diag) {
    Impl* dec = impl_.get();
    if (!dec)
        return rkvc::Status::Nomem;
    if (!input || input->spec().fmt != rkvc::PixelFormat::Bitstream ||
        !input->data() || !input->size())
        return rkvc::Status::Format;
    if (!dec->initialized) {
        const uint8_t* data = static_cast<const uint8_t*>(input->data());
        detail::SniffedCodec sniffed =
            detail::sniff_annexb(data, input->size());
        if (sniffed == detail::SniffedCodec::Unknown) {
            if (diag)
                diag->add("process", "mpp.decode",
                          "cannot sniff codec (pass explicit codec for "
                          "AV1/non-Annex-B streams)");
            return rkvc::Status::Format;
        }
        dec->coding = (sniffed == detail::SniffedCodec::Avc)
                          ? MPP_VIDEO_CodingAVC
                          : MPP_VIDEO_CodingHEVC;
        rkvc::Status st = decoder_init_locked(dec);
        if (st != rkvc::Status::Ok)
            return st;
    }

    MppPacket packet = nullptr;
    void* payload = const_cast<void*>(input->data());
    if (mpp_packet_init(&packet, payload, input->size()) != MPP_OK)
        return rkvc::Status::Nomem;
    mpp_packet_set_pts(packet, input->pts());
    mpp_packet_set_dts(packet, input->dts());
    struct timespec deadline = {};
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += 10;
    rkvc::Status out = rkvc::Status::Ok;
    for (;;) {
        MPP_RET ret = dec->mpi->decode_put_packet(dec->ctx, packet);
        if (ret == MPP_OK)
            break;
        if (ret != MPP_ERR_BUFFER_FULL) {
            out = rkvc::Status::Hw;
            break;
        }
        // Input side full: drain decoded frames first to make progress.
        mpp_packet_deinit(&packet);
        if (mpp_packet_init(&packet, payload, input->size()) != MPP_OK) {
            out = rkvc::Status::Nomem;
            packet = nullptr;
            break;
        }
        mpp_packet_set_pts(packet, input->pts());
        mpp_packet_set_dts(packet, input->dts());
        uint64_t before = dec->emitted;
        rkvc::Status ds = drain_decoder(dec, false);
        if (ds != rkvc::Status::Ok) {
            out = ds;
            break;
        }
        if (dec->emitted != before) {
            clock_gettime(CLOCK_MONOTONIC, &deadline);
            deadline.tv_sec += 10;
        } else {
            struct timespec now = {};
            struct timespec nap = {0, 1000000};
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec > deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec &&
                 now.tv_nsec >= deadline.tv_nsec)) {
                if (diag)
                    diag->add("process", "mpp.decode",
                              "decoder input stays full; hw stall?");
                out = rkvc::Status::Hw;
                break;
            }
            nanosleep(&nap, nullptr);
        }
    }
    if (packet)
        mpp_packet_deinit(&packet);
    if (out != rkvc::Status::Ok)
        return out;
    return drain_decoder(dec, false);
}

rkvc::Status MppDecoderNode::flush(rkvc::Diag* /*diag*/) {
    Impl* dec = impl_.get();
    if (!dec)
        return rkvc::Status::Nomem;
    if (!dec->initialized)
        return rkvc::Status::Ok;
    RK_S64 out_timeout = 5000;
    MppPacket packet = nullptr;
    if (dec->mpi->control(dec->ctx, MPP_SET_OUTPUT_TIMEOUT, &out_timeout) !=
            MPP_OK ||
        mpp_packet_init(&packet, nullptr, 0) != MPP_OK)
        return rkvc::Status::Hw;
    mpp_packet_set_eos(packet);
    MPP_RET ret = dec->mpi->decode_put_packet(dec->ctx, packet);
    mpp_packet_deinit(&packet);
    if (ret != MPP_OK)
        return rkvc::Status::Hw;
    return drain_decoder(dec, true);
}

void MppDecoderNode::close() noexcept {
    Impl* dec = impl_.get();
    if (!dec)
        return;
    if (dec->ctx) {
        mpp_destroy(dec->ctx);
        dec->ctx = nullptr;
        dec->mpi = nullptr;
        dec->initialized = false;
    }
}

bool MppDecodeFactory::matches(const rkvc::Request& r,
                               const rkvc::DeviceCaps&) const noexcept {
    if (!detail::mpp_device_present())
        return false;
    if (r.operation == rkvc::Operation::Transcode)
        return true;
    if (r.operation != rkvc::Operation::Decode)
        return false;
    return r.codec == rkvc::Codec::Auto || r.codec == rkvc::Codec::H264 ||
           r.codec == rkvc::Codec::Hevc || r.codec == rkvc::Codec::Av1;
}

int MppDecodeFactory::score(const rkvc::Request& r,
                            const rkvc::DeviceCaps&) const noexcept {
    return r.policy == rkvc::Policy::Realtime ? 100 : 50;
}

rkvc::Result<rkvc::NodePtr> MppDecodeFactory::create(
    const rkvc::Request& r, rkvc::Diag*) const {
    rkvc::NodePtr n(new (std::nothrow) MppDecoderNode(r));
    if (!n)
        return rkvc::Result<rkvc::NodePtr>::failure(rkvc::Status::Nomem);
    return rkvc::Result<rkvc::NodePtr>::success(std::move(n));
}

bool mpp_device_present() noexcept {
    if (access("/dev/mpp_service", R_OK | W_OK) != 0 &&
        access("/dev/mpp-service", R_OK | W_OK) != 0)
        return false;
    return mpp_check_support_format(MPP_CTX_DEC, MPP_VIDEO_CodingAVC) ==
               MPP_OK ||
           mpp_check_support_format(MPP_CTX_ENC, MPP_VIDEO_CodingAVC) ==
               MPP_OK;
}

}  // namespace h264h265
