// SPDX-License-Identifier: AGPL-3.0-or-later
#include "rkvc/context.hpp"

#include <dirent.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <string>

#include "builtin.hpp"

namespace rkvc {

namespace {

// First "rockchip,<soc>" token from the device-tree compatible list
// (NUL-separated strings); empty when not on Rockchip hardware.
std::string probe_soc() {
    FILE* fp = std::fopen("/proc/device-tree/compatible", "rb");
    if (!fp)
        return {};
    char buf[1024];
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, fp);
    std::fclose(fp);
    buf[n] = '\0';
    for (size_t i = 0; i + 1 < n;) {
        if (buf[i] == '\0') {
            ++i;
            continue;
        }
        if (std::strncmp(&buf[i], "rockchip,", 9) == 0)
            return {&buf[i + 9]};
        i += std::strlen(&buf[i]);
    }
    return {};
}

bool node_exists(const char* path) noexcept {
    return ::access(path, F_OK) == 0;
}

// DRM-based rknpu driver exposes no /dev/rknpu; the NPU shows up as DRI
// card/render nodes instead (161: platform-27700000.npu-card/render).
bool dir_has(const char* dir, const char* needle) noexcept {
    DIR* dp = ::opendir(dir);
    if (!dp)
        return false;
    bool hit = false;
    while (dirent* e = ::readdir(dp)) {
        if (std::strstr(e->d_name, needle)) {
            hit = true;
            break;
        }
    }
    ::closedir(dp);
    return hit;
}

}  // namespace

Context::Context(ContextOptions opts) : opts_(std::move(opts)) {
    register_builtin_fileio(registry_);
}

Status Context::add_model(Model m) {
    if (m.meta.id.empty())
        return Status::Invalid;
    if (find_model(m.meta.id) != nullptr)
        return Status::Invalid;
    models_.push_back(std::move(m));
    return Status::Ok;
}

const Model* Context::find_model(const std::string& id) const noexcept {
    for (const auto& m : models_)
        if (m.meta.id == id)
            return &m;
    return nullptr;
}

const Model* Context::first_model() const noexcept {
    return models_.empty() ? nullptr : &models_.front();
}

DeviceCaps Context::probe_device() {
    if (!caps_probed_) {
        caps_ = DeviceCaps{};
        caps_.soc = probe_soc();
        // Advisory only: planner still rejects unsupported stages per node.
        // query_caps probes a bare context (no plugins loaded), so backend
        // registration cannot refine these bits in that flow; device nodes
        // are the only signal available here.
        if (node_exists("/dev/mpp_service")) {
            caps_.mpp_encode = true;
            caps_.mpp_decode = true;
        }
        caps_.rga = node_exists("/dev/rga");
        // NPU present bit; exact core count is refined by the RKNN backend
        // at registration (bare-context probe reports presence as 1).
        if (node_exists("/dev/rknpu") || node_exists("/dev/rknpu0") ||
            node_exists("/dev/rknn") || dir_has("/dev/dri/by-path", "npu") ||
            dir_has("/sys/class/devfreq", "npu")) {
            caps_.rknn = true;
            caps_.npu_cores = 1;
        }
        caps_probed_ = true;
    }
    return caps_;
}

void Context::override_device_caps(const DeviceCaps& caps) {
    caps_ = caps;
    caps_probed_ = true;
}

Result<Plan> Context::plan(const Request& req, Diag* diag) {
    auto reject = [&](Status s, const char* reason) {
        if (diag)
            diag->add("planner", "request", reason);
        return Result<Plan>::failure(s, diag ? *diag : Diag{});
    };
    if (!validate(req, diag))
        return Result<Plan>::failure(Status::Invalid, diag ? *diag : Diag{});
    DeviceCaps caps = probe_device();

    std::vector<NodeStage> middle;
    switch (req.operation) {
        case Operation::Encode:
            middle = {NodeStage::Encode};
            break;
        case Operation::Decode:
            middle = {NodeStage::Decode};
            break;
        case Operation::Transcode:
            middle = {NodeStage::Decode, NodeStage::Encode};
            break;
        case Operation::Upscale:
            middle = {NodeStage::Transform};
            break;
        default:
            return reject(Status::Invalid, "unknown operation");
    }

    Plan p;
    auto endpoint_step = [&](const Endpoint& ep, bool source) -> Result<void> {
        PlanStep st;
        st.stage = source ? NodeStage::Source : NodeStage::Sink;
        if (ep.kind == EndpointKind::FrameSink) {
            st.adapter_source = source;
            st.adapter_sink = !source;
            p.steps.push_back(std::move(st));
            return Result<void>::success();
        }
        if (ep.kind == EndpointKind::File) {
            for (const Factory* f : registry_.candidates(st.stage, req, caps))
                if (f->transport() == Transport::File)
                    st.candidates.push_back(f);
            if (st.candidates.empty())
                return Result<void>::failure(Status::Unsupported, Diag{});
            p.steps.push_back(std::move(st));
            return Result<void>::success();
        }
        return Result<void>::failure(Status::Unsupported, Diag{});
    };
    if (!endpoint_step(req.input, true))
        return reject(Status::Unsupported, "no source for endpoint");
    for (NodeStage st : middle) {
        PlanStep step;
        step.stage = st;
        step.candidates = registry_.candidates(st, req, caps);
        if (step.candidates.empty())
            return reject(Status::NotFound, "required stage has no candidate");
        p.steps.push_back(std::move(step));
    }
    if (!endpoint_step(req.output, false))
        return reject(Status::Unsupported, "no sink for endpoint");
    return Result<Plan>::success(std::move(p));
}

}  // namespace rkvc
