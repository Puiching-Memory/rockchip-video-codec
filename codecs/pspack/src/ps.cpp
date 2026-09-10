// SPDX-License-Identifier: AGPL-3.0-or-later
#include "pspack/ps.hpp"

namespace pspack {
namespace {

constexpr size_t kMaxBuffered = kMaxPayload + (1u << 20);
constexpr size_t kPesChunk = 60000;  // max ES bytes per PES packet.
constexpr size_t kNpos = static_cast<size_t>(-1);

class BitWriter {
  public:
    void put(uint32_t v, int n) noexcept {
        for (int i = n - 1; i >= 0; --i) {
            cur_ = static_cast<uint8_t>((cur_ << 1) | ((v >> i) & 1u));
            if (++bits_ == 8) {
                out_->push_back(cur_);
                cur_ = 0;
                bits_ = 0;
            }
        }
    }
    std::vector<uint8_t>* out_ = nullptr;
    uint8_t cur_ = 0;
    int bits_ = 0;
};

void wr_start(std::vector<uint8_t>& o, uint8_t id) {
    o.push_back(0);
    o.push_back(0);
    o.push_back(1);
    o.push_back(id);
}

void wr_u16(std::vector<uint8_t>& o, uint16_t v) {
    o.push_back(static_cast<uint8_t>(v >> 8));
    o.push_back(static_cast<uint8_t>(v));
}

void wr_u32be(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back(static_cast<uint8_t>(v >> 24));
    o.push_back(static_cast<uint8_t>(v >> 16));
    o.push_back(static_cast<uint8_t>(v >> 8));
    o.push_back(static_cast<uint8_t>(v));
}

void wr_pts(std::vector<uint8_t>& o, int64_t pts) {
    uint64_t p = (pts < 0 ? 0u : static_cast<uint64_t>(pts)) & 0x1FFFFFFFFULL;
    BitWriter w;
    w.out_ = &o;
    w.put(0x2, 4);
    w.put(static_cast<uint32_t>(p >> 30), 3);
    w.put(1, 1);
    w.put(static_cast<uint32_t>(p >> 15), 15);
    w.put(1, 1);
    w.put(static_cast<uint32_t>(p), 15);
    w.put(1, 1);
}

int64_t rd_pts(const uint8_t* p) noexcept {
    uint64_t v = (static_cast<uint64_t>(p[0] >> 1) & 7) << 30;
    v |= static_cast<uint64_t>((static_cast<uint16_t>(p[1]) << 8 | p[2]) >> 1) << 15;
    v |= static_cast<uint64_t>(static_cast<uint16_t>(p[3]) << 8 | p[4]) >> 1;
    return static_cast<int64_t>(v);
}

uint16_t rd_u16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) << 8 | p[1]);
}

uint32_t crc32(const uint8_t* d, size_t n) noexcept {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= d[i];
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return c ^ 0xFFFFFFFFu;
}

uint8_t stream_type(VideoCodec c) noexcept {
    return c == VideoCodec::Avc ? 0x1B : 0x24;
}

template <typename F>
void for_each_nal(const uint8_t* d, size_t n, F f) noexcept {
    size_t i = 0;
    while (i + 3 < n) {
        size_t hdr = 0;
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            hdr = 3;
        } else if (i + 4 < n && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 &&
                   d[i + 3] == 1) {
            hdr = 4;
        } else {
            ++i;
            continue;
        }
        f(d[i + hdr]);
        i += hdr;
    }
}

// HEVC first: low 5 bits of an HEVC header byte can alias AVC types.
VideoCodec sniff(const uint8_t* d, size_t n) noexcept {
    bool avc = false;
    bool hevc = false;
    for_each_nal(d, n, [&](uint8_t b) {
        uint8_t t6 = static_cast<uint8_t>((b >> 1) & 0x3F);
        if (t6 == 32 || t6 == 19 || t6 == 20)
            hevc = true;
        uint8_t t5 = static_cast<uint8_t>(b & 0x1F);
        if (t5 == 7 || t5 == 5)
            avc = true;
    });
    if (hevc)
        return VideoCodec::Hevc;
    return avc ? VideoCodec::Avc : VideoCodec::Hevc;
}

size_t find_sc(const uint8_t* d, size_t from, size_t n) noexcept {
    for (size_t i = from; i + 2 < n; ++i) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1)
            return i;
    }
    return kNpos;
}

}  // namespace

bool is_keyframe(VideoCodec codec, const uint8_t* data, size_t size) noexcept {
    if (!data || !size)
        return false;
    bool key = false;
    for_each_nal(data, size, [&](uint8_t b) {
        if (codec == VideoCodec::Avc) {
            uint8_t t = static_cast<uint8_t>(b & 0x1F);
            if (t == 5 || t == 7)
                key = true;
        } else {
            uint8_t t = static_cast<uint8_t>((b >> 1) & 0x3F);
            if (t == 32 || t == 19 || t == 20)
                key = true;
        }
    });
    return key;
}

