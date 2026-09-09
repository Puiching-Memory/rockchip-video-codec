// SPDX-License-Identifier: AGPL-3.0-or-later
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "args.hpp"

namespace {

bool parse(std::vector<std::string> words, std::string& cmd, cli::Args& a) {
    std::vector<char*> argv;
    // parse_args does not mutate its inputs; keep storage alive.
    std::vector<std::string> store = words;
    for (auto& w : store)
        argv.push_back(w.data());
    return cli::parse_args((int)argv.size(), argv.data(), cmd, a);
}

}  // namespace

TEST_CASE("cli parses the board encode command") {
    std::string cmd;
    cli::Args a;
    CHECK(parse({"rkvc", "encode", "--codec", "h264", "--input", "in.yuv",
                 "--width", "640", "--height", "368", "--pixfmt", "nv12",
                 "--output", "out.bin", "--backend-dir", "/tmp/x",
                 "--qp", "21"},
                cmd, a));
    CHECK(cmd == "encode");
    CHECK(a.codec == "h264");
    CHECK(a.input == "in.yuv");
    CHECK(a.width == 640);
    CHECK(a.height == 368);
    CHECK(a.pixfmt == "nv12");
    CHECK(a.output == "out.bin");
    CHECK(a.backend_dirs.size() == 1);
    CHECK(a.qp == 21);
}

TEST_CASE("cli parses caps and optionals") {
    std::string cmd;
    cli::Args a;
    CHECK(parse({"rkvc", "caps"}, cmd, a));
    CHECK(cmd == "caps");
    CHECK(parse({"rkvc", "encode", "--codec", "av1", "--input", "i",
                 "--width", "64", "--height", "64", "--pixfmt", "yuv420p",
                 "--output", "o", "--bitrate", "1000000", "--gop", "30"},
                cmd, a));
    CHECK(a.bitrate == 1000000);
    CHECK(a.gop == 30);
    CHECK(a.qp == -1);
}

TEST_CASE("cli rejects bad input") {
    std::string cmd;
    cli::Args a;
    CHECK(!parse({"rkvc"}, cmd, a));
    CHECK(!parse({"rkvc", "frobnicate"}, cmd, a));
    CHECK(!parse({"rkvc", "encode", "--bogus"}, cmd, a));
    CHECK(!parse({"rkvc", "encode", "--codec"}, cmd, a));
    CHECK(!parse({"rkvc", "encode", "--codec", "h264", "--width", "6x4"},
                 cmd, a));
}

TEST_CASE("cli parses model options") {
    std::string cmd;
    cli::Args a;
    CHECK(parse({"rkvc", "encode", "--codec", "mlvc", "--input", "in.yuv",
                 "--width", "640", "--height", "368", "--output", "out.bin",
                 "--model", "a.rkmodel", "--model", "b.rkmodel",
                 "--model-dir", "models/mlvc", "--model-id", "mlvc-rk3576",
                 "--qp", "21"},
                cmd, a));
    CHECK(a.models.size() == 2);
    CHECK(a.models[0] == "a.rkmodel");
    CHECK(a.models[1] == "b.rkmodel");
    CHECK(a.model_dirs.size() == 1);
    CHECK(a.model_dirs[0] == "models/mlvc");
    CHECK(a.model_id == "mlvc-rk3576");
}

TEST_CASE("cli parses decode") {
    std::string cmd;
    cli::Args a;
    CHECK(parse({"rkvc", "decode", "--codec", "mlvc", "--input", "s.mlvc",
                 "--width", "640", "--height", "368", "--pixfmt", "nv12",
                 "--output", "s.yuv", "--model-id", "mlvc-dec"},
                cmd, a));
    CHECK(cmd == "decode");
    CHECK(a.codec == "mlvc");
    CHECK(a.model_id == "mlvc-dec");
}

TEST_CASE("cli parses upscale") {
    std::string cmd;
    cli::Args a;
    CHECK(parse({"rkvc", "upscale", "--input", "in.yuv", "--width", "640",
                 "--height", "360", "--pixfmt", "nv12", "--output", "o.yuv",
                 "--model-id", "sr-x3"},
                cmd, a));
    CHECK(cmd == "upscale");
    CHECK(a.width == 640);
    CHECK(a.model_id == "sr-x3");
}

TEST_CASE("cli parses version, inspect and fps") {
    std::string cmd;
    cli::Args a;
    CHECK(parse({"rkvc", "version", "--json"}, cmd, a));
    CHECK(cmd == "version");
    CHECK(a.json);
    CHECK(parse({"rkvc", "inspect", "models", "--model-dir", "m",
                 "--json"},
                cmd, a));
    CHECK(cmd == "inspect");
    CHECK(a.sub == "models");
    CHECK(a.json);
    CHECK(parse({"rkvc", "inspect", "backends", "--backend-dir", "b"},
                cmd, a));
    CHECK(a.sub == "backends");
    CHECK(!parse({"rkvc", "inspect", "frobnicate"}, cmd, a));
    CHECK(parse({"rkvc", "encode", "--codec", "h264", "--input", "i",
                 "--width", "64", "--height", "64", "--output", "o",
                 "--fps", "120"},
                cmd, a));
    CHECK(a.fps == 120);
}
