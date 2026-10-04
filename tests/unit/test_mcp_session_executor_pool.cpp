#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

#include "include/mcp/session_executor_pool.h"

using namespace fairyfly::mcp;
using namespace std::chrono_literals;

namespace {
bool retire_eventually(SessionExecutorPool& pool, const std::string& key) {
    for (int i = 0; i < 100; ++i) {
        if (pool.retire_idle(key)) return true;
        std::this_thread::sleep_for(10ms);
    }
    return false;
}
}

TEST_CASE("Session executor lanes overlap across identities and serialize within one", "[mcp][session-executor]") {
    SessionExecutorPool pool(2, 4, 5000);
    std::atomic<int> started{0};
    std::promise<void> release;
    auto release_signal = release.get_future().share();
    auto held_job = [&](int id) {
        ExecJob job;
        job.id = id;
        job.run = [&, id](CallState&) {
            ++started;
            release_signal.wait();
            return json{{"id", id}};
        };
        return job;
    };

    SubmitResult status;
    auto a = pool.submit_future("session-A", held_job(1), &status);
    REQUIRE(status == SubmitResult::Queued);
    auto b = pool.submit_future("session-B", held_job(2), &status);
    REQUIRE(status == SubmitResult::Queued);
    for (int i = 0; i < 100 && started < 2; ++i) std::this_thread::sleep_for(10ms);
    CHECK(started == 2);
    release.set_value();
    CHECK(a.get()["id"] == 1);
    CHECK(b.get()["id"] == 2);

    std::atomic<int> same_active{0};
    std::atomic<int> same_max{0};
    auto same_job = [&](int id) {
        ExecJob job;
        job.id = id;
        job.run = [&, id](CallState&) {
            const int n = ++same_active;
            same_max = std::max(same_max.load(), n);
            std::this_thread::sleep_for(40ms);
            --same_active;
            return json{{"id", id}};
        };
        return job;
    };
    auto first = pool.submit_future("session-A", same_job(3), &status);
    auto second = pool.submit_future("session-A", same_job(4), &status);
    CHECK(first.get()["id"] == 3);
    CHECK(second.get()["id"] == 4);
    CHECK(same_max == 1);
}

TEST_CASE("Session lane reservation waits for running work and blocks later work", "[mcp][session-executor]") {
    SessionExecutorPool pool(1, 3, 5000);
    std::promise<void> release_first;
    const auto first_signal = release_first.get_future().share();
    std::promise<void> first_started;
    ExecJob first_job;
    first_job.run = [&](CallState&) {
        first_started.set_value();
        first_signal.wait();
        return json::object();
    };
    auto first = pool.submit_future("connection:7", std::move(first_job));
    REQUIRE(first_started.get_future().wait_for(1s) == std::future_status::ready);
    auto reservation = std::async(std::launch::async, [&] {
        return pool.reserve("connection:7", 2s);
    });
    CHECK(reservation.wait_for(50ms) == std::future_status::timeout);
    release_first.set_value();
    first.get();
    REQUIRE(reservation.wait_for(1s) == std::future_status::ready);
    auto held = reservation.get();
    REQUIRE(held);
    std::atomic<bool> later_ran{false};
    ExecJob later_job;
    later_job.run = [&](CallState&) { later_ran = true; return json::object(); };
    auto later = pool.submit_future("connection:7", std::move(later_job));
    CHECK(later.wait_for(50ms) == std::future_status::timeout);
    CHECK_FALSE(later_ran);
    held.reset();
    REQUIRE(later.wait_for(1s) == std::future_status::ready);
    CHECK(later_ran);
}

TEST_CASE("Session lane reservation stops promptly when its queued barrier is dropped", "[mcp][session-executor]") {
    SessionExecutorPool pool(1, 2, 5000);
    std::promise<void> first_started;
    std::promise<void> release_first;
    const auto release_signal = release_first.get_future().share();
    ExecJob first_job;
    first_job.run = [&](CallState&) {
        first_started.set_value();
        release_signal.wait();
        return json::object();
    };
    auto first = pool.submit_future("window:A", std::move(first_job));
    REQUIRE(first_started.get_future().wait_for(1s) == std::future_status::ready);
    auto reservation = std::async(std::launch::async, [&] { return pool.reserve("window:A", 5s); });
    for (int i = 0; i < 100 && pool.queued("window:A") == 0; ++i) std::this_thread::sleep_for(1ms);
    REQUIRE(pool.queued("window:A") == 1);
    pool.request_stop();
    const bool returned = reservation.wait_for(500ms) == std::future_status::ready;
    release_first.set_value();
    first.get();
    CHECK(returned);
    if (returned) CHECK_FALSE(reservation.get());
    else reservation.wait();
}

