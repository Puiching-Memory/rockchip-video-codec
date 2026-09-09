// SPDX-License-Identifier: AGPL-3.0-or-later
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "rkvc/context.hpp"
#include "rkvc/session.hpp"

using namespace rkvc;

namespace {

Spec nv12_64x32() {
    Spec s;
    s.width = 64;
    s.height = 32;
    s.fmt = PixelFormat::Nv12;
    return s;
}

size_t file_size(const std::string& path) {
    FILE* fh = fopen(path.c_str(), "rb");
    if (!fh)
        return 0;
    fseek(fh, 0, SEEK_END);
    long n = ftell(fh);
    fclose(fh);
    return n < 0 ? 0 : static_cast<size_t>(n);
}

bool write_file(const std::string& path, const std::vector<uint8_t>& data) {
    FILE* fh = fopen(path.c_str(), "wb");
    if (!fh)
        return false;
    size_t w = fwrite(data.data(), 1, data.size(), fh);
    fclose(fh);
    return w == data.size();
}

bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    size_t n = file_size(path);
    if (n == 0)
        return false;
    out.resize(n);
    FILE* fh = fopen(path.c_str(), "rb");
    if (!fh)
        return false;
    size_t r = fread(out.data(), 1, n, fh);
    fclose(fh);
    return r == n;
}

Request upscale_queue_req() {
    Request r;
    r.operation = Operation::Upscale;
    r.input_spec = nv12_64x32();
    r.output_spec = nv12_64x32();
    return r;
}

}  // namespace

TEST_CASE("identity plugin end to end") {
    Context ctx;
    CHECK(ctx.load_plugin(RKVC_TEST_PLUGIN_IDENTITY) == Status::Ok);
    CHECK(ctx.plugin_count() == 1);
    CHECK(ctx.registry().find("test.identity") != nullptr);
    auto s = Session::create(ctx, upscale_queue_req());
    CHECK(s);
    if (!s)
        return;
    auto session = s.value();
    CHECK(session->start() == Status::Ok);
    std::vector<uint8_t> buf(min_size(nv12_64x32()), 0xAB);
    auto fr = Frame::borrow_host(nv12_64x32(), buf.data(), buf.size());
    CHECK(fr);
    if (!fr)
        return;
    fr.value()->set_pts(7);
    CHECK(session->push(fr.value()) == Status::Ok);
    CHECK(session->push_eos() == Status::Ok);
    auto out = session->pull();
    CHECK(out);
    if (out) {
        CHECK(out.value()->pts() == 7);
        CHECK(out.value()->size() == buf.size());
        CHECK(out.value()->data() != nullptr);
        const uint8_t* p =
            static_cast<const uint8_t*>(out.value()->data());
        CHECK(p[0] == 0xAB);
        CHECK(p[buf.size() - 1] == 0xAB);
    }
    CHECK(session->pull().status() == Status::Eof);
}

TEST_CASE("bad abi plugin is evicted") {
    Context ctx;
    size_t before = ctx.registry().size();
    CHECK(ctx.load_plugin(RKVC_TEST_PLUGIN_BADABI) == Status::Unsupported);
    CHECK(ctx.plugin_count() == 0);
    CHECK(ctx.registry().size() == before);
    CHECK(ctx.registry().find("test.identity") == nullptr);
}

TEST_CASE("missing query symbol is not found") {
    Context ctx;
    CHECK(ctx.load_plugin(RKVC_TEST_PLUGIN_EMPTY) == Status::NotFound);
    CHECK(ctx.plugin_count() == 0);
}

TEST_CASE("file source to file sink through plugin") {
    std::string dir = RKVC_TEST_TMPDIR;
    std::string in_path = dir + "/fileio_in.nv12";
    std::string out_path = dir + "/fileio_out.nv12";
    std::vector<uint8_t> raw(2 * min_size(nv12_64x32()));
    for (size_t i = 0; i < raw.size(); ++i)
        raw[i] = static_cast<uint8_t>(i & 0xff);
    CHECK(write_file(in_path, raw));
    remove(out_path.c_str());

    Context ctx;
    CHECK(ctx.load_plugin(RKVC_TEST_PLUGIN_IDENTITY) == Status::Ok);
    Request r;
    r.operation = Operation::Upscale;
    r.input.kind = EndpointKind::File;
    r.input.uri = in_path;
    r.input_spec = nv12_64x32();
    r.output.kind = EndpointKind::File;
    r.output.uri = out_path;
    r.output_spec = nv12_64x32();
    auto s = Session::create(ctx, r);
    CHECK(s);
    CHECK(s.value()->start() == Status::Ok);
    for (int i = 0; i < 400 && file_size(out_path) < raw.size(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(s.value()->close() == Status::Ok);
    std::vector<uint8_t> back;
    CHECK(read_file(out_path, back));
    CHECK(back == raw);
}
