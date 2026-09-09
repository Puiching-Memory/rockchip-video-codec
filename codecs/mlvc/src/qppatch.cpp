// SPDX-License-Identifier: AGPL-3.0-or-later
#include "mlvc/qppatch.hpp"

#include <array>
#include <cstring>

namespace mlvc {

namespace {

uint32_t rd_u32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t rd_u64(const uint8_t* p) noexcept {
    return rd_u32(p) | (static_cast<uint64_t>(rd_u32(p + 4)) << 32);
}

constexpr std::array<uint32_t, 256> kCrcTable = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t r = i;
        for (int k = 0; k < 8; ++k)
            r = (r >> 1) ^ (0xEDB88320u & (0u - (r & 1)));
        t[i] = r;
    }
    return t;
}();

constexpr size_t kHeaderSize = 48;
constexpr size_t kRangeSize = 8;

}  // namespace

uint32_t crc32(const uint8_t* data, size_t size) noexcept {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i)
        c = kCrcTable[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

rkvc::Result<void> apply_qppatch(uint8_t* base, size_t base_size,
                                 const uint8_t* patch, size_t patch_size,
                                 int expected_qp, rkvc::Diag* diag) {
    using R = rkvc::Result<void>;
    auto bad = [&](const char* reason) {
        if (diag)
            diag->add("apply", "qppatch", reason);
        return R::failure(rkvc::Status::Format, diag ? *diag : rkvc::Diag{});
    };
    if (!base || !patch)
        return R::failure(rkvc::Status::Invalid);
    if (patch_size < kHeaderSize || memcmp(patch, "QPP1", 4) != 0)
        return bad("bad magic");
    if (rd_u32(patch + 4) != 1)
        return bad("bad version");
    if (rd_u64(patch + 8) != base_size)
        return bad("base size mismatch");
    uint32_t qp = rd_u32(patch + 16);
    uint32_t num_ranges = rd_u32(patch + 20);
    if (rd_u32(patch + 24) != 0)
        return bad("bad flags");
    uint32_t base_crc = rd_u32(patch + 28);
    uint32_t payload_crc = rd_u32(patch + 32);
    if (expected_qp >= 0 && qp != static_cast<uint32_t>(expected_qp))
        return bad("qp mismatch");
    if (crc32(base, base_size) != base_crc)
        return bad("base crc mismatch");
    if (num_ranges > (UINT32_MAX - kHeaderSize) / kRangeSize)
        return bad("range count overflow");
    size_t ranges_bytes = static_cast<size_t>(num_ranges) * kRangeSize;
    if (patch_size < kHeaderSize + ranges_bytes)
        return bad("truncated ranges");
    const uint8_t* ranges = patch + kHeaderSize;
    const uint8_t* payload = ranges + ranges_bytes;
    size_t payload_size = patch_size - (kHeaderSize + ranges_bytes);
    size_t need = 0;
    for (uint32_t i = 0; i < num_ranges; ++i) {
        const uint8_t* r = ranges + static_cast<size_t>(i) * kRangeSize;
        uint64_t off = rd_u32(r);
        uint64_t len = rd_u32(r + 4);
        if (off + len > base_size)
            return bad("range out of bounds");
        if (len > SIZE_MAX - need)
            return bad("payload size overflow");
        need += static_cast<size_t>(len);
    }
    if (need != payload_size || crc32(payload, payload_size) != payload_crc)
        return bad("payload crc mismatch");
    size_t cursor = 0;
    for (uint32_t i = 0; i < num_ranges; ++i) {
        const uint8_t* r = ranges + static_cast<size_t>(i) * kRangeSize;
        uint32_t off = rd_u32(r);
        uint32_t len = rd_u32(r + 4);
        if (len)
            memcpy(base + off, payload + cursor, len);
        cursor += len;
    }
    return R::success();
}

}  // namespace mlvc
