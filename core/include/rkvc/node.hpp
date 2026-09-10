// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "rkvc/diag.hpp"
#include "rkvc/frame.hpp"
#include "rkvc/result.hpp"
#include "rkvc/spec.hpp"
#include "rkvc/status.hpp"

namespace rkvc {

using FramePtr = std::shared_ptr<Frame>;
using NodePtr = std::unique_ptr<class Node>;

struct DeviceCaps {
    bool mpp_encode = false;
    bool mpp_decode = false;
    bool rga = false;
    bool rknn = false;
    uint32_t npu_cores = 0;
    std::string soc;
};

struct Port {
    std::string name;
    bool is_input = false;
    Spec desired;   // node-side preference, set at construction
    Spec resolved;  // negotiated value, filled by the graph
};

/// Downstream delivery. Blocking with cancel; safe to call from process().
class Emit {
public:
    virtual ~Emit() = default;
    virtual Status emit(size_t port, FramePtr f) = 0;
};

/// Node contract: configure() never touches hardware; the input frame is
/// only borrowed for the duration of process() (retain a copy to keep it).
class Node {
public:
    virtual ~Node() = default;
    virtual std::string_view id() const noexcept = 0;
    virtual std::vector<Port> make_ports() const = 0;
    virtual Status configure(std::vector<Port>& ports, Diag* diag);
    virtual Status open(Emit* emit, Diag* diag) = 0;
    virtual Status process(FramePtr input, Diag* diag) = 0;
    virtual Status flush(Diag* /*diag*/) { return Status::Ok; }
    virtual void close() noexcept {}
    virtual bool wants_model() const noexcept { return false; }
    virtual Status bind_model(const class Model& /*model*/, Diag* /*diag*/) {
        return Status::Ok;
    }
};

}  // namespace rkvc