rkvc::Result<std::vector<uint8_t>> mux_frame(VideoCodec codec, const uint8_t* annexb,
                                             size_t size, int64_t pts90, bool keyframe,
                                             const MuxOptions& opts) {
    using R = rkvc::Result<std::vector<uint8_t>>;
    if (size && !annexb)
        return R::failure(rkvc::Status::Invalid);
    if (!size)
        return R::failure(rkvc::Status::Invalid);
    if (size > kMaxPayload)
        return R::failure(rkvc::Status::Format);
    std::vector<uint8_t> out;
    out.reserve(64 + size);
    uint64_t scr = (pts90 < 0 ? 0u : static_cast<uint64_t>(pts90)) & 0x1FFFFFFFFULL;
    uint32_t rate = opts.mux_rate_50bs & 0x3FFFFFu;
    // One AU per call, fragmented into explicit-length PES packets; the
    // first carries PTS and data_alignment=1 so the demuxer can reassemble.
    size_t off = 0;
    bool first = true;
    while (off < size) {
        size_t chunk = size - off > kPesChunk ? kPesChunk : size - off;
        wr_start(out, 0xBA);
        {
            BitWriter w;
            w.out_ = &out;
            w.put(1, 2);
            w.put(static_cast<uint32_t>(scr >> 30), 3);
            w.put(1, 1);
            w.put(static_cast<uint32_t>(scr >> 15), 15);
            w.put(1, 1);
            w.put(static_cast<uint32_t>(scr), 15);
            w.put(1, 1);
            w.put(0, 9);
            w.put(1, 1);
            w.put(rate, 22);
            w.put(1, 1);
            w.put(1, 1);
            w.put(0x1F, 5);
            w.put(0, 3);
        }
        if (keyframe && first) {
            wr_start(out, 0xBB);
            wr_u16(out, 9);
            BitWriter w;
            w.out_ = &out;
            w.put(1, 1);
            w.put(rate, 22);
            w.put(1, 1);
            w.put(0, 6);
            w.put(0, 1);
            w.put(0, 1);
            w.put(0, 1);
            w.put(0, 1);
            w.put(1, 1);
            w.put(1, 5);
            w.put(0, 1);
            w.put(0x7F, 7);
            w.put(kVideoStreamId, 8);
            w.put(3, 2);
            w.put(1, 1);
            w.put(230, 13);
            wr_start(out, 0xBC);
            const uint8_t map[] = {0xC0, 0xFF, 0x00, 0x00, 0x00, 0x04,
                                   stream_type(codec), kVideoStreamId, 0x00, 0x00};
            wr_u16(out, static_cast<uint16_t>(sizeof(map) + 4));
            out.insert(out.end(), map, map + sizeof(map));
            wr_u32be(out, crc32(map, sizeof(map)));
        }
        wr_start(out, kVideoStreamId);
        if (first) {
            wr_u16(out, static_cast<uint16_t>(3 + 5 + chunk));
            out.push_back(0x84);  // data_alignment = 1: AU start.
            out.push_back(0x80);
            out.push_back(5);
            wr_pts(out, pts90);
        } else {
            wr_u16(out, static_cast<uint16_t>(3 + chunk));
            out.push_back(0x80);
            out.push_back(0x00);
            out.push_back(0);
        }
        out.insert(out.end(), annexb + off, annexb + off + chunk);
        off += chunk;
        first = false;
    }
    return R::success(std::move(out));
}

rkvc::Status Demux::append(const uint8_t* data, size_t size) {
    if (size && !data)
        return rkvc::Status::Format;
    if (!size)
        return rkvc::Status::Ok;
    if (buf_.size() + size > kMaxBuffered)
        return rkvc::Status::Format;
    buf_.insert(buf_.end(), data, data + size);
    return rkvc::Status::Ok;
}

