// SPDX-License-Identifier: AGPL-3.0-or-later
#include "mlvc/npu.hpp"

#include <cstring>

#include "mlvc/pixel.hpp"

namespace mlvc {

namespace {

// Deterministic invertible stub: encoder quantizes a 4x4-averaged luma-ish
// thumb (plus small per-channel offsets) to small ints; decoder inverts by
// nearest-neighbor upscale. Exercises the full container/rANS/DPB path
// without hardware.
class FakeEncoder : public NpuModel {
public:
    explicit FakeEncoder(FakeNpuGeometry g) : g_(g) {}
    rkvc::Status init(std::span<const uint8_t>, std::span<const uint8_t>,
                      int, rkvc::Diag*) override {
        return rkvc::Status::Ok;
    }
    std::vector<NpuTensorInfo> inputs() const override {
        return {{{"x", {1, g_.img, g_.img, 3}},
                 {"ref_feature", {1, g_.ref_hw, g_.ref_hw, g_.ref_c}}}};
    }
    std::vector<NpuTensorInfo> outputs() const override {
        return {{{"feature", {1, g_.ref_hw, g_.ref_hw, g_.ref_c}},
                 {"z_raw", {1, g_.z_c, g_.z_hw, g_.z_hw}},
                 {"y_raw_0", {1, g_.y_c, g_.y_hw, g_.y_hw}},
                 {"y_raw_1", {1, g_.y_c, g_.y_hw, g_.y_hw}}}};
    }
    rkvc::Status set_input(size_t idx,
                           std::span<const uint16_t> fp16) override {
        if (idx >= 2 || fp16.empty())
            return rkvc::Status::Invalid;
        ins_[idx].assign(fp16.begin(), fp16.end());
        return rkvc::Status::Ok;
    }
    rkvc::Status run(rkvc::Diag*) override {
        if (ins_[0].empty())
            return rkvc::Status::Invalid;
        // y0[c,y,x] = round(2 * avg4x4(luma)) + c - 5, y1 = 0,
        // z[c,y,x] = (c + x + y) % 5, feat = 0.
        const uint32_t S = g_.img / g_.y_hw;  // 4
        y0_.assign(g_.y_c * g_.y_hw * g_.y_hw, 0);
        y1_.assign(g_.y_c * g_.y_hw * g_.y_hw, 0);
        for (uint32_t c = 0; c < g_.y_c; ++c) {
            for (uint32_t y = 0; y < g_.y_hw; ++y) {
                for (uint32_t x = 0; x < g_.y_hw; ++x) {
                    float acc = 0;
                    for (uint32_t dy = 0; dy < S; ++dy)
                        for (uint32_t dx = 0; dx < S; ++dx) {
                            uint32_t sy = y * S + dy;
                            uint32_t sx = x * S + dx;
                            const uint16_t* px =
                                &ins_[0][(sy * g_.img + sx) * 3];
                            acc += pixel::f16_to_f32(px[c % 3]);
                        }
                    acc /= S * S;
                    float v = acc * 2 + (c < 3 ? (int)c - 1 : 0);
                    y0_[(c * g_.y_hw + y) * g_.y_hw + x] =
                        pixel::f32_to_f16(v);
                }
            }
        }
        z_.assign(g_.z_c * g_.z_hw * g_.z_hw, 0);
        for (uint32_t c = 0; c < g_.z_c; ++c)
            for (uint32_t y = 0; y < g_.z_hw; ++y)
                for (uint32_t x = 0; x < g_.z_hw; ++x)
                    z_[(c * g_.z_hw + y) * g_.z_hw + x] =
                        pixel::f32_to_f16((float)((c + x + y) % 5));
        feat_.assign(g_.ref_c * g_.ref_hw * g_.ref_hw, 0);
        return rkvc::Status::Ok;
    }
    std::span<const uint16_t> output(size_t idx) const override {
        // Order: feature, z_raw, y_raw_0, y_raw_1.
        switch (idx) {
            case 0:
                return feat_;
            case 1:
                return z_;
            case 2:
                return y0_;
            case 3:
                return y1_;
            default:
                return {};
        }
    }

private:
    FakeNpuGeometry g_;
    std::vector<uint16_t> ins_[2];
    std::vector<uint16_t> y0_, y1_, z_, feat_;
};

class FakeDecoder : public NpuModel {
public:
    explicit FakeDecoder(FakeNpuGeometry g) : g_(g) {}
    rkvc::Status init(std::span<const uint8_t>, std::span<const uint8_t>,
                      int, rkvc::Diag*) override {
        return rkvc::Status::Ok;
    }
    std::vector<NpuTensorInfo> inputs() const override {
        return {{{"z_raw", {1, g_.z_c, g_.z_hw, g_.z_hw}},
                 {"y_raw_0", {1, g_.y_c, g_.y_hw, g_.y_hw}},
                 {"y_raw_1", {1, g_.y_c, g_.y_hw, g_.y_hw}},
                 {"ref_feature", {1, g_.ref_hw, g_.ref_hw, g_.ref_c}}}};
    }
    std::vector<NpuTensorInfo> outputs() const override {
        return {{{"x_hat", {1, 3, g_.img, g_.img}},
                 {"feature", {1, g_.ref_hw, g_.ref_hw, g_.ref_c}}}};
    }
    rkvc::Status set_input(size_t idx,
                           std::span<const uint16_t> fp16) override {
        if (idx >= 4 || fp16.empty())
            return rkvc::Status::Invalid;
        ins_[idx].assign(fp16.begin(), fp16.end());
        return rkvc::Status::Ok;
    }
    rkvc::Status run(rkvc::Diag*) override {
        if (ins_[1].empty())
            return rkvc::Status::Invalid;
        // x_hat[c,Y,X] = 0.5 * y0[c,Y/4,X/4] (nearest upscale, NCHW).
        const uint32_t S = g_.img / g_.y_hw;
        x_.assign(3 * g_.img * g_.img, 0);
        for (uint32_t c = 0; c < 3; ++c) {
            for (uint32_t y = 0; y < g_.img; ++y) {
                for (uint32_t x = 0; x < g_.img; ++x) {
                    float v = pixel::f16_to_f32(
                        ins_[1][(c * g_.y_hw + y / S) * g_.y_hw + x / S]);
                    x_[(c * g_.img + y) * g_.img + x] =
                        pixel::f32_to_f16(v * 0.5f);
                }
            }
        }
        feat_.assign(g_.ref_c * g_.ref_hw * g_.ref_hw, 0);
        return rkvc::Status::Ok;
    }
    std::span<const uint16_t> output(size_t idx) const override {
        if (idx == 0)
            return x_;
        if (idx == 1)
            return feat_;
        return {};
    }

private:
    FakeNpuGeometry g_;
    std::vector<uint16_t> ins_[4];
    std::vector<uint16_t> x_, feat_;
};

}  // namespace

std::unique_ptr<NpuModel> make_fake_encoder(const FakeNpuGeometry& g) {
    return std::unique_ptr<NpuModel>(new (std::nothrow) FakeEncoder(g));
}

std::unique_ptr<NpuModel> make_fake_decoder(const FakeNpuGeometry& g) {
    return std::unique_ptr<NpuModel>(new (std::nothrow) FakeDecoder(g));
}

}  // namespace mlvc
