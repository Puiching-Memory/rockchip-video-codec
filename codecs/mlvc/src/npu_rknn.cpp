// SPDX-License-Identifier: AGPL-3.0-or-later
// Real RKNN backend, host-copy path only (board-verified in Phase 5).
// Layout contract: image/reference inputs NHWC fp16, latent inputs and all
// outputs logical NCHW fp16. If board bring-up shows the exported models
// want native NC1HWC2 for z/y, switch those feeds to the native mem API.
#include "mlvc/npu.hpp"

#include <cstring>
#include <new>
#include <utility>
#include <vector>

#include "mlvc/qppatch.hpp"
#include "rknn_api.h"

namespace mlvc {

namespace {

rkvc::Status rknn_failed(rkvc::Diag* diag, const char* reason) {
    if (diag)
        diag->add("npu", "rknn", reason);
    return rkvc::Status::Hw;
}

class RknnModel : public NpuModel {
public:
    ~RknnModel() override {
        if (ctx_ != 0)
            rknn_destroy(ctx_);
    }
    rkvc::Status init(std::span<const uint8_t> model,
                      std::span<const uint8_t> patch, int patch_qp,
                      rkvc::Diag* diag) override {
        if (model.empty() || model.size() > (256u << 20))
            return rknn_failed(diag, "bad model bytes");
        owned_ = std::vector<uint8_t>(model.begin(), model.end());
        if (!patch.empty()) {
            rkvc::Diag d;
            auto pr = apply_qppatch(owned_.data(), owned_.size(),
                                    patch.data(), patch.size(), patch_qp, &d);
            if (!pr)
                return rknn_failed(diag, "qppatch apply failed");
        }
        if (rknn_init(&ctx_, (void*)owned_.data(), (uint32_t)owned_.size(),
                      0, nullptr) != RKNN_SUCC) {
            ctx_ = 0;
            return rknn_failed(diag, "rknn_init failed");
        }
        if (rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io_,
                       sizeof(io_)) != RKNN_SUCC ||
            io_.n_input == 0 || io_.n_input > 8 || io_.n_output == 0 ||
            io_.n_output > 8) {
            rknn_destroy(ctx_);
            ctx_ = 0;
            return rknn_failed(diag, "io num query failed");
        }
        in_attrs_.resize(io_.n_input);
        for (uint32_t i = 0; i < io_.n_input; ++i) {
            memset(&in_attrs_[i], 0, sizeof(in_attrs_[i]));
            in_attrs_[i].index = i;
            if (rknn_query(ctx_, RKNN_QUERY_INPUT_ATTR, &in_attrs_[i],
                           sizeof(in_attrs_[i])) != RKNN_SUCC) {
                rknn_destroy(ctx_);
                ctx_ = 0;
                return rknn_failed(diag, "input attr query failed");
            }
        }
        out_attrs_.resize(io_.n_output);
        for (uint32_t i = 0; i < io_.n_output; ++i) {
            memset(&out_attrs_[i], 0, sizeof(out_attrs_[i]));
            out_attrs_[i].index = i;
            if (rknn_query(ctx_, RKNN_QUERY_OUTPUT_ATTR, &out_attrs_[i],
                           sizeof(out_attrs_[i])) != RKNN_SUCC) {
                rknn_destroy(ctx_);
                ctx_ = 0;
                return rknn_failed(diag, "output attr query failed");
            }
        }
        return rkvc::Status::Ok;
    }
    std::vector<NpuTensorInfo> inputs() const override {
        return descs(in_attrs_);
    }
    std::vector<NpuTensorInfo> outputs() const override {
        return descs(out_attrs_);
    }
    rkvc::Status set_input(size_t idx,
                           std::span<const uint16_t> fp16) override {
        if (idx >= in_attrs_.size() || fp16.empty())
            return rkvc::Status::Invalid;
        if (fp16.size() != in_attrs_[idx].n_elems)
            return rkvc::Status::Invalid;
        if (inputs_.size() != in_attrs_.size())
            inputs_.resize(in_attrs_.size());
        inputs_[idx].assign(fp16.begin(), fp16.end());
        return rkvc::Status::Ok;
    }
    rkvc::Status run(rkvc::Diag* diag) override {
        if (ctx_ == 0 || inputs_.size() != in_attrs_.size())
            return rknn_failed(diag, "model not ready");
        std::vector<rknn_input> ins(in_attrs_.size());
        memset(ins.data(), 0, ins.size() * sizeof(ins[0]));
        for (size_t i = 0; i < ins.size(); ++i) {
            if (inputs_[i].empty())
                return rknn_failed(diag, "input not fed");
            ins[i].index = (uint32_t)i;
            ins[i].buf = (void*)inputs_[i].data();
            ins[i].size = (uint32_t)(inputs_[i].size() * 2);
            ins[i].type = RKNN_TENSOR_FLOAT16;
            ins[i].fmt = RKNN_TENSOR_NHWC;
        }
        if (rknn_inputs_set(ctx_, (uint32_t)ins.size(), ins.data()) !=
            RKNN_SUCC)
            return rknn_failed(diag, "inputs_set failed");
        if (rknn_run(ctx_, nullptr) != RKNN_SUCC)
            return rknn_failed(diag, "run failed");
        std::vector<rknn_output> outs(out_attrs_.size());
        memset(outs.data(), 0, outs.size() * sizeof(outs[0]));
        for (size_t i = 0; i < outs.size(); ++i) {
            outs[i].index = (uint32_t)i;
            outs[i].want_float = 0;
            outs[i].is_prealloc = 0;
        }
        if (rknn_outputs_get(ctx_, (uint32_t)outs.size(), outs.data(),
                             nullptr) != RKNN_SUCC)
            return rknn_failed(diag, "outputs_get failed");
        last_.assign(outs.size(), {});
        for (size_t i = 0; i < outs.size(); ++i) {
            size_t want = (size_t)out_attrs_[i].n_elems * 2;
            if (!outs[i].buf || outs[i].size < want) {
                rknn_outputs_release(ctx_, (uint32_t)outs.size(),
                                     outs.data());
                return rknn_failed(diag, "short output");
            }
            const uint16_t* p = (const uint16_t*)outs[i].buf;
            last_[i].assign(p, p + out_attrs_[i].n_elems);
        }
        rknn_outputs_release(ctx_, (uint32_t)outs.size(), outs.data());
        return rkvc::Status::Ok;
    }
    std::span<const uint16_t> output(size_t idx) const override {
        if (idx >= last_.size())
            return {};
        return last_[idx];
    }

private:
    static std::vector<NpuTensorInfo> descs(
        const std::vector<rknn_tensor_attr>& attrs) {
        std::vector<NpuTensorInfo> out;
        for (const auto& a : attrs) {
            NpuTensorInfo t;
            t.name = a.name;
            for (uint32_t i = 0; i < a.n_dims && i < 8; ++i)
                t.dims.push_back(a.dims[i]);
            out.push_back(std::move(t));
        }
        return out;
    }

    rknn_context ctx_ = 0;
    rknn_input_output_num io_ = {};
    std::vector<rknn_tensor_attr> in_attrs_;
    std::vector<rknn_tensor_attr> out_attrs_;
    std::vector<uint8_t> owned_;
    std::vector<std::vector<uint16_t>> inputs_;
    std::vector<std::vector<uint16_t>> last_;
};

}  // namespace

std::unique_ptr<NpuModel> make_rknn_model() {
    return std::unique_ptr<NpuModel>(new (std::nothrow) RknnModel());
}

}  // namespace mlvc
