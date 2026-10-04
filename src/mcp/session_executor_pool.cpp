#include "include/mcp/session_executor_pool.h"

#include <algorithm>
#include <thread>
#include <vector>

namespace fairyfly::mcp {

struct SessionExecutorPool::Lane {
    Lane(std::size_t max_queue, int timeout_ms) : executor(max_queue, timeout_ms), thread([this] { executor.run(); }) {}
    ~Lane() { join(); }
    void join() {
        std::lock_guard<std::mutex> lock(join_mu);
        executor.request_stop();
        if (thread.joinable()) thread.join();
    }
    CallExecutor executor;
    std::thread thread;
    std::mutex join_mu;
};

SessionExecutorPool::SessionExecutorPool(std::size_t max_sessions, std::size_t max_queue, int call_timeout_ms)
    : max_sessions_(max_sessions), max_queue_(max_queue), call_timeout_ms_(call_timeout_ms) {}

SessionExecutorPool::~SessionExecutorPool() { join(); }

std::shared_ptr<SessionExecutorPool::Lane> SessionExecutorPool::find_lane(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = lanes_.find(key);
    return it == lanes_.end() ? nullptr : it->second;
}

std::future<json> SessionExecutorPool::submit_future(const std::string& key, ExecJob job,
                                                     SubmitResult* result_out,
                                                     std::shared_ptr<CallState>* state_out) {
    std::shared_ptr<Lane> retired;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stop_ || key.empty()) {
            if (result_out) *result_out = SubmitResult::QueueFull;
            return {};
        }
        const auto it = lanes_.find(key);
        std::shared_ptr<Lane> lane;
        if (it != lanes_.end()) lane = it->second;
        else {
            if (lanes_.size() >= max_sessions_) {
                for (auto old = lanes_.begin(); old != lanes_.end(); ++old) {
                    if (!old->second->executor.idle()) continue;
                    retired = std::move(old->second);
                    retired->executor.request_stop();
                    lanes_.erase(old);
                    break;
                }
                if (lanes_.size() >= max_sessions_) {
                    if (result_out) *result_out = SubmitResult::QueueFull;
                    return {};
                }
            }
            lane = std::make_shared<Lane>(max_queue_, call_timeout_ms_);
            lanes_.emplace(key, lane);
        }
        // Keep the pool lock through submit so retire_idle cannot remove this
        // identity and create a second lane before the job reaches its queue.
        return lane->executor.submit_future(std::move(job), result_out, state_out);
    }
}

SubmitResult SessionExecutorPool::submit(const std::string& key, ExecJob job,
                                          std::shared_ptr<CallState>* state_out) {
    std::shared_ptr<Lane> retired;
    std::lock_guard<std::mutex> lock(mu_);
    if (stop_ || key.empty()) return SubmitResult::QueueFull;
    const auto it = lanes_.find(key);
    std::shared_ptr<Lane> lane;
    if (it != lanes_.end()) lane = it->second;
    else {
        if (lanes_.size() >= max_sessions_) {
            for (auto old = lanes_.begin(); old != lanes_.end(); ++old) {
                if (!old->second->executor.idle()) continue;
                retired = std::move(old->second);
                retired->executor.request_stop();
                lanes_.erase(old);
                break;
            }
            if (lanes_.size() >= max_sessions_) return SubmitResult::QueueFull;
        }
        lane = std::make_shared<Lane>(max_queue_, call_timeout_ms_);
        lanes_.emplace(key, lane);
    }
    // Hold the pool lock through submit, matching submit_future's retirement rule.
    return lane->executor.submit(std::move(job), state_out);
}

std::shared_ptr<SessionExecutorPool::Reservation> SessionExecutorPool::reserve(
    const std::string& lane_key, std::chrono::milliseconds timeout,
    const std::function<bool()>& cancelled) {
    if (lane_key.empty() || timeout <= std::chrono::milliseconds::zero()) return {};
    auto entered = std::make_shared<std::promise<void>>();
    auto entered_future = entered->get_future();
    auto release = std::make_shared<std::promise<void>>();
    const auto release_signal = release->get_future().share();
    auto reservation = std::make_shared<Reservation>(release);
    ExecJob barrier;
    barrier.tool = "session control reservation";
    barrier.run = [entered, release_signal](CallState&) {
        entered->set_value();
        release_signal.wait();
        return json();
    };
    std::shared_ptr<CallState> state;
    if (submit(lane_key, std::move(barrier), &state) != SubmitResult::Queued) return {};
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool entered_lane = false;
    while (!stopping() && !(cancelled && cancelled())) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        if (entered_future.wait_for(std::min(remaining, std::chrono::milliseconds(50))) ==
            std::future_status::ready) {
            entered_lane = true;
            break;
        }
    }
    if (!entered_lane || stopping() || (cancelled && cancelled())) {
        reservation->release();
        cancel(lane_key, state);
        return {};
    }
    return reservation;
}

void SessionExecutorPool::cancel(const std::string& key, const std::shared_ptr<CallState>& state) {
    if (auto lane = find_lane(key)) lane->executor.cancel(state);
}

std::optional<CallInfo> SessionExecutorPool::running_info(const std::string& key) const {
    if (auto lane = find_lane(key)) return lane->executor.running_info();
    return std::nullopt;
}

std::size_t SessionExecutorPool::queued(const std::string& key) const {
    if (auto lane = find_lane(key)) return lane->executor.queued();
    return 0;
}

bool SessionExecutorPool::retire_idle(const std::string& key) {
    std::shared_ptr<Lane> retired;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = lanes_.find(key);
        if (it == lanes_.end() || !it->second->executor.idle()) return false;
        retired = std::move(it->second);
        retired->executor.request_stop();
        lanes_.erase(it);
    }
    // The lane thread can exit and join without holding the pool lock.
    retired.reset();
    return true;
}

void SessionExecutorPool::request_stop() {
    std::vector<std::shared_ptr<Lane>> lanes;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stop_) return;
        stop_ = true;
        for (const auto& entry : lanes_) lanes.push_back(entry.second);
    }
    for (const auto& lane : lanes) lane->executor.request_stop();
}

void SessionExecutorPool::join() {
    std::lock_guard<std::mutex> join_lock(join_mu_);
    request_stop();
    std::map<std::string, std::shared_ptr<Lane>> lanes;
    {
        std::lock_guard<std::mutex> lock(mu_);
        lanes.swap(lanes_);
    }
    // request_stop() may hold another shared_ptr to a lane; join explicitly.
    for (const auto& [identity, lane] : lanes) lane->join();
    lanes.clear();
}

bool SessionExecutorPool::stopping() const {
    std::lock_guard<std::mutex> lock(mu_);
    return stop_;
}

} // namespace fairyfly::mcp
