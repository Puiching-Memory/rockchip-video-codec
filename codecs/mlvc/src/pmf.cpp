// SPDX-License-Identifier: AGPL-3.0-or-later
#include "mlvc/pmf.hpp"

#include <cstring>
#include <utility>

namespace mlvc {

namespace {

uint32_t rd_u32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

double rd_f64(const uint8_t* p) noexcept {
    uint64_t bits = rd_u32(p) | (static_cast<uint64_t>(rd_u32(p + 4)) << 32);
    double out = 0;
    memcpy(&out, &bits, 8);
    return out;
}

constexpr uint32_t kMaxLen = 1u << 20;
constexpr uint32_t kMaxTab = 16u << 20;

}  // namespace

rkvc::Result<Pmf> load_pmf(const uint8_t* data, size_t size,
                           rkvc::Diag* diag) {
    using R = rkvc::Result<Pmf>;
    auto bad = [&](const char* reason) {
        if (diag)
            diag->add("load", "pmf", reason);
        return R::failure(rkvc::Status::Format, diag ? *diag : rkvc::Diag{});
    };
    if (!data || size < 20 || memcmp(data, "PMF1", 4) != 0)
        return bad("bad magic");
    uint32_t nL = rd_u32(data + 4);
    uint32_t nO = rd_u32(data + 8);
    uint32_t nT = rd_u32(data + 12);
    if (!nL || nL > kMaxLen || !nO || nO > kMaxLen || !nT || nT > kMaxTab)
        return bad("bad counts");
    uint64_t need =
        (static_cast<uint64_t>(nL) + nO + nT) * 4 + 4;
    if (need > size - 16)
        return bad("truncated arrays");
    Pmf p;
    p.lengths.assign(reinterpret_cast<const int32_t*>(data + 16),
                     reinterpret_cast<const int32_t*>(data + 16 + nL * 4));
    p.offsets.assign(reinterpret_cast<const int32_t*>(data + 16 + nL * 4),
                     reinterpret_cast<const int32_t*>(data + 16 +
                                                      (nL + nO) * 4));
    p.table.assign(reinterpret_cast<const int32_t*>(data + 16 +
                                                    (nL + nO) * 4),
                   reinterpret_cast<const int32_t*>(data + 16 +
                                                    (nL + nO + nT) * 4));
    const uint8_t* cur = data + 16 + (nL + nO + nT) * 4;
    uint32_t tag = rd_u32(cur);
    cur += 4;
    if (tag == 1) {
        if (size - (cur - data) < 24)
            return bad("truncated gaussian tail");
        p.scale_min = rd_f64(cur);
        p.scale_max = rd_f64(cur + 8);
        p.scale_levels = rd_u32(cur + 16);
        p.index_space = rd_u32(cur + 20);
        p.gaussian = true;
    } else if (tag == 2) {
        if (size - (cur - data) < 8)
            return bad("truncated bitest tail");
        p.qp_num = rd_u32(cur);
        p.channels = rd_u32(cur + 4);
    } else {
        return bad("unknown tag");
    }
    return R::success(std::move(p));
}

}  // namespace mlvc
