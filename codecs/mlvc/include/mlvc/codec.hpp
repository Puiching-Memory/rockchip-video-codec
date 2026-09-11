// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "mlvc/container.hpp"
#include "mlvc/npu.hpp"
#include "mlvc/pmf.hpp"
#include "mlvc/qptab.hpp"
#include "mlvc/rans.hpp"
#include "mlvc/ratectl.hpp"
#include "rkvc/diag.hpp"
#include "rkvc/node.hpp"
#include "rkvc/registry.hpp"
#include "rkvc/result.hpp"
#include "rkvc/model.hpp"

namespace mlvc {

// 模型 payload 的 kind 字符串（目录装载与节点约定一致）。
inline constexpr const char* kKindRknn = "rknn";
inline constexpr const char* kKindPmfGaussian = "pmf-gaussian";
inline constexpr const char* kKindPmfBitest = "pmf-bitest";
inline constexpr const char* kKindQptab = "qptab";
inline constexpr const char* kKindQppatch = "qppatch";

inline constexpr size_t kMaxRungs = 8;
inline constexpr size_t kMaxQrows = 4;
inline constexpr uint32_t kMlvcFps = 30;
inline constexpr int kDefaultQp = 21;

struct EntropyConfig {
    int scale_max_index = 0;
    uint32_t qp_num = 0;
    uint32_t z_channels = 0;
    int channel_repeat = 0;
    int spatial_repeat = 0;
};

// 每个 QP 档位一个 NPU 上下文（QPPATCH 增量在初始化时应用）。
struct RungSet {
    std::vector<std::unique_ptr<NpuModel>> models;
    std::vector<int> rung_qp;  // 升序
    size_t active = 0;
};

struct QRows {
    bool present = false;
    // 供 q_*_row 使用的（输入索引，表）对。
    std::vector<std::pair<size_t, const QpTable*>> rows;
    int fed_qp = -1;
};

// 共享辅助函数（codec.cpp）。
rkvc::Status init_rans_coders(const rkvc::Model& model, RansCoder& g,
                              RansCoder& b, EntropyConfig& cfg,
                              rkvc::Diag* diag);
rkvc::Status collect_rungs(const rkvc::Model& model, int session_qp,
                           std::vector<int>& rung_qp, rkvc::Diag* diag);
rkvc::Status init_rungs(const rkvc::Model& model,
                        const std::vector<int>& rung_qp, NpuModelFn make_model,
                        RungSet& rs, rkvc::Diag* diag);
rkvc::Status setup_qrows(const rkvc::Model& model, const NpuModel& npu,
                         QpTables& qptab, QRows& qr, rkvc::Diag* diag);
rkvc::Status resolve_entropy_geometry(EntropyConfig& cfg, int qp, int ZC,
                                      int ZH, int ZW, int YC, int YH, int YW,
                                      rkvc::Diag* diag);
int nearest_rung(const std::vector<int>& rung_qp, int q) noexcept;
void build_z_idx(std::vector<int32_t>& z_idx, int qp, int ZC, int ZH, int ZW);
int find_input(const std::vector<NpuTensorInfo>& ts, const char* key) noexcept;
int find_output(const std::vector<NpuTensorInfo>& ts, const char* key) noexcept;

// MLVC 神经编码器（"mlvc.encode"）/解码器（"mlvc.decode"）。NPU
// 后端为注入式（插件中是真实 RKNN，测试中是 fake）。
class MlvcEncoderNode : public rkvc::Node {
public:
    struct Impl;
    MlvcEncoderNode(rkvc::Request req, NpuModelFn make_model);
    ~MlvcEncoderNode() override;
    std::string_view id() const noexcept override { return "mlvc.encode"; }
    std::vector<rkvc::Port> make_ports() const override;
    rkvc::Status configure(std::vector<rkvc::Port>& ports,
                           rkvc::Diag* diag) override;
    rkvc::Status open(rkvc::Emit* emit, rkvc::Diag* diag) override;
    rkvc::Status process(rkvc::FramePtr input, rkvc::Diag* diag) override;
    rkvc::Status flush(rkvc::Diag* diag) override;
    void close() noexcept override;
    bool wants_model() const noexcept override { return true; }
    rkvc::Status bind_model(const rkvc::Model& model,
                            rkvc::Diag* diag) override;

private:
    std::unique_ptr<Impl> impl_;
};

class MlvcDecoderNode : public rkvc::Node {
public:
    struct Impl;
    MlvcDecoderNode(rkvc::Request req, NpuModelFn make_model);
    ~MlvcDecoderNode() override;
    std::string_view id() const noexcept override { return "mlvc.decode"; }
    std::vector<rkvc::Port> make_ports() const override;
    rkvc::Status configure(std::vector<rkvc::Port>& ports,
                           rkvc::Diag* diag) override;
    rkvc::Status open(rkvc::Emit* emit, rkvc::Diag* diag) override;
    rkvc::Status process(rkvc::FramePtr input, rkvc::Diag* diag) override;
    void close() noexcept override;
    bool wants_model() const noexcept override { return true; }
    rkvc::Status bind_model(const rkvc::Model& model,
                            rkvc::Diag* diag) override;

private:
    std::unique_ptr<Impl> impl_;
};

class MlvcEncodeFactory : public rkvc::Factory {
public:
    explicit MlvcEncodeFactory(NpuModelFn make_model = make_rknn_model)
        : make_model_(std::move(make_model)) {}
    std::string_view id() const noexcept override { return "mlvc.encode"; }
    rkvc::NodeStage stage() const noexcept override {
        return rkvc::NodeStage::Encode;
    }
    bool matches(const rkvc::Request& r,
                 const rkvc::DeviceCaps&) const noexcept override {
        return r.operation == rkvc::Operation::Encode &&
               r.codec == rkvc::Codec::Mlvc;
    }
    rkvc::Result<rkvc::NodePtr> create(const rkvc::Request& r,
                                       rkvc::Diag*) const override;

private:
    NpuModelFn make_model_;
};

class MlvcDecodeFactory : public rkvc::Factory {
public:
    explicit MlvcDecodeFactory(NpuModelFn make_model = make_rknn_model)
        : make_model_(std::move(make_model)) {}
    std::string_view id() const noexcept override { return "mlvc.decode"; }
    rkvc::NodeStage stage() const noexcept override {
        return rkvc::NodeStage::Decode;
    }
    bool matches(const rkvc::Request& r,
                 const rkvc::DeviceCaps&) const noexcept override {
        return r.operation == rkvc::Operation::Decode &&
               r.codec == rkvc::Codec::Mlvc;
    }
    rkvc::Result<rkvc::NodePtr> create(const rkvc::Request& r,
                                       rkvc::Diag*) const override;

private:
    NpuModelFn make_model_;
};

}  // namespace mlvc
