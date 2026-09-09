// SPDX-License-Identifier: AGPL-3.0-or-later
#include "rkvc/rkmodel.hpp"

#include <cstring>

#include "rkvc/sha256.hpp"

namespace rkvc {

namespace {

constexpr size_t kHeaderSize = 128;
constexpr size_t kEntrySize = 88;
constexpr size_t kMaxPayloads = 16;
constexpr size_t kMaxFile = 1u << 29;  // 512 MiB upper bound
// Format generation 1 by policy: all format versions stay at 1 and old
// files are regenerated, never adapted.
constexpr char kMagic[8] = {'R', 'K', 'M', 'D', 'L', '1', 0, 0};
constexpr size_t kIdLen = 32;
constexpr size_t kWordLen = 16;
constexpr size_t kKindLen = 32;

void put32le(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

void put64le(uint8_t* p, uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i)
        p[i] = static_cast<uint8_t>(v >> (i * 8));
}

uint32_t get32le(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t get64le(const uint8_t* p) noexcept {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<uint64_t>(p[i]) << (i * 8);
    return v;
}

bool put_str(uint8_t* dst, size_t cap, const std::string& s) noexcept {
    if (s.size() >= cap)
        return false;
    memcpy(dst, s.data(), s.size());
    memset(dst + s.size(), 0, cap - s.size());
    return true;
}

bool get_str(const uint8_t* src, size_t cap, std::string& out) {
    size_t n = 0;
    while (n < cap && src[n])
        ++n;
    if (n == cap)
        return false;
    out.assign(reinterpret_cast<const char*>(src), n);
    return true;
}

}  // namespace

const ModelPayload* Model::find(const std::string& kind) const noexcept {
    for (const auto& p : payloads)
        if (p.kind == kind)
            return &p;
    return nullptr;
}

Result<std::vector<uint8_t>> pack_model(const Model& m, Diag* diag) {
    auto reject = [&](Status s, const char* reason) {
        if (diag)
            diag->add("pack", "rkmodel", reason);
        return Result<std::vector<uint8_t>>::failure(
            s, diag ? *diag : Diag{});
    };
    if (m.payloads.size() > kMaxPayloads)
        return reject(Status::Invalid, "too many payloads");
    if (m.meta.id.empty() || m.meta.family.empty() || m.meta.role.empty())
        return reject(Status::Invalid, "id/family/role required");
    for (size_t i = 0; i < m.payloads.size(); ++i) {
        if (m.payloads[i].kind.empty())
            return reject(Status::Invalid, "empty payload kind");
        for (size_t j = 0; j < i; ++j)
            if (m.payloads[j].kind == m.payloads[i].kind)
                return reject(Status::Invalid, "duplicate payload kind");
    }
    size_t total = kHeaderSize + kEntrySize * m.payloads.size();
    for (const auto& p : m.payloads) {
        total += p.data.size();
        if (total > kMaxFile)
            return reject(Status::Invalid, "model too large");
    }
    std::vector<uint8_t> out(total);
    memcpy(out.data(), kMagic, 8);
    put32le(out.data() + 8, static_cast<uint32_t>(kHeaderSize));
    put32le(out.data() + 12,
            static_cast<uint32_t>(m.payloads.size()));
    if (!put_str(out.data() + 16, kIdLen, m.meta.id) ||
        !put_str(out.data() + 48, kWordLen, m.meta.family) ||
        !put_str(out.data() + 64, kWordLen, m.meta.role) ||
        !put_str(out.data() + 80, kWordLen, m.meta.target))
        return reject(Status::Invalid, "meta string too long");
    memset(out.data() + 96, 0, 32);
    size_t off = kHeaderSize + kEntrySize * m.payloads.size();
    for (size_t i = 0; i < m.payloads.size(); ++i) {
        const auto& p = m.payloads[i];
        uint8_t* e = out.data() + kHeaderSize + i * kEntrySize;
        if (!put_str(e, kKindLen, p.kind))
            return reject(Status::Invalid, "kind string too long");
        put32le(e + 32, p.flags);
        put32le(e + 36, 0);
        put64le(e + 40, static_cast<uint64_t>(off));
        put64le(e + 48, static_cast<uint64_t>(p.data.size()));
        auto h = sha256(p.data.data(), p.data.size());
        memcpy(e + 56, h.data(), 32);
        if (!p.data.empty())
            memcpy(out.data() + off, p.data.data(), p.data.size());
        off += p.data.size();
    }
    return Result<std::vector<uint8_t>>::success(std::move(out));
}

Result<Model> unpack_model(const uint8_t* data, size_t size, Diag* diag) {
    auto reject = [&](Status s, const char* reason) {
        if (diag)
            diag->add("unpack", "rkmodel", reason);
        return Result<Model>::failure(s, diag ? *diag : Diag{});
    };
    if (!data || size < kHeaderSize)
        return reject(Status::Format, "truncated header");
    if (memcmp(data, kMagic, 8) != 0)
        return reject(Status::Format, "bad magic");
    if (get32le(data + 8) != kHeaderSize)
        return reject(Status::Format, "bad header size");
    uint32_t count = get32le(data + 12);
    if (count > kMaxPayloads)
        return reject(Status::Format, "payload count overflow");
    for (size_t i = 96; i < 128; ++i)
        if (data[i] != 0)
            return reject(Status::Format, "reserved nonzero");
    if (size < kHeaderSize + kEntrySize * count)
        return reject(Status::Format, "truncated entries");
    if (size > kMaxFile)
        return reject(Status::Format, "file too large");
    Model m;
    if (!get_str(data + 16, kIdLen, m.meta.id) ||
        !get_str(data + 48, kWordLen, m.meta.family) ||
        !get_str(data + 64, kWordLen, m.meta.role) ||
        !get_str(data + 80, kWordLen, m.meta.target))
        return reject(Status::Format, "bad meta string");
    uint64_t prev_end = kHeaderSize + kEntrySize * count;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = data + kHeaderSize + i * kEntrySize;
        ModelPayload p;
        if (!get_str(e, kKindLen, p.kind) || p.kind.empty())
            return reject(Status::Format, "bad kind string");
        // Repeated kinds are tolerated here (pack stays strict): patch
        // payloads (one per QP rung) are gathered by full-table scan,
        // while find() keeps returning the first match.
        if (get32le(e + 36) != 0)
            return reject(Status::Format, "entry reserved nonzero");
        p.flags = get32le(e + 32);
        uint64_t off = get64le(e + 40);
        uint64_t len = get64le(e + 48);
        if (len > kMaxFile || off > size || len > size - off)
            return reject(Status::Format, "payload out of bounds");
        if (off < prev_end)
            return reject(Status::Format, "payload overlap");
        prev_end = off + len;
        p.data.assign(data + off, data + off + len);
        auto h = sha256(p.data.data(), p.data.size());
        if (memcmp(e + 56, h.data(), 32) != 0)
            return reject(Status::Integrity, "payload hash mismatch");
        m.payloads.push_back(std::move(p));
    }
    return Result<Model>::success(std::move(m));
}

}  // namespace rkvc