rkvc::Result<Frame> Demux::next() {
    using R = rkvc::Result<Frame>;
    if (cursor_ > 0) {
        buf_.erase(buf_.begin(), buf_.begin() + cursor_);
        cursor_ = 0;
    }
    const size_t n = buf_.size();
    const uint8_t* d = buf_.data();
    size_t pos = 0;
    for (;;) {
        size_t sc = find_sc(d, pos, n);
        if (sc == kNpos) {
            cursor_ = n > 3 ? n - 3 : 0;  // keep a partial start code.
            return R::failure(rkvc::Status::Again);
        }
        if (sc + 4 > n) {
            cursor_ = sc;
            return R::failure(rkvc::Status::Again);
        }
        uint8_t id = d[sc + 3];
        if (id == 0xBA) {  // pack header is 14 bytes total.
            if (sc + 14 > n) {
                cursor_ = sc;
                return R::failure(rkvc::Status::Again);
            }
            pos = sc + 14;
            continue;
        }
        if (id == 0xBB || id == 0xBC) {  // length-prefixed system header / PSM.
            if (sc + 6 > n) {
                cursor_ = sc;
                return R::failure(rkvc::Status::Again);
            }
            size_t end = sc + 6 + rd_u16(d + sc + 4);
            if (end > n) {
                cursor_ = sc;
                return R::failure(rkvc::Status::Again);
            }
            if (id == 0xBC && end >= sc + 10) {
                // Parse the ES map, ignore the trailing CRC.
                size_t p = sc + 6;
                size_t map_end = end - 4;
                // program_stream_info_length follows the version and marker
                // bytes; the ES map starts past those descriptors.
                size_t ps_info = rd_u16(d + p + 2) & 0x7FFF;
                size_t q = p + 4 + ps_info;
                if (q + 2 <= map_end) {
                    size_t es_len = rd_u16(d + q);
                    q += 2;
                    size_t es_end = q + es_len;
                    if (es_end <= map_end) {
                        while (q + 4 <= es_end) {
                            uint8_t type = d[q];
                            uint8_t sid = d[q + 1];
                            uint16_t ilen = rd_u16(d + q + 2);
                            if (sid >= 0xE0 && sid <= 0xEF) {
                                if (type == 0x1B) {
                                    psm_codec_ = VideoCodec::Avc;
                                    have_psm_ = true;
                                } else if (type == 0x24) {
                                    psm_codec_ = VideoCodec::Hevc;
                                    have_psm_ = true;
                                }
                            }
                            q += 4 + ilen;
                        }
                    }
                }
            }
            pos = end;
            continue;
        }
        if (id == 0xB7 || id == 0xB9) {
            pos = sc + 4;
            continue;
        }
        bool is_pes = (id >= 0xBD && id <= 0xBF) || (id >= 0xC0 && id <= 0xEF);
        if (!is_pes) {  // unknown system code, resync past it.
            pos = sc + 4;
            continue;
        }
        if (sc + 9 > n) {
            cursor_ = sc;
            return R::failure(rkvc::Status::Again);
        }
        uint16_t len = rd_u16(d + sc + 4);
        uint8_t flags2 = d[sc + 7];
        uint8_t hlen = d[sc + 8];
        size_t pay = sc + 9 + hlen;
        if (pay > n) {
            cursor_ = sc;
            return R::failure(rkvc::Status::Again);
        }
        int64_t pts = 0;
        // PTS is the first 5-byte field for both '10' (PTS) and
        // '11' (PTS+DTS); only '00'/'01' carry no presentation time.
        if (flags2 & 0x80) {
            if (sc + 14 > n) {
                cursor_ = sc;
                return R::failure(rkvc::Status::Again);
            }
            pts = rd_pts(d + sc + 9);
        }
        size_t plen;
        size_t end;
        if (len == 0) {
            if (id >= 0xE0)
                return R::failure(rkvc::Status::Format);  // pairs with mux framing.
            size_t nx = find_sc(d, pay, n);
            if (nx == kNpos) {
                cursor_ = sc;
                return R::failure(rkvc::Status::Again);
            }
            pos = nx;
            continue;
        }
        if (len < 3u + hlen)
            return R::failure(rkvc::Status::Format);
        plen = len - 3u - hlen;
        end = pay + plen;
        if (end > n) {
            cursor_ = sc;
            return R::failure(rkvc::Status::Again);
        }
        if (id < 0xE0 || !plen) {  // non-video PES or empty, skip it.
            pos = end;
            continue;
        }
        if ((d[sc + 6] & 0x04) != 0) {  // data_alignment: new AU.
            rkvc::Result<Frame> done = emit_au();
            au_.assign(d + pay, d + end);
            au_pts_ = (flags2 & 0x80) ? pts : 0;
            au_open_ = true;
            cursor_ = end;
            if (done)
                return done;
            pos = end;
            continue;
        }
        if (!au_open_) {  // orphan continuation before any AU start.
            pos = end;
            continue;
        }
        if (au_.size() + plen > kMaxPayload)
            return R::failure(rkvc::Status::Format);
        au_.insert(au_.end(), d + pay, d + end);
        cursor_ = end;
        pos = end;
    }
}

rkvc::Result<Frame> Demux::emit_au() {
    using R = rkvc::Result<Frame>;
    if (!au_open_ || au_.empty())
        return R::failure(rkvc::Status::Eof);
    Frame f;
    f.annexb = au_;
    f.pts90 = au_pts_;
    f.codec = have_psm_ ? psm_codec_ : sniff(au_.data(), au_.size());
    f.keyframe = is_keyframe(f.codec, au_.data(), au_.size());
    au_.clear();
    au_open_ = false;
    return R::success(std::move(f));
}

rkvc::Result<Frame> Demux::flush() {
    return emit_au();
}

}  // namespace pspack
