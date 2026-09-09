// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace cli {

struct Args {
    std::vector<std::string> backend_dirs;
    std::vector<std::string> models;
    std::vector<std::string> model_dirs;
    std::string model_id;
    std::string codec;
    std::string input;
    std::string output;
    std::string pixfmt = "nv12";
    uint32_t width = 0;
    uint32_t height = 0;
    int qp = -1;
    int64_t bitrate = 0;
    uint32_t gop = 0;
};

void usage();
bool parse_args(int argc, char** argv, std::string& cmd, Args& a);

}  // namespace cli
