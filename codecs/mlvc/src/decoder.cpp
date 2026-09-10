// SPDX-License-Identifier: AGPL-3.0-or-later
#include "mlvc/codec.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>
#include <span>
#include <utility>
#include <vector>

#include "mlvc/container.hpp"
#include "mlvc/pixel.hpp"

namespace mlvc {

namespace {

void free_buffer(void* p) noexcept {
    free(p);
}

bool fp16_finite(uint16_t h) noexcept {
    return (h & 0x7c00) != 0x7c00;
}

bool all_finite(std::span<const uint16_t> t) noexcept {
    for (uint16_t v : t)
        if (!fp16_finite(v))
            return false;
    return true;
}

}  // namespace

struct MlvcDecoderNode::Impl {
    rkvc::Request req;
    NpuModelFn make_model;
    RungSet rs;
    QpTables qptab;
    QRows qrows;
    RansCoder g_coder;
    RansCoder b_coder;
    EntropyConfig entropy;
    bool rans_ready = false;
    const rkvc::Model* model = nullptr;  // borrowed from the Context
    int qp = kDefaultQp;
    bool model_ready = false;

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
    int dec_z_in = -1;
    int dec_y0_in = -1;
    int dec_y1_in = -1;
    int dec_ref_in = -1;
    int dec_x_out = -1;
    int dec_ref_out = -1;
    // Tail-extracted x_hat (pre-shuffle NCHW [1,C,h',w'], C=3*bs*bs):
    // bs>0 selects the CPU DCR path, else direct 3-channel.
    int x_d2s_bs = 0;
    uint32_t x_pre_c = 0;
    uint32_t x_pre_h = 0;
    uint32_t x_pre_w = 0;

