// SPDX-License-Identifier: AGPL-3.0-or-later
#include "rkvc/model.hpp"

#include <dirent.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace rkvc {
namespace {

bool read_file(const std::string& path, std::vector<uint8_t>& out,
               std::string& err) {
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) {
        err = "open failed";
        return false;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        err = "seek failed";
        return false;
    }
    long size = ftell(fp);
    constexpr long kMaxFile = 1L << 29;  // 512 MiB，单文件上限
    if (size < 0 || size > kMaxFile || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        err = "bad file size";
        return false;
    }
    out.resize(static_cast<size_t>(size));
    bool ok = size == 0 || fread(out.data(), 1, out.size(), fp) == out.size();
    fclose(fp);
    if (!ok)
        err = "read failed";
    return ok;
}

bool has_suffix(const std::string& s, const char* suf) {
    size_t n = strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

}  // namespace

const ModelPayload* Model::find(const std::string& kind) const noexcept {
    for (const auto& p : payloads)
        if (p.kind == kind)
            return &p;
    return nullptr;
}

Result<std::vector<Model>> load_model_dir(const std::string& dir, Diag* diag) {
    auto reject = [&](Status s, const std::string& reason) {
        if (diag)
            diag->add("load", "modeldir", reason.c_str());
        return Result<std::vector<Model>>::failure(s, diag ? *diag : Diag{});
    };
    DIR* dp = opendir(dir.c_str());
    if (!dp)
        return reject(Status::Io, "opendir failed: " + dir);
    std::vector<std::string> names;
    while (dirent* e = readdir(dp))
        if (e->d_name[0] != '.')
            names.emplace_back(e->d_name);
    closedir(dp);
    std::sort(names.begin(), names.end());

    std::vector<std::string> enc, dec, plain_rknn, qptabs;
    bool has_gaussian = false, has_bitest = false;
    std::vector<std::string> qp_patches;
    for (const auto& n : names) {
        if (has_suffix(n, ".rknn")) {
            if (n.rfind("MLVCEncoder_", 0) == 0)
                enc.push_back(n);
            else if (n.rfind("MLVCDecoder_", 0) == 0)
                dec.push_back(n);
            else
                plain_rknn.push_back(n);
        } else if (n == "gaussian.bin") {
            has_gaussian = true;
        } else if (n == "bitest.bin") {
            has_bitest = true;
        } else if (has_suffix(n, ".bin") &&
                   (n.rfind("qptab_encoder", 0) == 0 ||
                    n.rfind("qptab_decoder", 0) == 0)) {
            qptabs.push_back(n);
        }
    }
    // qp_patches/ 子目录（--make-patches 产物）
    {
        DIR* qd = opendir((dir + "/qp_patches").c_str());
        if (qd) {
            while (dirent* e = readdir(qd))
                if (has_suffix(e->d_name, ".qppatch"))
                    qp_patches.emplace_back("qp_patches/" +
                                            std::string(e->d_name));
            closedir(qd);
            std::sort(qp_patches.begin(), qp_patches.end());
        }
    }

    std::vector<Model> models;
    std::string err;
    auto load = [&](const std::string& rel, const char* kind,
                    ModelPayload& dst) {
        std::vector<uint8_t> data;
        if (!read_file(dir + "/" + rel, data, err)) {
            err = rel + ": " + err;
            return false;
        }
        dst.kind = kind;
        dst.data = std::move(data);
        return true;
    };

    // MLVC 编解码：目录内 MLVC*.rknn + PMF +（可选）qptab/qppatch 整组。
    if (!enc.empty() || !dec.empty()) {
        if (!has_gaussian || !has_bitest)
            return reject(Status::Format,
                          "MLVC bundle missing gaussian.bin/bitest.bin");
        // target 从第一个 MLVC*_<t>.rknn 提取（如 rk3576）。
        std::string target;
        {
            const std::string& first = !enc.empty() ? enc.front() : dec.front();
            size_t u = first.find('_');
            target = u == std::string::npos
                         ? std::string()
                         : first.substr(u + 1, first.size() - u - 1 - 5);
        }
        for (const auto& role_files :
             std::vector<std::pair<const char*, std::vector<std::string>*>>{
                 {"encoder", &enc}, {"decoder", &dec}}) {
            const std::string role = role_files.first;
            for (const auto& n : *role_files.second) {
                Model m;
                std::string stem = n.substr(0, n.size() - 5);  // 去 .rknn
                m.meta.id = stem;
                m.meta.family = "mlvc";
                m.meta.role = role;
                m.meta.target = target;
                ModelPayload p;
                if (!load(n, "rknn", p))
                    return reject(Status::Io, err);
                m.payloads.push_back(std::move(p));
                ModelPayload g, b;
                if (!load("gaussian.bin", "pmf-gaussian", g) ||
                    !load("bitest.bin", "pmf-bitest", b))
                    return reject(Status::Io, err);
                m.payloads.push_back(std::move(g));
                m.payloads.push_back(std::move(b));
                const char* qp_pref =
                    role == "encoder" ? "qptab_encoder" : "qptab_decoder";
                for (const auto& t : qptabs)
                    if (t.rfind(qp_pref, 0) == 0) {
                        ModelPayload q;
                        if (!load(t, "qptab", q))
                            return reject(Status::Io, err);
                        m.payloads.push_back(std::move(q));
                    }
                const char* patch_pref =
                    role == "encoder" ? "enc_qp" : "dec_qp";
                for (const auto& t : qp_patches) {
                    std::string base = t.substr(t.find_last_of('/') + 1);
                    if (base.rfind(patch_pref, 0) == 0) {
                        ModelPayload q;
                        if (!load(t, "qppatch", q))
                            return reject(Status::Io, err);
                        m.payloads.push_back(std::move(q));
                    }
                }
                models.push_back(std::move(m));
            }
        }
    }

    // 其他 .rknn（SR 等）：单文件单模型，id=去扩展名文件名，family=sr。
    for (const auto& n : plain_rknn) {
        Model m;
        m.meta.id = n.substr(0, n.size() - 5);
        m.meta.family = "sr";
        m.meta.role = "upscale";
        ModelPayload p;
        if (!load(n, "rknn", p))
            return reject(Status::Io, err);
        m.payloads.push_back(std::move(p));
        models.push_back(std::move(m));
    }

    if (models.empty())
        return reject(Status::Format, "no model files in " + dir);
    return Result<std::vector<Model>>::success(std::move(models));
}

}  // namespace rkvc
