// SPDX-License-Identifier: AGPL-3.0-or-later
#include "args.hpp"

#include <cstdio>
#include <cstdlib>

namespace cli {

void usage() {
    printf(
        "usage:\n"
        "  rkvc caps [--backend-dir DIR]...\n"
        "  rkvc encode --codec h264|hevc|av1 --input IN --width W --height H\n"
        "            --pixfmt nv12|yuv420p --output OUT [--backend-dir DIR]...\n"
        "            [--qp Q] [--bitrate BPS] [--gop G]\n");
}

namespace {

bool take_value(int argc, char** argv, int& i, std::string& out) {
    if (i + 1 >= argc)
        return false;
    out = argv[++i];
    return true;
}

bool parse_uint(const std::string& s, uint32_t& out) {
    unsigned long v = 0;
    if (s.empty())
        return false;
    for (char c : s) {
        if (c < '0' || c > '9')
            return false;
        v = v * 10 + (c - '0');
        if (v > 0xFFFFFFFFul)
            return false;
    }
    out = (uint32_t)v;
    return true;
}

}  // namespace

bool parse_args(int argc, char** argv, std::string& cmd, Args& a) {
    if (argc < 2)
        return false;
    cmd = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string k = argv[i];
        std::string v;
        if (k == "--backend-dir" && take_value(argc, argv, i, v))
            a.backend_dirs.push_back(v);
        else if (k == "--codec" && take_value(argc, argv, i, v))
            a.codec = v;
        else if (k == "--input" && take_value(argc, argv, i, v))
            a.input = v;
        else if (k == "--output" && take_value(argc, argv, i, v))
            a.output = v;
        else if (k == "--pixfmt" && take_value(argc, argv, i, v))
            a.pixfmt = v;
        else if (k == "--width" && take_value(argc, argv, i, v)) {
            if (!parse_uint(v, a.width))
                return false;
        } else if (k == "--height" && take_value(argc, argv, i, v)) {
            if (!parse_uint(v, a.height))
                return false;
        } else if (k == "--qp" && take_value(argc, argv, i, v)) {
            a.qp = atoi(v.c_str());
        } else if (k == "--bitrate" && take_value(argc, argv, i, v)) {
            a.bitrate = atoll(v.c_str());
        } else if (k == "--gop" && take_value(argc, argv, i, v)) {
            uint32_t g = 0;
            if (!parse_uint(v, g))
                return false;
            a.gop = g;
        } else {
            return false;
        }
    }
    return cmd == "caps" || cmd == "encode";
}

}  // namespace cli