    Demuxer demux;
    std::vector<uint16_t> ref_prev;
    std::vector<uint16_t> ref_ltr;
    std::vector<uint16_t> ref_feed;  // NHWC transit for NPU input
    bool have_ltr = false;
    std::vector<int32_t> z_d, y0_d, y1_d, s0, s1, z_idx;
    int z_idx_qp = -1;
    std::vector<uint16_t> z_f16, y0_f16, y1_f16;
    std::vector<uint8_t> last_nv12;
    bool have_last_frame = false;
    uint32_t frame_count = 0;
    rkvc::Emit* emit = nullptr;
};

MlvcDecoderNode::MlvcDecoderNode(rkvc::Request req, NpuModelFn make_model)
    : impl_(new(std::nothrow) Impl()) {
    if (impl_) {
        impl_->req = std::move(req);
        impl_->make_model = std::move(make_model);
    }
}

MlvcDecoderNode::~MlvcDecoderNode() = default;

std::vector<rkvc::Port> MlvcDecoderNode::make_ports() const {
    rkvc::Port in, out;
    in.name = "bitstream";
    in.is_input = true;
    out.name = "video";
    out.desired.fmt = rkvc::PixelFormat::Nv12;
    out.desired.domain = rkvc::MemDomain::Host;
    return {in, out};
}

rkvc::Status MlvcDecoderNode::configure(std::vector<rkvc::Port>& ports,
                                        rkvc::Diag* diag) {
    for (const auto& p : ports) {
        if (p.is_input && p.resolved.fmt != rkvc::PixelFormat::Unknown &&
            p.resolved.fmt != rkvc::PixelFormat::Bitstream) {
            if (diag)
                diag->add("configure", "mlvc.decode", "bitstream input only");
            return rkvc::Status::Negotiate;
        }
    }
    return rkvc::Node::configure(ports, diag);
}

rkvc::Status MlvcDecoderNode::open(rkvc::Emit* emit, rkvc::Diag* diag) {
    Impl* d = impl_.get();
    if (!d)
        return rkvc::Status::Nomem;
    if (!emit) {
        if (diag)
            diag->add("open", "mlvc.decode", "null emit");
        return rkvc::Status::Invalid;
    }
    if (!d->rans_ready || !d->model) {
        if (diag)
            diag->add("open", "mlvc.decode", "opened without a bound model");
        return rkvc::Status::Format;
    }
    d->emit = emit;
    return rkvc::Status::Ok;
}

rkvc::Status MlvcDecoderNode::bind_model(const rkvc::Model& model,
                                         rkvc::Diag* diag) {
    Impl* d = impl_.get();
    if (!d)
        return rkvc::Status::Nomem;
    auto bad = [&](rkvc::Status s, const char* reason) {
        if (diag)
            diag->add("bind", "mlvc.decode", reason);
        return s;
    };
    if (model.meta.role != "decoder")
        return bad(rkvc::Status::Format, "model role is not decoder");
    rkvc::Status st =
        init_rans_coders(model, d->g_coder, d->b_coder, d->entropy, diag);
    if (st != rkvc::Status::Ok)
        return bad(st, "PMF/rANS init failed");
    d->rans_ready = true;
    d->model = &model;  // QP rungs resolve lazily once the header arrives.
    return rkvc::Status::Ok;
}

namespace {

// Geometry from the active rung (host I/O contract: inputs NHWC,
// outputs logical NCHW); x_hat is NCHW YUV after DCR.
rkvc::Status resolve_dec_geom(MlvcDecoderNode::Impl* d, rkvc::Diag* diag) {
    NpuModel& npu = *d->rs.models[d->rs.active];
    auto ins = npu.inputs();
    auto outs = npu.outputs();
    d->dec_z_in = find_input(ins, "z_raw");
    d->dec_y0_in = find_input(ins, "y_raw_0");
    d->dec_y1_in = find_input(ins, "y_raw_1");
    d->dec_ref_in = find_input(ins, "ref_feature");
    d->dec_x_out = find_output(outs, "x_hat");
    d->dec_ref_out = find_output(outs, "feature");
    if (d->dec_z_in < 0 || d->dec_y0_in < 0 || d->dec_y1_in < 0 ||
        d->dec_ref_in < 0 || d->dec_x_out < 0 || d->dec_ref_out < 0) {
        if (diag)
            diag->add("bind", "mlvc.decode", "tensor contract mismatch");
        return rkvc::Status::Format;
    }
    const auto& zd = ins[d->dec_z_in].dims;
    const auto& yd = ins[d->dec_y0_in].dims;
    const auto& rd = ins[d->dec_ref_in].dims;
    const auto& xd = outs[d->dec_x_out].dims;
    const auto& fd = outs[d->dec_ref_out].dims;
    if (zd.size() != 4 || yd.size() != 4 || rd.size() != 4 || xd.size() != 4 ||
        fd.size() != 4) {
        if (diag)
            diag->add("bind", "mlvc.decode", "tensor rank mismatch");
        return rkvc::Status::Format;
    }
    // x_hat is either full [1,3,H,W] or tail-extracted pre-shuffle
    // NCHW [1,3*bs*bs,h',w'] needing CPU DCR + saturate.
    if (xd[1] == 3) {
        d->x_d2s_bs = 0;
        d->img_h = xd[2];
        d->img_w = xd[3];
    } else {
        uint32_t C = xd[1], H = xd[2], W = xd[3];
        if (C < 3 * 2 * 2 || C % 3 != 0 || !H || !W) {
            if (diag)
                diag->add("bind", "mlvc.decode", "bad tail tensor shape");
            return rkvc::Status::Format;
        }
        uint32_t t = C / 3, bs = 0;
        for (uint32_t b = 2; b * b <= t; ++b)
            if (b * b == t)
                bs = b;
        if (!bs) {
            if (diag)
                diag->add("bind", "mlvc.decode", "tail channels not 3*bs*bs");
            return rkvc::Status::Format;
        }
        d->x_d2s_bs = (int)bs;
        d->x_pre_c = C;
        d->x_pre_h = H;
        d->x_pre_w = W;
        d->img_h = H * bs;
        d->img_w = W * bs;
    }
    d->z_c = zd[3];
    d->z_h = zd[1];
    d->z_w = zd[2];
    d->y_c = yd[3];
    d->y_h = yd[1];
    d->y_w = yd[2];
    d->ref_h = rd[1];
    d->ref_w = rd[2];
    d->ref_c = rd[3];
    auto elems = [](const std::vector<uint32_t>& v) {
        return (size_t)v[1] * v[2] * v[3];
    };
    if (!d->img_w || !d->img_h || elems(fd) != elems(rd)) {
        if (diag)
            diag->add("bind", "mlvc.decode", "tensor shape mismatch");
        return rkvc::Status::Format;
    }
    return rkvc::Status::Ok;
}

rkvc::Status lazy_init(MlvcDecoderNode::Impl* d, rkvc::Diag* diag) {
    std::vector<int> rungs;
    rkvc::Status st = collect_rungs(*d->model, d->qp, rungs, diag);
    if (st != rkvc::Status::Ok)
        return st;
    st = init_rungs(*d->model, rungs, d->make_model, d->rs, diag);
    if (st != rkvc::Status::Ok)
        return st;
    st = setup_qrows(*d->model, *d->rs.models[d->rs.active], d->qptab, d->qrows,
                     diag);
    if (st != rkvc::Status::Ok)
        return st;
    if (d->qrows.present && d->rs.rung_qp.size() != 1) {
        if (diag)
            diag->add("bind", "mlvc.decode", "qp-dynamic forbids rungs");
        return rkvc::Status::Format;
    }
    d->model_ready = true;
    st = resolve_dec_geom(d, diag);
    if (st != rkvc::Status::Ok)
        return st;
    st = resolve_entropy_geometry(d->entropy, d->qp, d->z_c, d->z_h, d->z_w,
                                  d->y_c, d->y_h, d->y_w, diag);
    if (st != rkvc::Status::Ok)
        return st;
    size_t ref_n = (size_t)d->ref_c * d->ref_h * d->ref_w;
    size_t z_n = (size_t)d->z_c * d->z_h * d->z_w;
    size_t y_n = (size_t)d->y_c * d->y_h * d->y_w;
    d->ref_prev.assign(ref_n, 0);
    d->ref_ltr.assign(ref_n, 0);
    d->ref_feed.assign(ref_n, 0);
    d->z_d.assign(z_n, 0);
    d->y0_d.assign(y_n, 0);
    d->y1_d.assign(y_n, 0);
    d->s0.assign(y_n, 0);
    d->s1.assign(y_n, 0);
    d->z_idx.assign(z_n, 0);
    d->z_f16.assign(z_n, 0);
    d->y0_f16.assign(y_n, 0);
    d->y1_f16.assign(y_n, 0);
    d->z_idx_qp = -1;
    return rkvc::Status::Ok;
}

rkvc::Status decode_frame(MlvcDecoderNode::Impl* d, const uint8_t* payload,
                          size_t size, int32_t rec_q, uint32_t rec_flags,
                          rkvc::Diag* diag) {
    auto bad = [&](rkvc::Status s, const char* reason) {
        if (diag)
            diag->add("process", "mlvc.decode", reason);
        return s;
    };
    bool keyframe = (rec_flags & kRecKeyframe) != 0;

    if (d->qrows.present) {
        d->rs.active = 0;
        // q_index selects a FiLM row: qptab_row() clamps an out-of-range
        // value silently and build_z_idx would overflow, so reject it here.
        for (const auto& r : d->qrows.rows) {
            const QpTable* t = r.second;
            if (!t || rec_q < 0 || static_cast<uint32_t>(rec_q) >= t->rows)
                return bad(rkvc::Status::Format,
                           "record q_index outside the qp table");
        }
    } else if (rec_q != d->rs.rung_qp[d->rs.active]) {
        int found = -1;
        for (size_t i = 0; i < d->rs.rung_qp.size(); ++i)
            if (d->rs.rung_qp[i] == rec_q)
                found = (int)i;
        if (found < 0) {
            if (d->rs.rung_qp.size() > 1)
                return bad(rkvc::Status::Format, "record qp has no rung");
        } else {
            d->rs.active = found;
        }
    }
    NpuModel& npu = *d->rs.models[d->rs.active];
    int z_qp = d->qrows.present ? rec_q : d->rs.rung_qp[d->rs.active];
    if (d->z_idx_qp != z_qp) {
        build_z_idx(d->z_idx, z_qp, d->z_c, d->z_h, d->z_w);
        d->z_idx_qp = z_qp;
    }

    if (keyframe) {
        std::fill(d->ref_prev.begin(), d->ref_prev.end(), 0);
        std::fill(d->ref_ltr.begin(), d->ref_ltr.end(), 0);
        d->have_ltr = false;
    }

    RansDecoder dec(RansVariant::Byte);
    if (dec.open(std::span<const uint8_t>(payload, payload ? size : 0)) !=
        rkvc::Status::Ok)
        return bad(rkvc::Status::Format, "rANS stream open failed");
    if (dec.decode(d->b_coder, d->z_d, d->z_idx) != rkvc::Status::Ok)
        return bad(rkvc::Status::Format, "rANS z decode failed");
    if (pixel::extract_scales(d->z_d.data(), d->s0.data(), d->s1.data(), d->y_c,
                              d->y_h, d->y_w, d->z_c, d->z_h, d->z_w,
                              d->entropy.channel_repeat,
                              d->entropy.spatial_repeat,
                              d->entropy.scale_max_index) != rkvc::Status::Ok)
        return bad(rkvc::Status::Format, "scale extraction mismatch");
    if (dec.decode(d->g_coder, d->y0_d, d->s0) != rkvc::Status::Ok ||
        dec.decode(d->g_coder, d->y1_d, d->s1) != rkvc::Status::Ok)
        return bad(rkvc::Status::Format, "rANS y decode failed");
    if (!dec.check_eof())
        return bad(rkvc::Status::Format, "rANS trailing data");

    // NPU z/y inputs are NHWC; rANS yields NCHW int32.
    pixel::nchw_i32_to_nhwc_f16(d->z_d.data(), d->z_f16.data(), (int)d->z_c,
                                (int)d->z_h, (int)d->z_w);
    pixel::nchw_i32_to_nhwc_f16(d->y0_d.data(), d->y0_f16.data(), (int)d->y_c,
                                (int)d->y_h, (int)d->y_w);
    pixel::nchw_i32_to_nhwc_f16(d->y1_d.data(), d->y1_f16.data(), (int)d->y_c,
                                (int)d->y_h, (int)d->y_w);
    const std::vector<uint16_t>& ref =
        ((rec_flags & kRecLtrRecovery) && d->have_ltr) ? d->ref_ltr
                                                       : d->ref_prev;
    // Reference slot is NCHW; the NPU takes NHWC.
    pixel::nchw_f16_to_nhwc(ref.data(), d->ref_feed.data(), (int)d->ref_c,
                            (int)d->ref_h, (int)d->ref_w);
    rkvc::Status st = npu.set_input(d->dec_z_in, d->z_f16);
    if (st == rkvc::Status::Ok)
        st = npu.set_input(d->dec_y0_in, d->y0_f16);
    if (st == rkvc::Status::Ok)
        st = npu.set_input(d->dec_y1_in, d->y1_f16);
    if (st == rkvc::Status::Ok)
        st = npu.set_input(d->dec_ref_in, d->ref_feed);
    for (size_t i = 0; st == rkvc::Status::Ok && i < d->qrows.rows.size();
         ++i) {
        size_t idx = d->qrows.rows[i].first;
        const QpTable* t = d->qrows.rows[i].second;
        const uint16_t* row = qptab_row(*t, (uint32_t)rec_q);
        st = npu.set_input(idx,
                           std::span<const uint16_t>(row, row ? t->cols : 0));
    }
    if (st == rkvc::Status::Ok)
        st = npu.run(diag);
    if (st != rkvc::Status::Ok)
        return bad(st, "NPU run failed");
    auto xh = npu.output(d->dec_x_out);
    auto fv = npu.output(d->dec_ref_out);
    if (fv.size() != d->ref_prev.size() || !all_finite(xh) || !all_finite(fv)) {
        return bad(rkvc::Status::Hw, "bad NPU outputs");
    }
    // Tail-extracted x_hat: CPU DCR to NCHW YUV, then shared saturate path.
    const uint16_t* yuv = xh.data();
    std::vector<uint16_t> d2s;
    if (d->x_d2s_bs > 0) {
        size_t pre_n = (size_t)d->x_pre_c * d->x_pre_h * d->x_pre_w;
        if (xh.size() < pre_n)
            return bad(rkvc::Status::Hw, "short tail output");
        d2s.resize((size_t)3 * d->img_w * d->img_h);
        pixel::nchw_d2s_dcr_f16(xh.data(), d2s.data(), 3, (int)d->x_pre_h,
                                (int)d->x_pre_w, d->x_d2s_bs);
        yuv = d2s.data();
    } else {
        size_t img_n = (size_t)3 * d->img_w * d->img_h;
        if (xh.size() < img_n)
            return bad(rkvc::Status::Hw, "short image output");
    }

    size_t nv12_size = (size_t)d->img_w * d->img_h * 3 / 2;
    uint8_t* nv12 = static_cast<uint8_t*>(malloc(nv12_size));
    if (!nv12)
        return bad(rkvc::Status::Nomem, "no memory");
    pixel::nchw_yuv_fp16_to_nv12_planes(yuv, d->img_w, d->img_h, nv12, d->img_w,
                                        nv12 + (size_t)d->img_w * d->img_h,
                                        d->img_w);

    d->ref_prev.assign(fv.begin(), fv.end());
    if (rec_flags & kRecLtrMark) {
        d->ref_ltr = d->ref_prev;
        d->have_ltr = true;
    }
    d->last_nv12.assign(nv12, nv12 + nv12_size);
    d->have_last_frame = true;

    rkvc::Spec spec;
    spec.width = d->img_w;
    spec.height = d->img_h;
    spec.fmt = rkvc::PixelFormat::Nv12;
    spec.stride = d->img_w;
    spec.ver_stride = d->img_h;
    auto fr =
        rkvc::Frame::borrow_host(spec, nv12, nv12_size, {free_buffer, nv12});
    if (!fr) {
        free(nv12);
        return bad(fr.status(), "output frame alloc failed");
    }
    if (keyframe || d->frame_count == 0)
        fr.value()->set_flags(rkvc::kFlagKeyframe);
    rkvc::Status emit_st = d->emit->emit(0, fr.value());
    d->frame_count++;
    return emit_st;
}

}  // namespace

rkvc::Status MlvcDecoderNode::process(rkvc::FramePtr input, rkvc::Diag* diag) {
    Impl* d = impl_.get();
    if (!d)
        return rkvc::Status::Nomem;
    auto bad = [&](rkvc::Status s, const char* reason) {
        if (diag)
            diag->add("process", "mlvc.decode", reason);
        return s;
    };
    if (!input || input->spec().fmt != rkvc::PixelFormat::Bitstream ||
        !input->data() || !input->size())
        return bad(rkvc::Status::Format, "bitstream host input only");
    const uint8_t* data = static_cast<const uint8_t*>(input->data());
    if (d->demux.append(data, input->size()) != rkvc::Status::Ok)
        return bad(rkvc::Status::Format, "demux append failed");
    for (;;) {
        auto r = d->demux.next();
        if (!r) {
            if (r.status() == rkvc::Status::Again && !d->model_ready &&
                d->demux.has_header()) {
                // Header arrived mid-loop: fall through to lazy init.
            } else if (r.status() == rkvc::Status::Again) {
                return rkvc::Status::Ok;  // need more blocks
            } else {
                return bad(rkvc::Status::Format, "malformed .mlvc stream");
            }
        }
        if (!d->model_ready) {
            if (!d->demux.has_header())
                return rkvc::Status::Ok;
            d->qp = (int)d->demux.header().qp;
            rkvc::Status st = lazy_init(d, diag);
            if (st != rkvc::Status::Ok)
                return bad(st, "lazy init failed");
            continue;
        }
        if (!r)
            return rkvc::Status::Ok;
        const Demuxer::View& v = r.value();
        if (v.q_index == kQindexDropped) {
            if (d->have_last_frame && !d->last_nv12.empty()) {
                size_t n = d->last_nv12.size();
                uint8_t* nv12 = static_cast<uint8_t*>(malloc(n));
                if (!nv12)
                    return bad(rkvc::Status::Nomem, "no memory");
                memcpy(nv12, d->last_nv12.data(), n);
                rkvc::Spec spec;
                spec.width = d->img_w;
                spec.height = d->img_h;
                spec.fmt = rkvc::PixelFormat::Nv12;
                spec.stride = d->img_w;
                spec.ver_stride = d->img_h;
                auto fr = rkvc::Frame::borrow_host(spec, nv12, n,
                                                   {free_buffer, nv12});
                if (!fr) {
                    free(nv12);
                    return bad(fr.status(), "drop frame alloc failed");
                }
                rkvc::Status st = d->emit->emit(0, fr.value());
                d->frame_count++;
                if (st != rkvc::Status::Ok)
                    return st;
            }
            d->demux.consume_record();
            continue;
        }
        rkvc::Status st =
            decode_frame(d, v.data, v.size, v.q_index, v.flags, diag);
        if (st != rkvc::Status::Ok)
            return st;
        d->demux.consume_record();
    }
}

void MlvcDecoderNode::close() noexcept {
    Impl* d = impl_.get();
    if (d)
        d->rs.models.clear();
}

}  // namespace mlvc
