#pragma once

#include <map>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>

#include "include/mcp/call_executor.h"

namespace fairyfly::mcp {

// One FIFO execution lane per immutable live session identity. The lane runner is
// deliberately independent of the command implementation: its job can call a
// session-owned STA handler or a private worker process without changing queue rules.
class SessionExecutorPool {
public:
    class Reservation {
    public:
        explicit Reservation(std::shared_ptr<std::promise<void>> release) : release_(std::move(release)) {}
        ~Reservation() { release(); }
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;
        void release() {
            if (release_) {
                try { release_->set_value(); } catch (...) {}
                release_.reset();
            }
        }
    private:
        std::shared_ptr<std::promise<void>> release_;
    };

    SessionExecutorPool(std::size_t max_sessions, std::size_t max_queue, int call_timeout_ms);
    ~SessionExecutorPool();
    SessionExecutorPool(const SessionExecutorPool&) = delete;
    SessionExecutorPool& operator=(const SessionExecutorPool&) = delete;

    std::future<json> submit_future(const std::string& session_identity, ExecJob job,
                                    SubmitResult* result_out = nullptr,
                                    std::shared_ptr<CallState>* state_out = nullptr);
    SubmitResult submit(const std::string& session_identity, ExecJob job,
                        std::shared_ptr<CallState>* state_out = nullptr);
    /// Enqueue a barrier behind current work. Later jobs on this lane wait until
    /// the returned reservation is released. Returns empty on timeout/shutdown.
    std::shared_ptr<Reservation> reserve(const std::string& lane_key,
                                         std::chrono::milliseconds timeout,
                                         const std::function<bool()>& cancelled = {});
    void cancel(const std::string& session_identity, const std::shared_ptr<CallState>& state);
    std::optional<CallInfo> running_info(const std::string& session_identity) const;
    std::size_t queued(const std::string& session_identity) const;
    /// Retire an empty lane after its session closes; never releases an in-flight call.
    bool retire_idle(const std::string& session_identity);
    void request_stop();
    /// Wait until every routed invocation has left its lane.
    void join();
    bool stopping() const;

private:
    struct Lane;
    std::shared_ptr<Lane> find_lane(const std::string& session_identity) const;

    const std::size_t max_sessions_;
    const std::size_t max_queue_;
    const int call_timeout_ms_;
    std::mutex join_mu_;
    mutable std::mutex mu_;
    bool stop_ = false;
    std::map<std::string, std::shared_ptr<Lane>> lanes_;
};

} // namespace fairyfly::mcp
