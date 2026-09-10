// SPDX-License-Identifier: AGPL-3.0-or-later
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <thread>

#include "rkvc/queue.hpp"

TEST_CASE("queue tri-state") {
    using namespace rkvc;
    BoundedQueue<int> q(2);
    CHECK(q.try_pop().status() == Status::Again);
    CHECK(q.try_push(1) == Status::Ok);
    CHECK(q.try_push(2) == Status::Ok);
    CHECK(q.try_push(3) == Status::Again);
    auto a = q.try_pop();
    CHECK(a);
    CHECK(a.value() == 1);
    auto b = q.pop();
    CHECK(b);
    CHECK(b.value() == 2);
    CHECK(q.try_pop().status() == Status::Again);
}

TEST_CASE("queue close drains then eof") {
    using namespace rkvc;
    BoundedQueue<int> q(2);
    CHECK(q.try_push(7) == Status::Ok);
    q.close();
    CHECK(q.try_push(8) == Status::Eof);
    CHECK(q.push(8) == Status::Eof);
    auto a = q.try_pop();
    CHECK(a);
    CHECK(a.value() == 7);
    CHECK(q.try_pop().status() == Status::Eof);
    CHECK(q.pop().status() == Status::Eof);
    q.close();  // idempotent
    CHECK(q.pop().status() == Status::Eof);
}

TEST_CASE("queue cancel surfaces after buffered items") {
    using namespace rkvc;
    BoundedQueue<int> q(2);
    CHECK(q.try_push(1) == Status::Ok);
    q.cancel(Status::Io);
    auto a = q.try_pop();
    CHECK(a);
    CHECK(a.value() == 1);
    CHECK(q.try_pop().status() == Status::Io);
    CHECK(q.try_push(2) == Status::Io);
    // cancel is sticky: close cannot override the error.
    q.close();
    CHECK(q.try_pop().status() == Status::Io);
}

TEST_CASE("blocking pop wakes on close") {
    using namespace rkvc;
    BoundedQueue<int> q(1);
    Status seen = Status::Ok;
    bool got_item = false;
    std::thread t([&] {
        auto r = q.pop();
        if (r)
            got_item = true;
        else
            seen = r.status();
    });
    q.try_push(42);
    t.join();
    CHECK(got_item);
    std::thread t2([&] { seen = q.pop().status(); });
    q.close();
    t2.join();
    CHECK(seen == Status::Eof);
}
