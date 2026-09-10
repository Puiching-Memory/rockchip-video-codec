// SPDX-License-Identifier: AGPL-3.0-or-later
#include "mlvc/rans.hpp"

#include <bit>
#include <cstring>
#include <utility>

namespace mlvc {

namespace {

constexpr uint32_t kByteStateBits = 31;
constexpr uint32_t kByteMaxScale = 30;
constexpr uint32_t kByteLower = 1u << 23;
constexpr uint64_t k64Lower = 1ULL << 31;

uint64_t mul64hi(uint64_t a, uint64_t b) noexcept {
#if defined(__SIZEOF_INT128__)
    return static_cast<uint64_t>(
        (static_cast<__uint128_t>(a) * b) >> 64);
#else
    uint64_t a_lo = static_cast<uint32_t>(a), a_hi = a >> 32;
    uint64_t b_lo = static_cast<uint32_t>(b), b_hi = b >> 32;
    uint64_t lo = a_lo * b_lo;
    uint64_t mid = a_hi * b_lo + a_lo * b_hi + (lo >> 32);
    return a_hi * b_hi + (mid >> 32);
#endif
}

void enc_sym_init_byte(RansEncSym& s, uint32_t start, uint32_t freq,
                       uint32_t scale_bits) noexcept {
    uint32_t scale = 1u << scale_bits;
    s.x_max_hi = freq << (kByteStateBits - scale_bits);
    if (freq > 1) {
        uint32_t shift = 1;
        while (freq > (1u << shift))
            ++shift;
        uint64_t nom = (1ULL << (shift + 32 - 1)) + (freq - 1);
        s.freq_rcp = static_cast<uint32_t>(nom / freq);
        s.freq_rcp_shift = shift - 1 + 32;
        s.bias = start;
    } else {
        s.freq_rcp = 0xFFFFFFFFu;
        s.freq_rcp_shift = 32;
        s.bias = start + scale - 1;
    }
    s.freq_cmpl = scale - freq;
}

void enc_sym_init_64(RansEncSym& s, uint32_t start, uint32_t freq,
                     uint32_t scale_bits) noexcept {
    uint32_t scale = 1u << scale_bits;
    s.x_max_hi = freq << (31 - scale_bits);
    if (freq > 1) {
        uint32_t shift = 1;
        while (freq > (1u << shift))
            ++shift;
        uint64_t x0 = freq - 1;
        uint64_t x1 = 1ULL << (shift + 31);
        uint64_t t1 = x1 / freq;
        x0 += (x1 % freq) << 32;
        uint64_t t0 = x0 / freq;
        uint64_t rcp = t0 + (t1 << 32);
        s.freq_rcp = static_cast<uint32_t>(rcp);
        s.freq_rcp_hi = static_cast<uint32_t>(rcp >> 32);
        s.freq_rcp_shift = shift - 1;
        s.bias = start;
    } else {
        s.freq_rcp = 0xFFFFFFFFu;
        s.freq_rcp_hi = 0xFFFFFFFFu;
        s.freq_rcp_shift = 0;
        s.bias = start + scale - 1;
    }
    s.freq_cmpl = scale - freq;
}

}  // namespace

rkvc::Result<RansCoder> RansCoder::create(RansVariant variant,
                                          std::span<const int32_t> lengths,
                                          std::span<const int32_t> offsets,
                                          std::span<const int32_t> table,
                                          int symbol_bits, int bypass_bits,
                                          rkvc::Diag* diag) {
    using R = rkvc::Result<RansCoder>;
    auto bad = [&](rkvc::Status s, const char* reason) {
        if (diag)
            diag->add("init", "rans", reason);
        return R::failure(s, diag ? *diag : rkvc::Diag{});
    };
    if (lengths.empty() || lengths.size() != offsets.size() || table.empty())
        return bad(rkvc::Status::Invalid, "bad pmf spans");
    // 31 caps both 1u << bypass_bits and freq << (63 - symbol_bits).
    uint32_t max_scale = (variant == RansVariant::Byte) ? kByteMaxScale : 31;
    if (symbol_bits < 2 || static_cast<uint32_t>(symbol_bits) > max_scale ||
        bypass_bits < 2 || static_cast<uint32_t>(bypass_bits) > max_scale)
        return bad(rkvc::Status::Invalid, "bad scale bits");

    RansCoder c;
    c.variant_ = variant;
    c.symbol_bits_ = symbol_bits;
    c.bypass_bits_ = bypass_bits;
    c.bypass_max_ = (1u << bypass_bits) - 1;

    size_t cursor = 0;
    c.dists_.reserve(lengths.size());
    for (size_t i = 0; i < lengths.size(); ++i) {
        int32_t length = lengths[i];
        if (length <= 1 || table.size() - cursor < static_cast<size_t>(length))
            return bad(rkvc::Status::Format, "bad dist length");
        RansDist d;
        d.value_offset = offsets[i];
        d.bypass_sentinel = length - 1;
        d.enc_sym_offset = cursor;
        cursor += static_cast<size_t>(length);
        c.dists_.push_back(d);
    }
    if (cursor != table.size())
        return bad(rkvc::Status::Format, "table size mismatch");

    uint32_t max_freq = 1u << symbol_bits;
    c.enc_syms_.resize(table.size());
    {
        size_t tbl = 0;
        for (size_t d = 0; d < lengths.size(); ++d) {
            int32_t start = 0;
            for (int32_t i = 0; i <= c.dists_[d].bypass_sentinel; ++i) {
                int32_t freq = table[tbl];
                if (!(freq > 0 && freq <= static_cast<int32_t>(max_freq - start)))
                    return bad(rkvc::Status::Format, "bad freq");
                if (variant == RansVariant::Byte)
                    enc_sym_init_byte(c.enc_syms_[tbl], start, freq,
                                      symbol_bits);
                else
                    enc_sym_init_64(c.enc_syms_[tbl], start, freq,
                                    symbol_bits);
                start += freq;
                ++tbl;
            }
        }
    }

    c.cdf_table_.assign(table.size() + lengths.size(), 0);
    {
        size_t tbl = 0;
        for (size_t d = 0; d < lengths.size(); ++d) {
            RansDist& desc = c.dists_[d];
            desc.dec_cdf_offset = tbl + d;
            int32_t start = 0;
            for (int32_t i = 0; i <= desc.bypass_sentinel; ++i, ++tbl) {
                int32_t freq = table[tbl];
                if (!(freq > 0 && freq <= static_cast<int32_t>(max_freq - start)))
                    return bad(rkvc::Status::Format, "bad freq");
                c.cdf_table_[tbl + d] = static_cast<uint32_t>(start);
                start += freq;
            }
            c.cdf_table_[tbl + d] = static_cast<uint32_t>(start);
        }
    }
    return R::success(std::move(c));
}

RansEncoder::RansEncoder(RansVariant variant, size_t initial_capacity)
    : variant_(variant) {
    static_assert(std::endian::native == std::endian::little,
                  "Rans64 unit writes assume little-endian");
    if (initial_capacity < 512)
        initial_capacity = 512;
    buf_.assign(initial_capacity, 0);
    ptr_ = initial_capacity;
    state_ = (variant == RansVariant::Byte) ? kByteLower : k64Lower;
}

bool RansEncoder::grow() {
    if (oom_ || buf_.empty())
        return false;
    size_t cap = buf_.size();
    size_t ncap = cap * 2;
    if (ncap < cap + 65536)
        ncap = cap + 65536;
    if (ncap <= cap) {
        oom_ = true;
        return false;
    }
    std::vector<uint8_t> nbuf(ncap);
    size_t content = cap - ptr_;
    memcpy(nbuf.data() + ncap - content, buf_.data() + ptr_, content);
    buf_.swap(nbuf);
    ptr_ = ncap - content;
    return true;
}

void RansEncoder::write_byte(uint8_t v) {
    if (oom_ || buf_.empty())
        return;
    if (ptr_ == 0 && !grow())
        return;
    if (ptr_ == 0)
        return;
    buf_[--ptr_] = v;
}

void RansEncoder::write_unit32(uint32_t v) {
    if (oom_ || buf_.empty())
        return;
    if (ptr_ < 4 && !grow())
        return;
    if (ptr_ < 4)
        return;
    ptr_ -= 4;
    memcpy(buf_.data() + ptr_, &v, 4);
}

void RansEncoder::renorm_byte(uint32_t x_max) {
    uint32_t x = static_cast<uint32_t>(state_);
    while (x >= x_max) {
        write_byte(static_cast<uint8_t>(x));
        x >>= 8;
    }
    state_ = x;
}

void RansEncoder::renorm_64(uint64_t x_max) {
    uint64_t x = state_;
    while (x >= x_max) {
        write_unit32(static_cast<uint32_t>(x));
        x >>= 32;
        break;
    }
    state_ = x;
}

void RansEncoder::put_sym_byte(const RansEncSym& s) {
    renorm_byte(s.x_max_hi);
    uint32_t x = static_cast<uint32_t>(state_);
    uint32_t q =
        static_cast<uint32_t>((static_cast<uint64_t>(x) * s.freq_rcp) >>
                              s.freq_rcp_shift);
    x += q * s.freq_cmpl + s.bias;
    state_ = x;
}

void RansEncoder::put_sym_64(const RansEncSym& s) {
    uint64_t x_max = static_cast<uint64_t>(s.x_max_hi) << 32;
    renorm_64(x_max);
    uint64_t x = state_;
    uint64_t rcp =
        (static_cast<uint64_t>(s.freq_rcp_hi) << 32) | s.freq_rcp;
    uint64_t q = mul64hi(x, rcp) >> s.freq_rcp_shift;
    x += q * s.freq_cmpl + s.bias;
    state_ = x;
}

void RansEncoder::put_raw(uint32_t start, uint32_t freq,
                          uint32_t scale_bits) {
    if (variant_ == RansVariant::Byte) {
        uint32_t x_max = freq << (kByteStateBits - scale_bits);
        renorm_byte(x_max);
        uint32_t x = static_cast<uint32_t>(state_);
        x = ((x / freq) << scale_bits) + start + (x % freq);
        state_ = x;
    } else {
        uint64_t x_max =
            static_cast<uint64_t>(freq) << (63 - scale_bits);
        renorm_64(x_max);
        uint64_t x = state_;
        x = ((x / freq) << scale_bits) + start + (x % freq);
        state_ = x;
    }
}

void RansEncoder::bypass(const RansCoder& c, uint32_t v) {
    uint32_t buf[16];
    int n = 0;
    while (v != 0) {
        buf[n++] = v & c.bypass_max();
        v >>= c.bypass_bits();
    }
    int total = n;
    while (n > 0)
        put_raw(buf[--n], 1, c.bypass_bits());
    uint32_t count = static_cast<uint32_t>(total);
    int prefix = 0;
    while (count >= c.bypass_max()) {
        count -= c.bypass_max();
        ++prefix;
    }
    put_raw(count, 1, c.bypass_bits());
    while (prefix > 0) {
        put_raw(c.bypass_max(), 1, c.bypass_bits());
        --prefix;
    }
}

rkvc::Status RansEncoder::encode(const RansCoder& coder,
                                 std::span<const int32_t> indices,
                                 std::span<const int32_t> values) {
    if (indices.size() != values.size())
        return rkvc::Status::Invalid;
    if (coder.dists().empty() || coder.enc_syms().empty())
        return rkvc::Status::Invalid;
    if (flushed_ || oom_ || buf_.empty())
        return rkvc::Status::Invalid;
    for (size_t k = values.size(); k > 0; --k) {
        int32_t index = indices[k - 1];
        if (index < 0)
            continue;
        if (static_cast<size_t>(index) >= coder.dists().size())
            index = static_cast<int32_t>(coder.dists().size()) - 1;
        const RansDist& desc = coder.dists()[index];
        int32_t value = values[k - 1] + desc.value_offset;
        if (value < 0 || value >= desc.bypass_sentinel) {
            uint32_t bv = (value < 0)
                              ? 2u * static_cast<uint32_t>(-value) - 1
                              : 2u * static_cast<uint32_t>(value -
                                                           desc.bypass_sentinel);
            bypass(coder, bv);
            value = desc.bypass_sentinel;
        }
        size_t sym = desc.enc_sym_offset + static_cast<size_t>(value);
        if (variant_ == RansVariant::Byte)
            put_sym_byte(coder.enc_syms()[sym]);
        else
            put_sym_64(coder.enc_syms()[sym]);
        if (oom_)
            return rkvc::Status::Nomem;
    }
    return rkvc::Status::Ok;
}

rkvc::Status RansEncoder::flush(const uint8_t** out, size_t* out_size) {
    if (flushed_ || oom_ || buf_.empty()) {
        if (out_size)
            *out_size = 0;
        return rkvc::Status::Invalid;
    }
    if (variant_ == RansVariant::Byte) {
        uint32_t x = static_cast<uint32_t>(state_);
        for (int i = 3; i > 0; --i)
            write_byte(static_cast<uint8_t>(x >> (i * 8)));
        write_byte(static_cast<uint8_t>(x));
    } else {
        uint64_t x = state_;
        write_unit32(static_cast<uint32_t>(x >> 32));
        write_unit32(static_cast<uint32_t>(x));
    }
    if (oom_) {
        if (out_size)
            *out_size = 0;
        return rkvc::Status::Nomem;
    }
    flushed_ = true;
    if (out)
        *out = buf_.data() + ptr_;
    if (out_size)
        *out_size = buf_.size() - ptr_;
    return rkvc::Status::Ok;
}

void RansEncoder::reset() noexcept {
    ptr_ = buf_.size();
    flushed_ = false;
    state_ = (variant_ == RansVariant::Byte) ? kByteLower : k64Lower;
}

RansDecoder::RansDecoder(RansVariant variant) : variant_(variant) {}

rkvc::Status RansDecoder::open(std::span<const uint8_t> data) {
    if (!data.data() && !data.empty())
        return rkvc::Status::Invalid;
    size_t unit = (variant_ == RansVariant::Byte) ? 1 : 4;
    if (data.size() % unit != 0)
        return rkvc::Status::Format;
    data_ = data.data();
    size_ = data.size();
    pos_ = 0;
    opened_ = false;
    if (!init_state())
        return rkvc::Status::Format;
    opened_ = true;
    return rkvc::Status::Ok;
}

bool RansDecoder::read_byte(uint8_t& out) noexcept {
    if (pos_ >= size_)
        return false;
    out = data_[pos_++];
    return true;
}

bool RansDecoder::read_unit32(uint32_t& out) noexcept {
    if (size_ - pos_ < 4)
        return false;
    memcpy(&out, data_ + pos_, 4);
    pos_ += 4;
    return true;
}

bool RansDecoder::init_state() {
    if (variant_ == RansVariant::Byte) {
        uint8_t u = 0;
        if (!read_byte(u))
            return false;
        uint32_t x = u;
        for (int i = 1; i < 4; ++i) {
            if (!read_byte(u))
                return false;
            x += static_cast<uint32_t>(u) << (i * 8);
        }
        if (x < kByteLower)
            return false;
        state_ = x;
        return true;
    }
    uint32_t u = 0;
    if (!read_unit32(u))
        return false;
    uint64_t x = u;
    for (int i = 1; i < 2; ++i) {
        if (!read_unit32(u))
            return false;
        x += static_cast<uint64_t>(u) << (i * 32);
    }
    if (x < k64Lower)
        return false;
    state_ = x;
    return true;
}

uint32_t RansDecoder::get(uint32_t scale_bits) const noexcept {
    return static_cast<uint32_t>(state_) & ((1u << scale_bits) - 1);
}

bool RansDecoder::advance(uint32_t start, uint32_t freq,
                          uint32_t scale_bits) noexcept {
    uint32_t scale = 1u << scale_bits;
    if (variant_ == RansVariant::Byte) {
        uint32_t x = static_cast<uint32_t>(state_);
        uint32_t value = x & (scale - 1);
        x = freq * (x >> scale_bits) + value - start;
        while (x < kByteLower) {
            uint8_t u = 0;
            if (!read_byte(u))
                return false;
            x = (x << 8) + u;
        }
        state_ = x;
        return true;
    }
    uint64_t x = state_;
    uint32_t value = static_cast<uint32_t>(x & (scale - 1));
    x = static_cast<uint64_t>(freq) * (x >> scale_bits) + value - start;
    while (x < k64Lower) {
        uint32_t u = 0;
        if (!read_unit32(u))
            return false;
        x = (x << 32) + u;
        break;
    }
    state_ = x;
    return true;
}

bool RansDecoder::bypass(const RansCoder& c, uint32_t& out) noexcept {
    uint32_t value = get(c.bypass_bits());
    if (!advance(value, 1, c.bypass_bits()))
        return false;
    // Prefix symbols sum to a digit count; each digit is bypass_bits wide and
    // the assembled value must fit the 32-bit result, so bound it in bits.
    const uint32_t max_digits = 32u / static_cast<uint32_t>(c.bypass_bits());
    uint32_t count = value;
    if (count > max_digits)
        return false;
    while (value == c.bypass_max()) {
        value = get(c.bypass_bits());
        if (!advance(value, 1, c.bypass_bits()))
            return false;
        count += value;
        if (count > max_digits)
            return false;
    }
    uint32_t encoded = 0;
    uint32_t total_bits = count * c.bypass_bits();
    for (uint32_t shift = 0; shift < total_bits; shift += c.bypass_bits()) {
        value = get(c.bypass_bits());
        if (!advance(value, 1, c.bypass_bits()))
            return false;
        encoded |= value << shift;
    }
    out = encoded;
    return true;
}

rkvc::Status RansDecoder::decode(const RansCoder& coder,
                                 std::span<int32_t> values,
                                 std::span<const int32_t> indices) {
    if (values.size() != indices.size())
        return rkvc::Status::Invalid;
    if (!opened_ || coder.dists().empty() || coder.cdf_table().empty())
        return rkvc::Status::Invalid;
    for (size_t i = 0; i < values.size(); ++i) {
        int32_t index = indices[i];
        if (index < 0) {
            values[i] = 0;
            continue;
        }
        if (static_cast<size_t>(index) >= coder.dists().size())
            index = static_cast<int32_t>(coder.dists().size()) - 1;
        const RansDist& desc = coder.dists()[index];
        uint32_t cum = get(coder.symbol_bits());
        const uint32_t* base =
            coder.cdf_table().data() + desc.dec_cdf_offset;
        int32_t lo = 0;
        int32_t hi = desc.bypass_sentinel + 1;
        while (lo < hi) {
            int32_t mid = lo + (hi - lo) / 2;
            if (base[mid + 1] <= cum)
                lo = mid + 1;
            else
                hi = mid;
        }
        int32_t symbol = lo;
        // lo can land on the search upper bound (sentinel + 1); its range
        // would read past this distribution's CDF segment.
        if (symbol > desc.bypass_sentinel)
            return rkvc::Status::Format;
        uint32_t start = base[symbol];
        uint32_t freq = base[symbol + 1] - base[symbol];
        if (!advance(start, freq, coder.symbol_bits()))
            return rkvc::Status::Format;
        if (symbol == desc.bypass_sentinel) {
            uint32_t bv = 0;
            if (!bypass(coder, bv))
                return rkvc::Status::Format;
            if (bv & 1)
                symbol = -static_cast<int32_t>(bv >> 1) - 1;
            else
                symbol =
                    static_cast<int32_t>(bv >> 1) + desc.bypass_sentinel;
        }
        values[i] = symbol - desc.value_offset;
    }
    return rkvc::Status::Ok;
}

bool RansDecoder::check_eof() const noexcept {
    if (!opened_ || pos_ != size_)
        return false;
    if (variant_ == RansVariant::Byte)
        return static_cast<uint32_t>(state_) == kByteLower;
    return state_ == k64Lower;
}

rkvc::Result<std::vector<uint8_t>> rans_encode(
    const RansCoder& coder, std::span<const int32_t> indices,
    std::span<const int32_t> values, rkvc::Diag* diag) {
    using R = rkvc::Result<std::vector<uint8_t>>;
    RansVariant v = coder.variant();
    RansEncoder enc(v, 65536);
    rkvc::Status st = enc.encode(coder, indices, values);
    if (st != rkvc::Status::Ok) {
        if (diag)
            diag->add("encode", "rans", "encode failed");
        return R::failure(st, diag ? *diag : rkvc::Diag{});
    }
    const uint8_t* out = nullptr;
    size_t size = 0;
    st = enc.flush(&out, &size);
    if (st != rkvc::Status::Ok || !out) {
        if (diag)
            diag->add("encode", "rans", "flush failed");
        return R::failure(st, diag ? *diag : rkvc::Diag{});
    }
    return R::success(std::vector<uint8_t>(out, out + size));
}

rkvc::Status rans_decode(const RansCoder& coder, std::span<int32_t> values,
                         std::span<const int32_t> indices,
                         std::span<const uint8_t> data) {
    RansDecoder dec(coder.variant());
    rkvc::Status st = dec.open(data);
    if (st != rkvc::Status::Ok)
        return st;
    st = dec.decode(coder, values, indices);
    if (st != rkvc::Status::Ok)
        return st;
    return dec.check_eof() ? rkvc::Status::Ok : rkvc::Status::Format;
}

}  // namespace mlvc
