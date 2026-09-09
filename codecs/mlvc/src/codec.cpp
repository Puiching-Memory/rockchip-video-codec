// SPDX-License-Identifier: AGPL-3.0-or-later
#include "mlvc/codec.hpp"

#include <cstdint>
#include <cstring>
#include <new>
#include <utility>

#include "mlvc/pmf.hpp"
#include "mlvc/qppatch.hpp"

namespace mlvc {

namespace {

const rkvc::ModelPayload* find_kind(const rkvc::Model& m, const char* kind) {
    return m.find(kind ? kind : "");
}

int qppatch_qp(const rkvc::ModelPayload* v) noexcept {
    if (!v || v->data.size() < 48 || memcmp(v->data.data(), "QPP1", 4) != 0)
        return -1;
    const uint8_t* p = v->data.data();
    return (int)((uint32_t)p[16] | ((uint32_t)p[17] << 8) |
                 ((uint32_t)p[18] << 16) | ((uint32_t)p[19] << 24));
}

}  // namespace

rkvc::Status init_rans_coders(const rkvc::Model& model, RansCoder& g,
                              RansCoder& b, EntropyConfig& cfg,
                              rkvc::Diag* diag) {
    auto bad = [&](const char* reason) {
        if (diag)
            diag->add("bind", "pmf", reason);
        return rkvc::Status::Format;
    };
    const rkvc::ModelPayload* gv = find_kind(model, kKindPmfGaussian);
    if (!gv)
        gv = find_kind(model, "pmf");
    const rkvc::ModelPayload* bv = find_kind(model, kKindPmfBitest);
    if (!gv || !bv)
        return bad("missing pmf payloads");
    auto gp = load_pmf(gv->data.data(), gv->data.size(), diag);
    if (!gp)
        return bad("gaussian pmf load failed");
    auto bp = load_pmf(bv->data.data(), bv->data.size(), diag);
    if (!bp)
        return bad("bitest pmf load failed");
    const Pmf& gpmf = gp.value();
    const Pmf& bpmf = bp.value();
    if (!gpmf.gaussian || !gpmf.index_space || !gpmf.scale_levels ||
        gpmf.scale_levels > gpmf.lengths.size() || !bpmf.qp_num ||
        !bpmf.channels ||
        (uint64_t)bpmf.qp_num * bpmf.channels != bpmf.lengths.size())
        return bad("pmf contract mismatch");
    auto gr = RansCoder::create(RansVariant::Byte, gpmf.lengths,
                                gpmf.offsets, gpmf.table, 16, 2, diag);
    if (!gr)
        return bad("gaussian coder init failed");
    auto br = RansCoder::create(RansVariant::Byte, bpmf.lengths,
                                bpmf.offsets, bpmf.table, 16, 2, diag);
    if (!br)
        return bad("bitest coder init failed");
    g = std::move(gr.value());
    b = std::move(br.value());
    cfg.scale_max_index = (int)gpmf.scale_levels - 1;
    cfg.qp_num = bpmf.qp_num;
    cfg.z_channels = bpmf.channels;
    return rkvc::Status::Ok;
}

rkvc::Status collect_rungs(const rkvc::Model& model, int session_qp,
                           std::vector<int>& rung_qp, rkvc::Diag* diag) {
    rung_qp.clear();
    for (const auto& p : model.payloads) {
        if (p.kind != kKindQppatch)
            continue;
        int qp = qppatch_qp(&p);
        if (qp < 0 || qp > 63)
            continue;
        bool dup = false;
        for (int q : rung_qp)
            if (q == qp)
                dup = true;
        if (dup || rung_qp.size() >= kMaxRungs)
            continue;
        size_t at = rung_qp.size();
        while (at > 0 && rung_qp[at - 1] > qp)
            --at;
        rung_qp.insert(rung_qp.begin() + at, qp);
    }
    if (rung_qp.empty()) {
        rung_qp.push_back(session_qp);
        return rkvc::Status::Ok;
    }
    for (int q : rung_qp)
        if (q == session_qp)
            return rkvc::Status::Ok;
    if (diag)
        diag->add("bind", "rungs", "session qp missing from rung set");
    return rkvc::Status::Format;
}

rkvc::Status init_rungs(const rkvc::Model& model,
                        const std::vector<int>& rung_qp, NpuModelFn make_model,
                        RungSet& rs, rkvc::Diag* diag) {
    rs.models.clear();
    rs.rung_qp = rung_qp;
    rs.active = 0;
    const rkvc::ModelPayload* base = find_kind(model, kKindRknn);
    if (!base || base->data.empty()) {
        if (diag)
            diag->add("bind", "rknn", "missing rknn payload");
        return rkvc::Status::Format;
    }
    for (size_t i = 0; i < rung_qp.size(); ++i) {
        const rkvc::ModelPayload* patch = nullptr;
        for (const auto& p : model.payloads) {
            if (p.kind == kKindQppatch && qppatch_qp(&p) == rung_qp[i]) {
                patch = &p;
                break;
            }
        }
        std::unique_ptr<NpuModel> npu =
            make_model ? make_model() : nullptr;
        if (!npu) {
            if (diag)
                diag->add("bind", "npu", "no npu backend");
            return rkvc::Status::Unsupported;
        }
        std::span<const uint8_t> patch_bytes;
        if (patch)
            patch_bytes = std::span<const uint8_t>(patch->data.data(),
                                                   patch->data.size());
        rkvc::Status st = npu->init(base->data, patch_bytes, rung_qp[i], diag);
        if (st != rkvc::Status::Ok) {
            rs.models.clear();
            return st;
        }
        rs.models.push_back(std::move(npu));
    }
    return rkvc::Status::Ok;
}

rkvc::Status setup_qrows(const rkvc::Model& model, const NpuModel& npu,
                         QpTables& qptab, QRows& qr, rkvc::Diag* diag) {
    qr = QRows{};
    bool has_qrows = false;
    for (const auto& t : npu.inputs()) {
        const std::string& n = t.name;
        if (n.size() >= 7 && n.compare(0, 2, "q_") == 0 &&
            n.compare(n.size() - 4, 4, "_row") == 0) {
            has_qrows = true;
            break;
        }
    }
    if (!has_qrows)
        return rkvc::Status::Ok;
    const rkvc::ModelPayload* qv = find_kind(model, kKindQptab);
    if (!qv) {
        if (diag)
            diag->add("bind", "qptab", "qp-dynamic model lacks qptab");
        return rkvc::Status::Format;
    }
    auto qt = load_qptab(qv->data.data(), qv->data.size(), diag);
    if (!qt) {
        if (diag)
            diag->add("bind", "qptab", "qptab load failed");
        return rkvc::Status::Format;
    }
    qptab = std::move(qt.value());
    for (size_t i = 0; i < npu.inputs().size(); ++i) {
        const std::string& n = npu.inputs()[i].name;
        if (n.size() < 7 || n.compare(0, 2, "q_") != 0 ||
            n.compare(n.size() - 4, 4, "_row") != 0)
            continue;
        if (qr.rows.size() >= kMaxQrows) {
            if (diag)
                diag->add("bind", "qptab", "too many q rows");
            return rkvc::Status::Format;
        }
        const QpTable* t = qptab.find(n);
        if (!t) {
            if (diag)
                diag->add("bind", "qptab", "missing row table");
            return rkvc::Status::Format;
        }
        qr.rows.emplace_back(i, t);
    }
    qr.present = !qr.rows.empty();
    return rkvc::Status::Ok;
}

rkvc::Status resolve_entropy_geometry(EntropyConfig& cfg, int qp, int ZC,
                                      int ZH, int ZW, int YC, int YH, int YW,
                                      rkvc::Diag* diag) {
    auto bad = [&] {
        if (diag)
            diag->add("bind", "entropy", "geometry mismatch");
        return rkvc::Status::Format;
    };
    if (qp < 0 || (uint32_t)qp >= cfg.qp_num || ZC <= 0 || ZH <= 0 ||
        ZW <= 0 || YC <= 0 || YH <= 0 || YW <= 0 ||
        (uint32_t)ZC != cfg.z_channels)
        return bad();
    auto ceil_div = [](int v, int d) { return (v + d - 1) / d; };
    if (ZH == ceil_div(YH, 8) && ZW == ceil_div(YW, 8)) {
        cfg.channel_repeat = 2;
        cfg.spatial_repeat = 8;
    } else if (ZH == ceil_div(YH, 4) && ZW == ceil_div(YW, 4)) {
        cfg.channel_repeat = 4;
        cfg.spatial_repeat = 4;
    } else {
        return bad();
    }
    if ((int64_t)(2 * YC + cfg.channel_repeat - 1) / cfg.channel_repeat > ZC)
        return bad();
    return rkvc::Status::Ok;
}

int nearest_rung(const std::vector<int>& rung_qp, int q) noexcept {
    int best = 0;
    int best_d = INT32_MAX;
    for (size_t i = 0; i < rung_qp.size(); ++i) {
        int d = rung_qp[i] > q ? rung_qp[i] - q : q - rung_qp[i];
        if (d < best_d) {
            best_d = d;
            best = (int)i;
        }
    }
    return best;
}

void build_z_idx(std::vector<int32_t>& z_idx, int qp, int ZC, int ZH,
                 int ZW) {
    z_idx.assign((size_t)ZC * ZH * ZW, 0);
    for (int c = 0; c < ZC; c++) {
        int32_t* p = z_idx.data() + (size_t)c * ZH * ZW;
        size_t n = (size_t)ZH * ZW;
        for (size_t i = 0; i < n; i++)
            p[i] = qp * ZC + c;
    }
}

int find_input(const std::vector<NpuTensorInfo>& ts,
               const char* key) noexcept {
    if (!key)
        return -1;
    for (size_t i = 0; i < ts.size(); ++i)
        if (ts[i].name.find(key) != std::string::npos)
            return (int)i;
    return -1;
}

int find_output(const std::vector<NpuTensorInfo>& ts,
                const char* key) noexcept {
    return find_input(ts, key);
}

rkvc::Result<rkvc::NodePtr> MlvcEncodeFactory::create(
    const rkvc::Request& r, rkvc::Diag*) const {
    rkvc::NodePtr n(
        new (std::nothrow) MlvcEncoderNode(r, make_model_));
    if (!n)
        return rkvc::Result<rkvc::NodePtr>::failure(rkvc::Status::Nomem);
    return rkvc::Result<rkvc::NodePtr>::success(std::move(n));
}

rkvc::Result<rkvc::NodePtr> MlvcDecodeFactory::create(
    const rkvc::Request& r, rkvc::Diag*) const {
    rkvc::NodePtr n(
        new (std::nothrow) MlvcDecoderNode(r, make_model_));
    if (!n)
        return rkvc::Result<rkvc::NodePtr>::failure(rkvc::Status::Nomem);
    return rkvc::Result<rkvc::NodePtr>::success(std::move(n));
}

}  // namespace mlvc