TEST_CASE("Session lane reservation stops promptly when caller cancels", "[mcp][session-executor]") {
    SessionExecutorPool pool(1, 2, 5000);
    std::promise<void> first_started;
    std::promise<void> release_first;
    const auto release_signal = release_first.get_future().share();
    ExecJob first_job;
    first_job.run = [&](CallState&) {
        first_started.set_value();
        release_signal.wait();
        return json::object();
    };
    auto first = pool.submit_future("window:A", std::move(first_job));
    REQUIRE(first_started.get_future().wait_for(1s) == std::future_status::ready);
    std::atomic<bool> cancelled{false};
    auto reservation = std::async(std::launch::async, [&] {
        return pool.reserve("window:A", 5s, [&] { return cancelled.load(); });
    });
    for (int i = 0; i < 100 && pool.queued("window:A") == 0; ++i) std::this_thread::sleep_for(1ms);
    REQUIRE(pool.queued("window:A") == 1);
    cancelled = true;
    const bool returned = reservation.wait_for(500ms) == std::future_status::ready;
    release_first.set_value();
    first.get();
    CHECK(returned);
    if (returned) CHECK_FALSE(reservation.get());
    else reservation.wait();
}

TEST_CASE("Session executor pool bounds the number of sessions", "[mcp][session-executor]") {
    SessionExecutorPool pool(1, 2, 5000);
    ExecJob job;
    job.id = 1;
    job.run = [](CallState&) { return json::object(); };
    SubmitResult status;
    auto first = pool.submit_future("session-A", job, &status);
    REQUIRE(status == SubmitResult::Queued);
    auto refused = pool.submit_future("session-B", job, &status);
    CHECK(status == SubmitResult::QueueFull);
    CHECK_FALSE(refused.valid());
    CHECK(first.valid());
    first.get();
    REQUIRE(retire_eventually(pool, "session-A"));
    auto replacement = pool.submit_future("session-B", job, &status);
    REQUIRE(status == SubmitResult::Queued);
    CHECK(replacement.valid());
    replacement.get();
}

TEST_CASE("Session executor pool reuses capacity from an idle prior identity", "[mcp][session-executor]") {
    SessionExecutorPool pool(1, 2, 5000);
    ExecJob job;
    job.run = [](CallState&) { return json::object(); };
    REQUIRE(pool.submit_future("guessed-connection", job).get().is_object());
    // The result can become ready just before the executor marks itself idle.
    std::this_thread::sleep_for(20ms);
    SubmitResult status = SubmitResult::QueueFull;
    auto legitimate = pool.submit_future("owner-session", job, &status);
    CHECK(status == SubmitResult::Queued);
    REQUIRE(legitimate.valid());
    CHECK(legitimate.get().is_object());
}

TEST_CASE("Session executor cannot retire a call still running", "[mcp][session-executor]") {
    SessionExecutorPool pool(1, 2, 5000);
    std::promise<void> release;
    auto release_signal = release.get_future().share();
    std::atomic<bool> started{false};
    ExecJob job;
    job.id = 1;
    job.run = [&](CallState&) {
        started = true;
        release_signal.wait();
        return json::object();
    };
    SubmitResult status;
    auto running = pool.submit_future("session-A", job, &status);
    REQUIRE(status == SubmitResult::Queued);
    for (int i = 0; i < 100 && !started; ++i) std::this_thread::sleep_for(10ms);
    REQUIRE(started);
    CHECK_FALSE(pool.retire_idle("session-A"));
    auto other = pool.submit_future("session-B", job, &status);
    CHECK(status == SubmitResult::QueueFull);
    CHECK_FALSE(other.valid());
    release.set_value();
    running.get();
    CHECK(retire_eventually(pool, "session-A"));
}

TEST_CASE("Call executor refuses a job after stop", "[mcp][session-executor]") {
    CallExecutor executor(2, 5000);
    executor.request_stop();
    ExecJob job;
    job.id = 1;
    job.run = [](CallState&) { return json::object(); };
    SubmitResult status;
    auto future = executor.submit_future(std::move(job), &status);
    CHECK(status == SubmitResult::QueueFull);
    CHECK_FALSE(future.valid());
}

TEST_CASE("Call executor stays active until response delivery finishes", "[mcp][session-executor]") {
    CallExecutor executor(2, 5000);
    std::promise<void> delivery_started;
    std::promise<void> release_delivery;
    auto release_signal = release_delivery.get_future().share();
    ExecJob job;
    job.id = 1;
    job.run = [](CallState&) { return json::object(); };
    job.deliver = [&](const json&) {
        delivery_started.set_value();
        release_signal.wait();
    };
    std::thread runner([&] { executor.run(); });
    REQUIRE(executor.submit(std::move(job)) == SubmitResult::Queued);
    auto delivered = delivery_started.get_future();
    if (delivered.wait_for(2s) != std::future_status::ready) {
        release_delivery.set_value();
        executor.request_stop();
        runner.join();
        FAIL("response delivery did not start");
    }
    CHECK_FALSE(executor.idle());
    release_delivery.set_value();
    executor.request_stop();
    runner.join();
    CHECK(executor.idle());
}
