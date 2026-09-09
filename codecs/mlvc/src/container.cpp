// SPDX-License-Identifier: AGPL-3.0-or-later
#include "mlvc/container.hpp"

#include <cstring>

namespace mlvc {

namespace {

uint32_t rd_u32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

void wr_u32(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

}  // namespace

void write_header(uint8_t out[kHdrSize], const Header& hdr) {
    memset(out, 0, kHdrSize);
    memcpy(out, "MLVC1", 5);
    out[5] = 0x02;
    wr_u32(out + 8, hdr.width);
    wr_u32(out + 12, hdr.height);
    wr_u32(out + 16, hdr.fps_num ? hdr.fps_num : 30);
    wr_u32(out + 20, hdr.fps_den ? hdr.fps_den : 1);
    wr_u32(out + 24, hdr.qp);
    wr_u32(out + 32, hdr.iframe_period);
    wr_u32(out + 36, hdr.ltr_start_idx);
    wr_u32(out + 40, hdr.ltr_period);
    wr_u32(out + 44, hdr.flags);
    wr_u32(out + 48, hdr.target_bitrate_bps);
}

rkvc::Result<Header> parse_header(const uint8_t* data, size_t size) {
    if (!data)
        return rkvc::Result<Header>::failure(rkvc::Status::Format);
    if (size < kHdrSize)
        return rkvc::Result<Header>::failure(rkvc::Status::Again);
    if (memcmp(data, "MLVC1", 5) != 0 || data[5] != 0x02)
        return rkvc::Result<Header>::failure(rkvc::Status::Format);
    Header h;
    h.width = rd_u32(data + 8);
    h.height = rd_u32(data + 12);
    h.fps_num = rd_u32(data + 16);
    h.fps_den = rd_u32(data + 20);
    h.qp = rd_u32(data + 24);
    h.frame_count = rd_u32(data + 28);
    h.iframe_period = rd_u32(data + 32);
    h.ltr_start_idx = rd_u32(data + 36);
    h.ltr_period = rd_u32(data + 40);
    h.flags = rd_u32(data + 44);
    h.target_bitrate_bps = rd_u32(data + 48);
    if (!h.width || !h.height)
        return rkvc::Result<Header>::failure(rkvc::Status::Format);
    return rkvc::Result<Header>::success(h);
}

void write_record(uint8_t out[kRecSize], uint32_t payload_size, int32_t q_index,
                  uint32_t flags) {
    wr_u32(out, payload_size);
    wr_u32(out + 4, static_cast<uint32_t>(q_index));
    wr_u32(out + 8, flags);
    wr_u32(out + 12, 0);
}

rkvc::Status Demuxer::append(const uint8_t* data, size_t size) {
    if ((!data && size) || parse_error_)
        return rkvc::Status::Format;
    if (size == 0)
        return rkvc::Status::Ok;
    size_t have = buf_.size();
    if (size > SIZE_MAX - have)
        return rkvc::Status::Nomem;
    buf_.resize(have + size);
    memcpy(buf_.data() + have, data, size);
    return rkvc::Status::Ok;
}

rkvc::Result<Demuxer::View> Demuxer::next() {
    using R = rkvc::Result<View>;
    if (parse_error_)
        return R::failure(rkvc::Status::Format);
    if (!have_header_) {
        auto h = parse_header(buf_.data(), buf_.size());
        if (!h) {
            if (h.status() == rkvc::Status::Again)
                return R::failure(rkvc::Status::Again);
            parse_error_ = true;
            return R::failure(rkvc::Status::Format);
        }
        header_ = h.value();
        have_header_ = true;
        buf_.erase(buf_.begin(), buf_.begin() + kHdrSize);
    }
    if (buf_.size() < kRecSize)
        return R::failure(rkvc::Status::Again);
    uint32_t sz = rd_u32(buf_.data());
    int32_t qi = static_cast<int32_t>(rd_u32(buf_.data() + 4));
    uint32_t flags = rd_u32(buf_.data() + 8);
    if (qi == kQindexDropped) {
        if (sz != 0) {
            parse_error_ = true;
            return R::failure(rkvc::Status::Format);
        }
        last_record_ = kRecSize;
        ++frames_emitted_;
        return R::success(View{true, qi, flags, nullptr, 0});
    }
    if (sz == 0 || sz > kMaxFrameBytes) {
        parse_error_ = true;
        return R::failure(rkvc::Status::Format);
    }
    if (buf_.size() < kRecSize + sz)
        return R::failure(rkvc::Status::Again);
    last_record_ = kRecSize + sz;
    ++frames_emitted_;
    return R::success(View{false, qi, flags, buf_.data() + kRecSize, sz});
}

void Demuxer::consume_record() noexcept {
    if (last_record_ == 0 || last_record_ > buf_.size())
        return;
    buf_.erase(buf_.begin(), buf_.begin() + last_record_);
    last_record_ = 0;
}

}  // namespace mlvc
