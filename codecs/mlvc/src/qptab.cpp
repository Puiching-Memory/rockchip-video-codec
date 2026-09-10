// SPDX-License-Identifier: AGPL-3.0-or-later
#include "mlvc/qptab.hpp"

#include <cstring>
#include <utility>

namespace mlvc {

namespace {

uint32_t rd_u32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

constexpr uint64_t kMaxElems = 1u << 20;

}  // namespace

const QpTable* QpTables::find(const std::string& name) const noexcept {
    for (const auto& t : tables)
        if (t.name == name)
            return &t;
    return nullptr;
}

const uint16_t* qptab_row(const QpTable& t, uint32_t q) noexcept {
    if (t.data.empty() || t.cols == 0)
        return nullptr;
    if (q >= t.rows)
        q = t.rows - 1;
    return t.data.data() + static_cast<size_t>(q) * t.cols;
}

rkvc::Result<QpTables> load_qptab(const uint8_t* data, size_t size,
                                  rkvc::Diag* diag) {
    using R = rkvc::Result<QpTables>;
    auto bad = [&](const char* reason) {
        if (diag)
            diag->add("load", "qptab", reason);
        return R::failure(rkvc::Status::Format, diag ? *diag : rkvc::Diag{});
    };
    if (!data || size < 8 || memcmp(data, "QPT1", 4) != 0)
        return bad("bad magic");
    uint32_t count = rd_u32(data + 4);
    if (!count || count > kQptabMaxTables)
        return bad("bad table count");
    QpTables out;
    const uint8_t* cur = data + 8;
    size_t left = size - 8;
    for (uint32_t i = 0; i < count; ++i) {
        QpTable t;
        if (left < 4)
            return bad("truncated name len");
        uint32_t name_len = rd_u32(cur);
        cur += 4;
        left -= 4;
        if (!name_len || name_len > kQptabNameMax || left < name_len + 8)
            return bad("bad name");
        t.name.assign(reinterpret_cast<const char*>(cur), name_len);
        cur += name_len;
        left -= name_len;
        uint32_t rows = rd_u32(cur);
        uint32_t cols = rd_u32(cur + 4);
        cur += 8;
        left -= 8;
        if (!rows || !cols || static_cast<uint64_t>(rows) * cols > kMaxElems)
            return bad("bad dims");
        size_t bytes = static_cast<size_t>(rows) * cols * 2;
        if (left < bytes)
            return bad("truncated rows");
        t.rows = rows;
        t.cols = cols;
        t.data.assign(reinterpret_cast<const uint16_t*>(cur),
                      reinterpret_cast<const uint16_t*>(cur + bytes));
        cur += bytes;
        left -= bytes;
        out.tables.push_back(std::move(t));
    }
    if (left != 0)
        return bad("trailing bytes");
    return R::success(std::move(out));
}

}  // namespace mlvc
