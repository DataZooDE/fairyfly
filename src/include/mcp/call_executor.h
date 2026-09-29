#pragma once
// CallExecutor: the FIFO queue + main-thread loop + soft-timeout watchdog that used to live inside the
// stdio Engine (server.cpp). Transport-neutral: any thread may submit(); ONLY the thread that calls
// run() executes jobs, so COM (STA) stays on the main thread. Results are delivered through a callback
// (stdio: write a line; HTTP: complete a waiter).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

#include "include/mcp/types.h"

namespace fairyfly::mcp {

/// Shared state of one job. `responded` is the single arbiter of "who answers": the main thread
/// (normal completion), the watchdog (timeout) or a canceller (which claims it without sending
/// anything). Guarded by the executor mutex, except `cancelled` (atomic, read by long calls).
struct CallState {
    json id;
    std::atomic<bool> cancelled{false};
    bool responded = false;
    bool timed_out = false;
    std::chrono::steady_clock::time_point deadline;
    std::function<void(const json&)> deliver;  ///< receives the JSON-RPC message; null json = dropped
    std::function<json()> timeout_response;    ///< answer sent by the watchdog when `timed`
};

struct ExecJob {
    json id;              ///< JSON-RPC id; null = notification (no response, not cancellable by id)
    bool timed = false;   ///< tools/call: subject to the watchdog and the SERVER_BUSY guard
    /// Runs on the main thread. Returns the JSON-RPC message to deliver, or null for "no response".
    std::function<json(CallState&)> run;
    std::function<void(const json&)> deliver;
    std::function<json()> timeout_response;
};

enum class SubmitResult { Queued, QueueFull, Busy };

class CallExecutor {
public:
    CallExecutor(std::size_t max_queue, int call_timeout_ms)
        : max_queue_(max_queue), call_timeout_ms_(call_timeout_ms) {}
    ~CallExecutor() = default;
    CallExecutor(const CallExecutor&) = delete;
    CallExecutor& operator=(const CallExecutor&) = delete;

    /// Thread-safe. Busy: `job.timed` and a previous timed-out call is still running.
    /// QueueFull: the queue holds `max_queue` jobs. Otherwise the job is queued.
    SubmitResult submit(ExecJob job, std::shared_ptr<CallState>* state_out = nullptr);

    /// Future-based convenience: the promise is completed by deliver (null json when dropped) and,
    /// for a timed job, by the watchdog. On Busy/QueueFull the future is not valid.
    std::future<json> submit_future(ExecJob job, SubmitResult* result_out, std::shared_ptr<CallState>* state_out = nullptr);

    /// stdio notifications/cancelled: cancels the running call with this id (suppressing its
    /// response) or removes the still-queued job. Returns true when something matched.
    bool cancel_by_id(const json& id);
    /// HTTP disconnect: same, by state handle.
    void cancel(const std::shared_ptr<CallState>& state);

    /// Main-thread loop. Blocks until request_stop(); drops queued jobs (delivering null) on exit.
    void run();
    void request_stop();
    bool stopping() const { return stop_.load(); }
    std::size_t queued() const;

private:
    void watchdog_loop();

    const std::size_t max_queue_;
    const int call_timeout_ms_;

    mutable std::mutex mu_;
    std::condition_variable cv_main_;
    std::condition_variable cv_watchdog_;
    struct Queued {
        ExecJob job;
        std::shared_ptr<CallState> state;
    };
    std::deque<Queued> queue_;
    std::shared_ptr<CallState> running_;
    std::atomic<bool> stop_{false};
};

} // namespace fairyfly::mcp
