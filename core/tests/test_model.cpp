// SPDX-License-Identifier: AGPL-3.0-or-later
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <sys/stat.h>
#include <sys/types.h>

#include <fstream>
#include <string>

#include "rkvc/model.hpp"

#ifndef RKVC_TEST_MODEL_TMPDIR
#error "need RKVC_TEST_MODEL_TMPDIR"
#endif

namespace {

const std::string kTmp = RKVC_TEST_MODEL_TMPDIR;

void write(const std::string& rel, const std::string& bytes) {
    std::ofstream f(kTmp + "/" + rel, std::ios::binary);
    f << bytes;
}

std::string mkbundle(const std::string& tag) {
    std::string dir = kTmp + "/" + tag;
    mkdir(dir.c_str(), 0755);
    mkdir((dir + "/qp_patches").c_str(), 0755);
    write(tag + "/MLVCEncoder_rk3576.rknn", "ENC-GRAPH");
    write(tag + "/MLVCDecoder_rk3576.rknn", "DEC-GRAPH");
    write(tag + "/gaussian.bin", "G");
    write(tag + "/bitest.bin", "B");
    write(tag + "/qptab_encoder.bin", "QT-E");
    write(tag + "/qptab_decoder.bin", "QT-D");
    write(tag + "/qp_patches/enc_qp18.qppatch", "PE18");
    write(tag + "/qp_patches/enc_qp24.qppatch", "PE24");
    write(tag + "/qp_patches/dec_qp18.qppatch", "PD18");
    return dir;
}

}  // namespace

TEST_CASE("mlvc bundle loads as a group per role") {
    auto r = rkvc::load_model_dir(mkbundle("b1"));
    CHECK(r.ok());
    CHECK(r.value().size() == 2);
    auto& enc = r.value()[0];
    CHECK(enc.meta.family == "mlvc");
    CHECK(enc.meta.role == "encoder");
    CHECK(enc.meta.target == "rk3576");
    CHECK(enc.meta.id == "MLVCEncoder_rk3576");
    CHECK(enc.payloads.size() == 6);  // rknn+2pmf+qptab+2qppatch
    CHECK(enc.payloads[0].kind == "rknn");
    CHECK(enc.payloads[1].kind == "pmf-gaussian");
    CHECK(enc.payloads[2].kind == "pmf-bitest");
    CHECK(enc.payloads[3].kind == "qptab");
    CHECK(enc.payloads[4].kind == "qppatch");
    CHECK(enc.payloads[5].kind == "qppatch");
    CHECK(enc.find("rknn")->data.size() == 9);  // "ENC-GRAPH"
    auto& dec = r.value()[1];
    CHECK(dec.meta.role == "decoder");
    CHECK(dec.payloads.size() == 5);  // rknn+2pmf+qptab+qppatch(dec_qp18)
    CHECK(dec.find("qppatch") != nullptr);
    CHECK(dec.find("qptab")->data.size() == 4);
}

TEST_CASE("plain rknn becomes an sr model") {
    std::string dir = kTmp + "/b2";
    mkdir(dir.c_str(), 0755);
    write("b2/phase_rlfn_sr_x3.rknn", "SRGRAPH");
    auto r = rkvc::load_model_dir(dir);
    CHECK(r.ok());
    CHECK(r.value().size() == 1);
    auto& m = r.value()[0];
    CHECK(m.meta.family == "sr");
    CHECK(m.meta.role == "upscale");
    CHECK(m.meta.id == "phase_rlfn_sr_x3");
    CHECK(m.find("rknn")->data.size() == 7);
}

TEST_CASE("mlvc bundle without pmf rejected") {
    std::string dir = mkbundle("b3");
    std::remove((dir + "/gaussian.bin").c_str());
    auto r = rkvc::load_model_dir(dir);
    CHECK(!r.ok());
    CHECK(r.status() == rkvc::Status::Format);
}

TEST_CASE("missing dir rejected") {
    auto r = rkvc::load_model_dir(kTmp + "/no-such-dir");
    CHECK(!r.ok());
    CHECK(r.status() == rkvc::Status::Io);
}
