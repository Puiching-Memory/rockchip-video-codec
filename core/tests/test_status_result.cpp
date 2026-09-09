// SPDX-License-Identifier: AGPL-3.0-or-later
// 无异常编译验证 + Status/Result/Diag 最小回归（doctest vendor 前的过渡形态）。
#include <cassert>
#include <string>

#include "rkvc/diag.hpp"
#include "rkvc/result.hpp"
#include "rkvc/status.hpp"

int main() {
    using namespace rkvc;
    assert(is_ok(Status::Ok));
    assert(!is_ok(Status::Again));
    assert(is_flow(Status::Again) && is_flow(Status::Eof));
    assert(!is_flow(Status::Ok));
    assert(static_cast<int>(Status::Again) < 0);
    assert(std::string(to_string(Status::Ok)) == "ok");

    auto ok = Result<int>::success(42);
    assert(ok && ok.status() == Status::Ok && ok.value() == 42);

    Diag d;
    assert(d.empty());
    d.add("open", "mpp", "no device");
    assert(d.size() == 1);
    auto fail = Result<int>::failure(Status::Hw, std::move(d));
    assert(!fail && fail.status() == Status::Hw);
    assert(fail.diag().size() == 1);
    assert(fail.diag().format().find("mpp") != std::string::npos);

    auto v = Result<void>::success();
    assert(v.ok());
    auto ve = Result<void>::failure(Status::Nomem);
    assert(!ve && ve.status() == Status::Nomem);
    return 0;
}
