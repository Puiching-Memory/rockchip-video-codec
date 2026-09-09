// SPDX-License-Identifier: AGPL-3.0-or-later
// Real RKNN Phase-RLFN runtime (board-verified in Phase 5).
#include <cstring>
#include <new>
#include <utility>
#include <vector>

#include "rknn_api.h"
#include "sr/post.hpp"
#include "sr/upscale.hpp"

namespace sr {

namespace {

bool rknn_host_input_acceptable(const rknn_tensor_attr& a) noexcept {
    if (a.type == RKNN_TENSOR_UINT8 || a.type == RKNN_TENSOR_FLOAT16 ||
        a.type == RKNN_TENSOR_FLOAT32)
        return true;
    // w8a8 quantized exports reinterpret uint8 bytes (real = q - zp).
    return a.type == RKNN_TENSOR_INT8 &&
           a.qnt_type != RKNN_TENSOR_QNT_NONE && a.zp == -128;
}

class RknnRuntime : public SrRuntime {
public:
    ~RknnRuntime() override {
        if (ctx_ != 0)
            rknn_destroy(ctx_);
    }
    rkvc::Status open(std::span<const uint8_t> model, SrGeometry& g,
                      rkvc::Diag* diag) override {
        auto bad = [&](const char* reason) {
            if (diag)
                diag->add("open", "rknn.upscale", reason);
            return rkvc::Status::Hw;
        };
        if (model.empty() || model.size() > UINT32_MAX)
            return bad("empty model");
        if (rknn_init(&ctx_, (void*)model.data(), (uint32_t)model.size(), 0,
                      nullptr) != RKNN_SUCC)
            return bad("rknn_init rejected model/runtime pair");
        rknn_input_output_num num = {};
        if (rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &num, sizeof(num)) !=
                RKNN_SUCC ||
            num.n_input != 1 || num.n_output != 1) {
            rknn_destroy(ctx_);
            ctx_ = 0;
            return bad("Phase-RLFN needs exactly 1 input and 1 output");
        }
        memset(&in_attr_, 0, sizeof(in_attr_));
        memset(&out_attr_, 0, sizeof(out_attr_));
        if (rknn_query(ctx_, RKNN_QUERY_INPUT_ATTR, &in_attr_,
                       sizeof(in_attr_)) != RKNN_SUCC ||
            rknn_query(ctx_, RKNN_QUERY_OUTPUT_ATTR, &out_attr_,
                       sizeof(out_attr_)) != RKNN_SUCC) {
            rknn_destroy(ctx_);
            ctx_ = 0;
            return bad("attr query failed");
        }
        auto chw = [](const rknn_tensor_attr& a, uint32_t& c, uint32_t& h,
                      uint32_t& w) {
            if (a.n_dims != 4)
                return false;
            if (a.fmt == RKNN_TENSOR_NCHW) {
                c = a.dims[1];
                h = a.dims[2];
                w = a.dims[3];
            } else if (a.fmt == RKNN_TENSOR_NHWC) {
                h = a.dims[1];
                w = a.dims[2];
                c = a.dims[3];
            } else {
                return false;
            }
            return c > 0 && h > 0 && w > 0;
        };
        uint32_t in_c = 0, in_h = 0, in_w = 0, out_c = 0, out_h = 0,
                 out_w = 0;
        if (!chw(in_attr_, in_c, in_h, in_w) ||
            !chw(out_attr_, out_c, out_h, out_w) ||
            in_attr_.fmt != RKNN_TENSOR_NHWC ||
            out_attr_.fmt != RKNN_TENSOR_NCHW ||
            in_c != post::kPhaseInCh || out_c != post::kPhaseOutCh ||
            in_w != out_w || in_h != out_h ||
            in_w > post::kMaxDim / post::kPhaseOutFactor ||
            in_h > post::kMaxDim / post::kPhaseOutFactor ||
            !rknn_host_input_acceptable(in_attr_) ||
            in_attr_.n_elems !=
                (size_t)post::kPhaseInCh * in_w * in_h ||
            out_attr_.n_elems !=
                (size_t)post::kPhaseOutCh * in_w * in_h) {
            rknn_destroy(ctx_);
            ctx_ = 0;
            return bad("tensor contract mismatch");
        }
        g.core_w = in_w;
        g.core_h = in_h;
        g.in_w = in_w * post::kPhaseInFactor;
        g.in_h = in_h * post::kPhaseInFactor;
        g.out_w = out_w * post::kPhaseOutFactor;
        g.out_h = out_h * post::kPhaseOutFactor;
        return rkvc::Status::Ok;
    }
    rkvc::Status run(std::span<const uint8_t> packed,
                     std::span<float> residual, rkvc::Diag* diag) override {
        auto bad = [&](const char* reason) {
            if (diag)
                diag->add("process", "rknn.upscale", reason);
            return rkvc::Status::Hw;
        };
        if (ctx_ == 0 || packed.size() != in_attr_.n_elems ||
            residual.size() != out_attr_.n_elems)
            return bad("bad buffers");
        rknn_input in = {};
        in.index = 0;
        in.buf = (void*)packed.data();
        in.size = (uint32_t)packed.size();
        in.type = RKNN_TENSOR_UINT8;
        in.fmt = RKNN_TENSOR_NHWC;
        if (rknn_inputs_set(ctx_, 1, &in) != RKNN_SUCC)
            return bad("inputs_set failed");
        if (rknn_run(ctx_, nullptr) != RKNN_SUCC)
            return bad("inference failed");
        rknn_output out = {};
        out.index = 0;
        out.want_float = 1;
        if (rknn_outputs_get(ctx_, 1, &out, nullptr) != RKNN_SUCC)
            return bad("output retrieval failed");
        if (!out.buf ||
            out.size < out_attr_.n_elems * sizeof(float)) {
            rknn_outputs_release(ctx_, 1, &out);
            return bad("short output");
        }
        memcpy(residual.data(), out.buf, out_attr_.n_elems * sizeof(float));
        rknn_outputs_release(ctx_, 1, &out);
        return rkvc::Status::Ok;
    }

private:
    rknn_context ctx_ = 0;
    rknn_tensor_attr in_attr_ = {};
    rknn_tensor_attr out_attr_ = {};
};

}  // namespace

std::unique_ptr<SrRuntime> make_rknn_runtime() {
    return std::unique_ptr<SrRuntime>(new (std::nothrow) RknnRuntime());
}

}  // namespace sr
