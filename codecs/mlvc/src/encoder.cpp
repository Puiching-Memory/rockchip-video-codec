// SPDX-License-Identifier: AGPL-3.0-or-later
#include "mlvc/codec.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>
#include <span>
#include <algorithm>
#include <utility>
#include <vector>

#include "mlvc/container.hpp"
#include "mlvc/pixel.hpp"

namespace mlvc {

namespace {

void free_buffer(void* p) noexcept { free(p); }

bool fp16_finite(uint16_t h) noexcept { return (h & 0x7c00) != 0x7c00; }

bool all_finite(std::span<const uint16_t> t) noexcept {
    for (uint16_t v : t)
        if (!fp16_finite(v))
            return false;
    return true;
}

}  // namespace

struct MlvcEncoderNode::Impl {
    rkvc::Request req;
    NpuModelFn make_model;
    RungSet rs;
    QpTables qptab;
    QRows qrows;
    RansCoder g_coder;
    RansCoder b_coder;
    EntropyConfig entropy;
    bool rans_ready = false;
    int qp = kDefaultQp;

    uint32_t img_w = 0;
    uint32_t img_h = 0;
    uint32_t ref_c = 0;
    uint32_t ref_h = 0;
    uint32_t ref_w = 0;
    uint32_t z_c = 0;
    uint32_t z_h = 0;
    uint32_t z_w = 0;
    uint32_t y_c = 0;
    uint32_t y_h = 0;
    uint32_t y_w = 0;
    int enc_x_in = -1;
    int enc_ref_in = -1;
    int enc_feat_out = -1;
    int enc_z_out = -1;
    int enc_y0_out = -1;
    int enc_y1_out = -1;

    std::vector<uint16_t> x_nhwc;
    std::vector<uint16_t> ref_prev;
    std::vector<uint16_t> ref_ltr;
    std::vector<uint16_t> ref_feed;  // NHWC transit for NPU input
    bool have_ltr = false;
    std::vector<int32_t> z_r, y0_r, y1_r, s0, s1, z_idx;
    int z_idx_qp = -1;

    uint32_t iframe_period = 0;
    uint32_t ltr_start_idx = 0;
    uint32_t ltr_period = 0;
    bool proactive_ltr = false;
    uint32_t cur_frame_idx = 0;
    uint32_t frame_count = 0;

