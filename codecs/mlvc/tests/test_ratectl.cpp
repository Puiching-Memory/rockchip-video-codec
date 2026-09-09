// SPDX-License-Identifier: AGPL-3.0-or-later
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <memory>
#include <vector>

#include "mlvc/ratectl.hpp"

namespace {

std::vector<int> run_scenario(mlvc::RateController& rc, int n, int ltr_at,
                              double fps, long pay_i, long pay_ltr,
                              long pay_p) {
    std::vector<int> qs;
    for (int i = 0; i < n; ++i) {
        mlvc::RcFrameType t = (i == 0) ? mlvc::RcFrameType::I
                              : ((i == ltr_at) ? mlvc::RcFrameType::LtrRecovery
                                               : mlvc::RcFrameType::P);
        double pt = i / fps;
        long pay = (t == mlvc::RcFrameType::I)          ? pay_i
                   : (t == mlvc::RcFrameType::LtrRecovery) ? pay_ltr
                                                           : pay_p;
        int q = rc.solve(pt, t, 128);
        qs.push_back(q);
        rc.update(128, q == mlvc::kQDrop ? 0 : pay);
    }
    return qs;
}

void check_eq(const std::vector<int>& got, const std::vector<int>& want) {
    CHECK(got.size() == want.size());
    if (got.size() != want.size())
        return;
    for (size_t i = 0; i < want.size(); ++i)
        CHECK(got[i] == want[i]);
}

}  // namespace

// Goldens dumped from the C implementation (same scenarios/feedback).
TEST_CASE("ratectl matches C golden S1") {
    auto rc = mlvc::RateController::create(640, 368, 300000.0, 30.0);
    CHECK(rc);
    if (!rc)
        return;
    auto qs = run_scenario(*rc.value(), 32, 16, 30.0, 30000, 15000, 8000);
    check_eq(qs, {59, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63,
                  63, 60, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63,
                  63, 63});
}

TEST_CASE("ratectl matches C golden S2 with drops") {
    auto rc = mlvc::RateController::create(320, 240, 24000.0, 30.0);
    CHECK(rc);
    if (!rc)
        return;
    auto qs = run_scenario(*rc.value(), 12, -1, 30.0, 12000, 0, 3000);
    check_eq(qs, {20, 25, 10, 0, 0, -1, -1, 0, -1, -1, -1, 0});
}

TEST_CASE("ratectl matches C golden S3") {
    auto rc = mlvc::RateController::create(1280, 720, 800000.0, 30.0);
    CHECK(rc);
    if (!rc)
        return;
    auto qs = run_scenario(*rc.value(), 20, -1, 30.0, 60000, 0, 16000);
    check_eq(qs, {49, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63,
                  63, 63, 63, 63, 63});
}

TEST_CASE("ratectl create and configure validation") {
    CHECK(!mlvc::RateController::create(0, 32, 1000.0, 30.0));
    CHECK(!mlvc::RateController::create(32, 32, 0.0, 30.0));
    CHECK(!mlvc::RateController::create(32, 32, 1000.0, 0.0));
    auto rc = mlvc::RateController::create(64, 64, 100000.0, 30.0);
    CHECK(rc);
    if (!rc)
        return;
    rc.value()->configure(-1.0);  // ignored
    int q = rc.value()->solve(0.0, mlvc::RcFrameType::I, 128);
    CHECK(q >= 0);
    CHECK(q <= 63);
    rc.value()->configure(200000.0);
    rc.value()->update(128, 5000);
}
