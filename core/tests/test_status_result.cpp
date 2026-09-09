// SPDX-License-Identifier: AGPL-3.0-or-later
// Status/Result/Diag 回归（doctest + DOCTEST_CONFIG_NO_EXCEPTIONS）。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>

#include "rkvc/diag.hpp"
#include "rkvc/result.hpp"
#include "rkvc/status.hpp"

TEST_CASE("status basics") {
    using namespace rkvc;
    CHECK(is_ok(Status::Ok));
    CHECK(!is_ok(Status::Again));
    CHECK(is_flow(Status::Again));
    CHECK(is_flow(Status::Eof));
    CHECK(!is_flow(Status::Ok));
    CHECK(static_cast<int>(Status::Again) < 0);
    CHECK(std::string(to_string(Status::Ok)) == "ok");
    CHECK(std::string(to_string(Status::Model)) == "model error");
}

TEST_CASE("result value and error") {
    using namespace rkvc;
    auto ok = Result<int>::success(42);
    CHECK(ok);
    CHECK(ok.status() == Status::Ok);
    CHECK(ok.value() == 42);

    Diag d;
    CHECK(d.empty());
    d.add("open", "mpp", "no device");
    CHECK(d.size() == 1);
    auto fail = Result<int>::failure(Status::Hw, std::move(d));
    CHECK(!fail);
    CHECK(fail.status() == Status::Hw);
    CHECK(fail.diag().size() == 1);
    CHECK(fail.diag().format().find("mpp") != std::string::npos);
}

TEST_CASE("result void") {
    using namespace rkvc;
    auto v = Result<void>::success();
    CHECK(v.ok());
    auto ve = Result<void>::failure(Status::Nomem);
    CHECK(!ve);
    CHECK(ve.status() == Status::Nomem);
}

TEST_CASE("diag format") {
    using namespace rkvc;
    Diag d;
    d.add("probe", "mpp", "no device");
    d.add("open", "svt", "missing library");
    CHECK(d.size() == 2);
    std::string text = d.format();
    CHECK(text.find("probe(mpp)") != std::string::npos);
    CHECK(text.find("open(svt)") != std::string::npos);
}
