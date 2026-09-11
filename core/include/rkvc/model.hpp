// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "rkvc/diag.hpp"
#include "rkvc/result.hpp"

namespace rkvc {

/// 内存态模型（由模型目录装载，或宿主直接构造）。payload kind 约定：
/// "rknn"（NPU 图）/"pmf-gaussian"/"pmf-bitest"/"qptab"/"qppatch"。
struct ModelMeta {
    std::string id;
    std::string family;
    std::string role;
    std::string target;
};

struct ModelPayload {
    std::string kind;
    std::vector<uint8_t> data;
};

struct Model {
    ModelMeta meta;
    std::vector<ModelPayload> payloads;
    const ModelPayload* find(const std::string& kind) const noexcept;
};

/// 按导出器约定扫描模型目录，把每组配套的原生文件读成内存态 Model：
/// MLVCEncoder_<t>.rknn / MLVCDecoder_<t>.rknn / gaussian.bin / bitest.bin /
/// qptab_{encoder,decoder}*.bin / qp_patches/{enc,dec}_qp*.qppatch 整组注册
/// （family "mlvc"），其余 *.rknn 各成单载荷模型（family "sr"）。
/// 同一 bundle 目录即同一次导出，配套完整性由 QPP1 base_crc、qptab 表名
/// 与 rung 集合校验在节点侧兜底。
Result<std::vector<Model>> load_model_dir(const std::string& dir,
                                          Diag* diag = nullptr);

}  // namespace rkvc