    std::unique_ptr<RateController> rc;
    int64_t rc_bitrate = 0;
    rkvc::Emit* emit = nullptr;
};

MlvcEncoderNode::MlvcEncoderNode(rkvc::Request req, NpuModelFn make_model)
    : impl_(new (std::nothrow) Impl()) {
    if (impl_) {
        impl_->req = std::move(req);
        impl_->make_model = std::move(make_model);
    }
}

MlvcEncoderNode::~MlvcEncoderNode() = default;

std::vector<rkvc::Port> MlvcEncoderNode::make_ports() const {
    rkvc::Port in, out;
    in.name = "video";
    in.is_input = true;
    in.desired.fmt = rkvc::PixelFormat::Nv12;
    in.desired.domain = rkvc::MemDomain::Host;
    out.name = "bitstream";
    out.desired.fmt = rkvc::PixelFormat::Bitstream;
    out.desired.domain = rkvc::MemDomain::Host;
    return {in, out};
}

rkvc::Status MlvcEncoderNode::configure(std::vector<rkvc::Port>& ports,
                                        rkvc::Diag* diag) {
    return rkvc::Node::configure(ports, diag);
}

rkvc::Status MlvcEncoderNode::open(rkvc::Emit* emit, rkvc::Diag* diag) {
    Impl* e = impl_.get();
    if (!e)
        return rkvc::Status::Nomem;
    if (!emit) {
        if (diag)
            diag->add("open", "mlvc.encode", "null emit");
        return rkvc::Status::Invalid;
    }
    if (!e->rans_ready) {
        if (diag)
            diag->add("open", "mlvc.encode", "opened without a bound model");
        return rkvc::Status::Format;
    }
    e->iframe_period = e->req.quality.gop_size;
    e->ltr_start_idx = e->req.quality.ltr_start_idx;
    e->ltr_period = e->req.quality.ltr_period;
    e->proactive_ltr = e->ltr_period != 0;
    e->emit = emit;
    return rkvc::Status::Ok;
}

rkvc::Status MlvcEncoderNode::bind_model(const rkvc::Model& model,
                                         rkvc::Diag* diag) {
    Impl* e = impl_.get();
    if (!e)
        return rkvc::Status::Nomem;
    auto bad = [&](rkvc::Status s, const char* reason) {
        if (diag)
            diag->add("bind", "mlvc.encode", reason);
        return s;
    };
    if (model.meta.role != "encoder")
        return bad(rkvc::Status::Format, "model role is not encoder");
    e->qp = e->req.quality.qp >= 0 ? e->req.quality.qp : kDefaultQp;
    if (e->qp > 63)
        return bad(rkvc::Status::Invalid, "mlvc qp out of range");

    rkvc::Status st =
        init_rans_coders(model, e->g_coder, e->b_coder, e->entropy, diag);
    if (st != rkvc::Status::Ok)
        return bad(st, "PMF/rANS init failed");
    e->rans_ready = true;

    std::vector<int> rungs;
    st = collect_rungs(model, e->qp, rungs, diag);
    if (st != rkvc::Status::Ok)
        return bad(st, "session qp missing from rung set");
    st = init_rungs(model, rungs, e->make_model, e->rs, diag);
    if (st != rkvc::Status::Ok)
        return bad(st, "NPU init failed");
    st = setup_qrows(model, *e->rs.models[e->rs.active], e->qptab, e->qrows,
                     diag);
    if (st != rkvc::Status::Ok)
        return bad(st, "qptab setup failed");
    if (e->qrows.present && e->rs.rung_qp.size() != 1)
        return bad(rkvc::Status::Format, "qp-dynamic forbids qppatch rungs");

    // Geometry from the active rung's tensor contract (host fp16: inputs
    // NHWC, outputs logical NCHW).
    NpuModel& npu = *e->rs.models[e->rs.active];
    auto ins = npu.inputs();
    auto outs = npu.outputs();
    e->enc_x_in = find_input(ins, "x");
    if (e->enc_x_in < 0)
        e->enc_x_in = find_input(ins, "New_input_x");
    e->enc_ref_in = find_input(ins, "ref_feature");
    e->enc_feat_out = find_output(outs, "feature");
    e->enc_z_out = find_output(outs, "z_raw");
    e->enc_y0_out = find_output(outs, "y_raw_0");
    e->enc_y1_out = find_output(outs, "y_raw_1");
    if (e->enc_x_in < 0 || e->enc_ref_in < 0 || e->enc_feat_out < 0 ||
        e->enc_z_out < 0 || e->enc_y0_out < 0 || e->enc_y1_out < 0)
        return bad(rkvc::Status::Format, "tensor contract mismatch");
    const auto& xd = ins[e->enc_x_in].dims;
    const auto& rd = ins[e->enc_ref_in].dims;
    const auto& zd = outs[e->enc_z_out].dims;
    const auto& yd = outs[e->enc_y0_out].dims;
    const auto& fd = outs[e->enc_feat_out].dims;
    if (xd.size() != 4 || rd.size() != 4 || zd.size() != 4 ||
        yd.size() != 4 || fd.size() != 4)
        return bad(rkvc::Status::Format, "tensor rank mismatch");
    e->img_h = xd[1];
    e->img_w = xd[2];
    e->ref_h = rd[1];
    e->ref_w = rd[2];
    e->ref_c = rd[3];
    e->z_c = zd[1];
    e->z_h = zd[2];
    e->z_w = zd[3];
    e->y_c = yd[1];
    e->y_h = yd[2];
    e->y_w = yd[3];
    auto elems = [](const std::vector<uint32_t>& d) {
        return (size_t)d[1] * d[2] * d[3];
    };
    if (!e->img_w || !e->img_h || elems(fd) != elems(rd))
        return bad(rkvc::Status::Format, "tensor shape mismatch");
    st = resolve_entropy_geometry(e->entropy, e->qp, e->z_c, e->z_h, e->z_w,
                                  e->y_c, e->y_h, e->y_w, diag);
    if (st != rkvc::Status::Ok)
        return bad(st, "entropy geometry mismatch");

    size_t img_n = (size_t)e->img_h * e->img_w;
    size_t ref_n = (size_t)e->ref_c * e->ref_h * e->ref_w;
    size_t z_n = (size_t)e->z_c * e->z_h * e->z_w;
    size_t y_n = (size_t)e->y_c * e->y_h * e->y_w;
    e->x_nhwc.assign(img_n * 3, 0);
    e->ref_prev.assign(ref_n, 0);
    e->ref_ltr.assign(ref_n, 0);
    e->ref_feed.assign(ref_n, 0);
    e->z_r.assign(z_n, 0);
    e->y0_r.assign(y_n, 0);
    e->y1_r.assign(y_n, 0);
    e->s0.assign(y_n, 0);
    e->s1.assign(y_n, 0);
    e->z_idx.assign(z_n, 0);
    e->z_idx_qp = -1;
    return rkvc::Status::Ok;
}

rkvc::Status MlvcEncoderNode::process(rkvc::FramePtr input, rkvc::Diag* diag) {
    Impl* e = impl_.get();
    if (!e)
        return rkvc::Status::Nomem;
    auto bad = [&](rkvc::Status s, const char* reason) {
        if (diag)
            diag->add("process", "mlvc.encode", reason);
        return s;
    };
    if (!input || input->spec().fmt != rkvc::PixelFormat::Nv12 ||
        input->spec().domain != rkvc::MemDomain::Host || !input->data())
        return bad(rkvc::Status::Format, "NV12 host input only");
    if (input->spec().width != e->img_w ||
        input->spec().height != e->img_h)
        return bad(rkvc::Status::Format, "geometry mismatch");
    const uint8_t* base = static_cast<const uint8_t*>(input->data());

    if (input->encode().bitrate_bps > 0 &&
        input->encode().bitrate_bps != e->rc_bitrate) {
        if (!e->rc) {
            auto rc = RateController::create(
                e->img_w, e->img_h, (double)input->encode().bitrate_bps,
                kMlvcFps, diag);
            if (!rc)
                return bad(rc.status(), "rate controller init failed");
            e->rc = std::move(rc.value());
        } else {
            e->rc->configure((double)input->encode().bitrate_bps);
        }
        e->rc_bitrate = input->encode().bitrate_bps;
    }
    if (input->encode().gop_size > 0)
        e->iframe_period = input->encode().gop_size;

    uint32_t cur = e->cur_frame_idx;
    bool is_idr = e->frame_count == 0 || input->encode().force_idr ||
                  (e->iframe_period && cur > 0 && cur % e->iframe_period == 0);
    bool mark_ltr = false;
    bool is_recovery = false;
    if (!is_idr && e->ltr_period) {
        mark_ltr = cur == e->ltr_start_idx ||
                   (cur > e->ltr_start_idx && cur % e->ltr_period == 0);
        is_recovery = e->proactive_ltr && mark_ltr && e->have_ltr;
    }
    uint32_t rec_flags = (is_idr ? kRecKeyframe : 0) |
                         (mark_ltr ? kRecLtrMark : 0) |
                         (is_recovery ? kRecLtrRecovery : 0);
    RcFrameType rc_type = is_idr ? RcFrameType::I
                          : is_recovery ? RcFrameType::LtrRecovery
                                        : RcFrameType::P;

    int q_index = e->qp;
    if (e->rc) {
        double pt = (double)e->frame_count / kMlvcFps;
        int solved = e->rc->solve(pt, rc_type, kRecSize * 8);
        if (solved == kQDrop) {
            if (rc_type == RcFrameType::P && !mark_ltr) {
                uint8_t* record =
                    static_cast<uint8_t*>(malloc(kRecSize));
                if (!record)
                    return bad(rkvc::Status::Nomem, "no memory");
                write_record(record, 0, kQindexDropped, 0);
                rkvc::Spec spec;
                spec.fmt = rkvc::PixelFormat::Bitstream;
                auto fr = rkvc::Frame::borrow_host(
                    spec, record, kRecSize, {free_buffer, record});
                if (!fr) {
                    free(record);
                    return bad(fr.status(), "drop frame alloc failed");
                }
                fr.value()->set_pts(input->pts());
                fr.value()->set_dts(input->pts());
                rkvc::Status st = e->emit->emit(0, fr.value());
                e->frame_count++;
                e->cur_frame_idx = cur + 1;
                return st;
            }
            solved = 0;
        }
        q_index = solved;
    }

    int used_qp = q_index;
    if (!e->qrows.present) {
        e->rs.active = nearest_rung(e->rs.rung_qp, q_index);
        used_qp = e->rs.rung_qp[e->rs.active];
    }
    if (e->z_idx_qp != used_qp) {
        build_z_idx(e->z_idx, used_qp, e->z_c, e->z_h, e->z_w);
        e->z_idx_qp = used_qp;
    }
    NpuModel& npu = *e->rs.models[e->rs.active];
    if (is_idr) {
        std::fill(e->ref_prev.begin(), e->ref_prev.end(), 0);
        std::fill(e->ref_ltr.begin(), e->ref_ltr.end(), 0);
        e->have_ltr = false;
    }

    {
        uint32_t stride = input->spec().stride ? input->spec().stride
                                               : input->spec().width;
        uint32_t vstride = input->spec().ver_stride
                               ? input->spec().ver_stride
                               : input->spec().height;
        const uint8_t* uv = base + (size_t)stride * vstride;
        pixel::yuv_to_nhwc_fp16(base, stride, uv, uv + 1, stride, 1,
                                e->img_w, e->img_h, e->x_nhwc.data());
    }

    const std::vector<uint16_t>& ref =
        (is_recovery && e->have_ltr) ? e->ref_ltr : e->ref_prev;
    // Reference slot is NCHW; the NPU takes NHWC.
    pixel::nchw_f16_to_nhwc(ref.data(), e->ref_feed.data(), (int)e->ref_c,
                            (int)e->ref_h, (int)e->ref_w);
    rkvc::Status st = npu.set_input(e->enc_x_in, e->x_nhwc);
    if (st == rkvc::Status::Ok)
        st = npu.set_input(e->enc_ref_in, e->ref_feed);
    for (size_t i = 0; st == rkvc::Status::Ok && i < e->qrows.rows.size();
         ++i) {
        size_t idx = e->qrows.rows[i].first;
        const QpTable* t = e->qrows.rows[i].second;
        const uint16_t* row = qptab_row(*t, (uint32_t)used_qp);
        st = npu.set_input(idx, std::span<const uint16_t>(
                                    row, row ? t->cols : 0));
    }
    if (st == rkvc::Status::Ok)
        st = npu.run(diag);
    if (st != rkvc::Status::Ok)
        return bad(st, "NPU run failed");
    auto out_z = npu.output(e->enc_z_out);
    auto out_y0 = npu.output(e->enc_y0_out);
    auto out_y1 = npu.output(e->enc_y1_out);
    auto out_feat = npu.output(e->enc_feat_out);
    size_t z_n = e->z_r.size();
    size_t y_n = e->y0_r.size();
    if (out_z.size() < z_n || out_y0.size() < y_n || out_y1.size() < y_n ||
        out_feat.size() != e->ref_prev.size() || !all_finite(out_z) ||
        !all_finite(out_y0) || !all_finite(out_y1) ||
        !all_finite(out_feat))
        return bad(rkvc::Status::Hw, "bad NPU outputs");
    for (size_t i = 0; i < z_n; ++i)
        e->z_r[i] = std::lrintf(pixel::f16_to_f32(out_z[i]));
    for (size_t i = 0; i < y_n; ++i) {
        e->y0_r[i] = std::lrintf(pixel::f16_to_f32(out_y0[i]));
        e->y1_r[i] = std::lrintf(pixel::f16_to_f32(out_y1[i]));
    }
    e->ref_prev.assign(out_feat.begin(), out_feat.end());
    if (mark_ltr) {
        e->ref_ltr = e->ref_prev;
        e->have_ltr = true;
    }

    rkvc::Status xs = pixel::extract_scales(
        e->z_r.data(), e->s0.data(), e->s1.data(), e->y_c, e->y_h, e->y_w,
        e->z_c, e->z_h, e->z_w, e->entropy.channel_repeat,
        e->entropy.spatial_repeat, e->entropy.scale_max_index);
    if (xs != rkvc::Status::Ok)
        return bad(xs, "scale extraction mismatch");

    RansEncoder enc(RansVariant::Byte, 65536);
    rkvc::Status rs = enc.encode(e->g_coder, e->s1, e->y1_r);
    if (rs == rkvc::Status::Ok)
        rs = enc.encode(e->g_coder, e->s0, e->y0_r);
    if (rs == rkvc::Status::Ok)
        rs = enc.encode(e->b_coder, e->z_idx, e->z_r);
    if (rs != rkvc::Status::Ok)
        return bad(rkvc::Status::Internal, "rANS encode failed");
    const uint8_t* bits = nullptr;
    size_t bits_size = 0;
    if (enc.flush(&bits, &bits_size) != rkvc::Status::Ok || !bits)
        return bad(rkvc::Status::Internal, "rANS flush failed");

    size_t record_size =
        (e->frame_count == 0 ? kHdrSize : 0) + kRecSize + bits_size;
    uint8_t* record = static_cast<uint8_t*>(malloc(record_size));
    if (!record)
        return bad(rkvc::Status::Nomem, "no memory");
    {
        uint8_t* curp = record;
        if (e->frame_count == 0) {
            Header hdr;
            hdr.width = e->img_w;
            hdr.height = e->img_h;
            hdr.fps_num = kMlvcFps;
            hdr.fps_den = 1;
            hdr.qp = (uint32_t)e->qp;
            hdr.iframe_period = e->iframe_period;
            hdr.ltr_start_idx = e->ltr_start_idx;
            hdr.ltr_period = e->ltr_period;
            hdr.flags =
                ((e->proactive_ltr && e->ltr_period) ? kHdrProactiveLtr : 0) |
                (e->rc ? kHdrCbr : 0);
            hdr.target_bitrate_bps =
                (uint32_t)(e->rc_bitrate > 0 ? e->rc_bitrate : 0);
            write_header(curp, hdr);
            curp += kHdrSize;
        }
        write_record(curp, (uint32_t)bits_size, used_qp, rec_flags);
        memcpy(curp + kRecSize, bits, bits_size);
    }
    if (e->rc)
        e->rc->update((int64_t)(record_size - bits_size) * 8,
                      (int64_t)bits_size * 8);
    rkvc::Spec spec;
    spec.fmt = rkvc::PixelFormat::Bitstream;
    auto fr = rkvc::Frame::borrow_host(spec, record, record_size,
                                       {free_buffer, record});
    if (!fr) {
        free(record);
        return bad(fr.status(), "output frame alloc failed");
    }
    fr.value()->set_pts(input->pts());
    fr.value()->set_dts(input->pts());
    if (is_idr)
        fr.value()->set_flags(rkvc::kFlagKeyframe);
    rkvc::Status emit_st = e->emit->emit(0, fr.value());
    e->frame_count++;
    e->cur_frame_idx = is_idr ? 1 : cur + 1;
    return emit_st;
}

rkvc::Status MlvcEncoderNode::flush(rkvc::Diag*) {
    return rkvc::Status::Ok;  // frame-synchronous: nothing buffered
}

void MlvcEncoderNode::close() noexcept {
    Impl* e = impl_.get();
    if (e)
        e->rs.models.clear();
}

}  // namespace mlvc
