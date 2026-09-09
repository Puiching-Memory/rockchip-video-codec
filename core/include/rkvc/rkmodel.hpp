// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "rkvc/diag.hpp"
#include "rkvc/result.hpp"

namespace rkvc {

// .rkmodel container, new design. Format is coupled to the code release:
// no version field, no backward/forward compatibility window. Old "RKMF"
// files are rejected by magic.
struct ModelMeta {
    std::string id;
    std::string family;
    std::string role;
    std::string target;
};

struct ModelPayload {
    std::string kind;
    uint32_t flags = 0;
    std::vector<uint8_t> data;
};

struct Model {
    ModelMeta meta;
    std::vector<ModelPayload> payloads;
    const ModelPayload* find(const std::string& kind) const noexcept;
};

Result<std::vector<uint8_t>> pack_model(const Model& m, Diag* diag = nullptr);
Result<Model> unpack_model(const uint8_t* data, size_t size,
                           Diag* diag = nullptr);

}  // namespace rkvc
