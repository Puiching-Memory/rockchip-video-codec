// SPDX-License-Identifier: AGPL-3.0-or-later
#include <cstdio>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "builtin.hpp"
#include "rkvc/frame.hpp"

namespace rkvc {

namespace {

void delete_bytes(void* p) noexcept {
    delete[] static_cast<uint8_t*>(p);
}

class FileSourceNode : public Node {
public:
    FileSourceNode(std::string path, Spec spec)
        : path_(std::move(path)), spec_(spec) {}
    std::string_view id() const noexcept override { return "file.source"; }
    std::vector<Port> make_ports() const override {
        Port p;
        p.name = "out";
        p.desired = spec_;
        return {p};
    }
    Status open(Emit* emit, Diag* diag) override {
        if (!emit) {
            if (diag)
                diag->add("open", "file.source", "null emit");
            return Status::Invalid;
        }
        emit_ = emit;
        frame_size_ = min_size(spec_);
        if (frame_size_ == 0) {
            // Variable-size stream input: deliver the whole file as one
            // frame; the downstream demuxer splits it into records.
            if (spec_.fmt != PixelFormat::Bitstream) {
                if (diag)
                    diag->add("open", "file.source", "unframed spec");
                return Status::Invalid;
            }
            return open_stream(diag);
        }
        fh_ = fopen(path_.c_str(), "rb");
        if (!fh_) {
            if (diag)
                diag->add("open", path_.c_str(), "cannot read file");
            return Status::Io;
        }
        return Status::Ok;
    }
    Status process(FramePtr /*input*/, Diag* diag) override {
        if (stream_)
            return emit_stream(diag);
        uint8_t* buf = new (std::nothrow) uint8_t[frame_size_];
        if (!buf) {
            if (diag)
                diag->add("process", "file.source", "no memory");
            return Status::Nomem;
        }
        size_t n = fread(buf, 1, frame_size_, fh_);
        if (n == 0) {
            delete[] buf;
            return Status::Eof;
        }
        if (n < frame_size_) {
            delete[] buf;  // truncated tail is dropped, not emitted corrupt
            return Status::Eof;
        }
        FrameHooks hooks{delete_bytes, buf};
        auto r = Frame::borrow_host(spec_, buf, n, hooks, diag);
        if (!r) {
            delete[] buf;
            return r.status();
        }
        return emit_->emit(0, r.value());
    }
    void close() noexcept override {
        if (fh_) {
            fclose(fh_);
            fh_ = nullptr;
        }
        delete[] stream_;
        stream_ = nullptr;
    }

private:
    Status open_stream(Diag* diag) {
        FILE* fh = fopen(path_.c_str(), "rb");
        if (!fh) {
            if (diag)
                diag->add("open", path_.c_str(), "cannot read file");
            return Status::Io;
        }
        if (fseek(fh, 0, SEEK_END) != 0) {
            fclose(fh);
            if (diag)
                diag->add("open", path_.c_str(), "cannot seek file");
            return Status::Io;
        }
        long size = ftell(fh);
        constexpr long kMaxStream = 64L << 20;
        if (size <= 0 || size > kMaxStream || fseek(fh, 0, SEEK_SET) != 0) {
            fclose(fh);
            if (diag)
                diag->add("open", path_.c_str(), "bad stream size");
            return Status::Format;
        }
        stream_ = new (std::nothrow) uint8_t[(size_t)size];
        if (!stream_) {
            fclose(fh);
            return Status::Nomem;
        }
        bool ok = fread(stream_, 1, (size_t)size, fh) == (size_t)size;
        fclose(fh);
        if (!ok) {
            delete[] stream_;
            stream_ = nullptr;
            if (diag)
                diag->add("open", path_.c_str(), "cannot read file");
            return Status::Io;
        }
        stream_size_ = (size_t)size;
        return Status::Ok;
    }
    Status emit_stream(Diag* diag) {
        if (!stream_)
            return Status::Eof;
        FrameHooks hooks{delete_bytes, stream_};
        stream_ = nullptr;
        auto r =
            Frame::borrow_host(spec_, hooks.ctx, stream_size_, hooks, diag);
        if (!r)
            return r.status();
        return emit_->emit(0, r.value());
    }

    std::string path_;
    Spec spec_;
    Emit* emit_ = nullptr;
    FILE* fh_ = nullptr;
    size_t frame_size_ = 0;
    uint8_t* stream_ = nullptr;
    size_t stream_size_ = 0;
};

class FileSinkNode : public Node {
public:
    FileSinkNode(std::string path, Spec spec)
        : path_(std::move(path)), spec_(spec) {}
    std::string_view id() const noexcept override { return "file.sink"; }
    std::vector<Port> make_ports() const override {
        Port p;
        p.name = "in";
        p.is_input = true;
        p.desired = spec_;
        return {p};
    }
    Status open(Emit* /*emit*/, Diag* diag) override {
        fh_ = fopen(path_.c_str(), "wb");
        if (!fh_) {
            if (diag)
                diag->add("open", path_.c_str(), "cannot write file");
            return Status::Io;
        }
        return Status::Ok;
    }
    Status process(FramePtr input, Diag* diag) override {
        if (!input || !input->data() || input->size() == 0) {
            if (diag)
                diag->add("process", "file.sink", "unreadable frame");
            return Status::Format;
        }
        size_t w = fwrite(input->data(), 1, input->size(), fh_);
        if (w != input->size()) {
            if (diag)
                diag->add("process", path_.c_str(), "short write");
            return Status::Io;
        }
        return Status::Ok;
    }
    void close() noexcept override {
        if (fh_) {
            fclose(fh_);
            fh_ = nullptr;
        }
    }

private:
    std::string path_;
    Spec spec_;
    FILE* fh_ = nullptr;
};

struct FileSourceFactory : public Factory {
    std::string_view id() const noexcept override { return "file.source"; }
    NodeStage stage() const noexcept override { return NodeStage::Source; }
    Transport transport() const noexcept override { return Transport::File; }
    bool matches(const Request& r, const DeviceCaps&) const noexcept override {
        return r.input.kind == EndpointKind::File;
    }
    Result<NodePtr> create(const Request& r, Diag*) const override {
        NodePtr n(new (std::nothrow) FileSourceNode(r.input.uri, r.input_spec));
        if (!n)
            return Result<NodePtr>::failure(Status::Nomem);
        return Result<NodePtr>::success(std::move(n));
    }
};

struct FileSinkFactory : public Factory {
    std::string_view id() const noexcept override { return "file.sink"; }
    NodeStage stage() const noexcept override { return NodeStage::Sink; }
    Transport transport() const noexcept override { return Transport::File; }
    bool matches(const Request& r, const DeviceCaps&) const noexcept override {
        return r.output.kind == EndpointKind::File;
    }
    Result<NodePtr> create(const Request& r, Diag*) const override {
        NodePtr n(new (std::nothrow) FileSinkNode(r.output.uri, r.output_spec));
        if (!n)
            return Result<NodePtr>::failure(Status::Nomem);
        return Result<NodePtr>::success(std::move(n));
    }
};

}  // namespace

void register_builtin_fileio(Registry& registry) {
    // Builtin and infallible in practice (fixed ids, first registration).
    registry.add(std::unique_ptr<Factory>(new FileSourceFactory()));
    registry.add(std::unique_ptr<Factory>(new FileSinkFactory()));
}

}  // namespace rkvc
