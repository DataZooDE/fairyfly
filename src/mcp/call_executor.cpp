#include "include/mcp/call_executor.h"

#include <algorithm>
#include <exception>

#include <spdlog/spdlog.h>

#include "include/mcp/json_rpc.h"

namespace fairyfly::mcp {

using Clock = std::chrono::steady_clock;

namespace {
CallInfo info_of(const CallState& state, int timeout_ms) {
    CallInfo info;
    info.tool = state.tool;
    info.timeout_ms = timeout_ms;
    info.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - state.started).count();
    return info;
}
} // namespace

long long retry_after_ms_hint(const CallInfo& info) {
    const long long remaining = static_cast<long long>(info.timeout_ms) - info.elapsed_ms;
    if (remaining <= 0) return 5000;
    return std::max<long long>(500, std::min<long long>(5000, remaining));
}

json busy_call_result(const std::string& code, const CallInfo& info) {
    const long long retry = retry_after_ms_hint(info);
    const long long seconds = info.elapsed_ms / 1000;
    const std::string tool = info.tool.empty() ? std::string("a SAP call") : info.tool;
    std::string message;
    if (code == "CALL_TIMEOUT")
        message = "the SAP call " + tool + " is still running after " + std::to_string(seconds) +
                  " s; it cannot be interrupted and finishes in the background. Do not repeat it: wait about " +
                  std::to_string(retry) + " ms (retry_after_ms) and read the screen, or retry then.";
    else
        message = "the previous SAP call " + tool + " is still running (" + std::to_string(seconds) +
                  " s so far) and the session serves one call at a time. Retry in about " + std::to_string(retry) +
                  " ms (retry_after_ms).";
    json error = {{"code", code}, {"message", message}, {"running_tool", info.tool}, {"elapsed_ms", info.elapsed_ms},
                  {"timeout_ms", info.timeout_ms}, {"retry_after_ms", retry}};
    json result = {{"content", json::array({json{{"type", "text"}, {"text", code + ": " + message}}})},
                   {"structuredContent", json{{"status", "error"}, {"error", error}}},
                   {"isError", true}};
    return result;
}

SubmitResult CallExecutor::submit(ExecJob job, std::shared_ptr<CallState>* state_out) {
    auto state = std::make_shared<CallState>();
    state->id = job.id;
    state->deliver = job.deliver;
    state->timeout_response = job.timeout_response;
    state->tool = job.tool;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stop_) return SubmitResult::QueueFull;
        if (job.timed && running_ && running_->timed_out) return SubmitResult::Busy;
        if (queue_.size() >= max_queue_) return SubmitResult::QueueFull;
        if (state_out) *state_out = state;
        queue_.push_back(Queued{std::move(job), std::move(state)});
    }
    cv_main_.notify_all();
    return SubmitResult::Queued;
}

std::future<json> CallExecutor::submit_future(ExecJob job, SubmitResult* result_out,
                                              std::shared_ptr<CallState>* state_out) {
    auto promise = std::make_shared<std::promise<json>>();
    auto done = std::make_shared<std::atomic<bool>>(false);
    auto set_once = [promise, done](const json& message) {
        if (done->exchange(true)) return;
        promise->set_value(message);
    };
    std::future<json> future = promise->get_future();
    job.deliver = set_once;
    const SubmitResult result = submit(std::move(job), state_out);
    if (result_out) *result_out = result;
    if (result != SubmitResult::Queued) return {};
    return future;
}

bool CallExecutor::cancel_by_id(const json& id) {
    std::lock_guard<std::mutex> lock(mu_);
    if (running_ && running_->id == id) {
        running_->cancelled = true;
        running_->responded = true;  // suppress the response; the call finishes on its own
        return true;
    }
    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
        if (!it->job.id.is_null() && it->job.id == id) {
            queue_.erase(it);
            return true;
        }
    }
    return false;
}

void CallExecutor::cancel(const std::shared_ptr<CallState>& state) {
    if (!state) return;
    state->cancelled = true;
    std::lock_guard<std::mutex> lock(mu_);
    if (running_ == state) {
        state->responded = true;
        return;
    }
    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
        if (it->state == state) {
            queue_.erase(it);
            return;
        }
    }
}

std::optional<CallInfo> CallExecutor::running_info() const {
    std::lock_guard<std::mutex> lock(mu_);
    if (!running_) return std::nullopt;
    return info_of(*running_, call_timeout_ms_);
}

std::size_t CallExecutor::queued() const {
    std::lock_guard<std::mutex> lock(mu_);
    return queue_.size();
}

bool CallExecutor::idle() const {
    std::lock_guard<std::mutex> lock(mu_);
    return queue_.empty() && !active_;
}

void CallExecutor::request_stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    cv_main_.notify_all();
    cv_watchdog_.notify_all();
}

void CallExecutor::watchdog_loop() {
    std::unique_lock<std::mutex> lock(mu_);
    while (!stop_) {
        if (!running_ || running_->timed_out || running_->responded) {
            cv_watchdog_.wait(lock);
            continue;
        }
        std::shared_ptr<CallState> state = running_;
        if (Clock::now() < state->deadline) {
            cv_watchdog_.wait_until(lock, state->deadline);
            continue;
        }
        state->timed_out = true;
        state->responded = true;
        json response = state->timeout_response ? state->timeout_response(info_of(*state, call_timeout_ms_)) : json();
        auto deliver = state->deliver;
        lock.unlock();
        if (deliver && !response.is_null()) {
            try {
                deliver(response);
            } catch (const std::exception& e) {
                spdlog::error("MCP deliver failed: {}", e.what());
            }
        }
        lock.lock();
    }
}

void CallExecutor::run() {
    std::thread watchdog([this] { watchdog_loop(); });
    while (true) {
        Queued item;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_main_.wait(lock, [this] { return stop_.load() || !queue_.empty(); });
            if (stop_) {
                std::deque<Queued> dropped;
                dropped.swap(queue_);
                lock.unlock();
                for (auto& d : dropped)
                    if (d.job.deliver) {
                        try { d.job.deliver(json()); } catch (...) {}
                    }
                break;
            }
            item = std::move(queue_.front());
            queue_.pop_front();
            active_ = true;
            if (item.job.timed) {
                item.state->started = Clock::now();
                item.state->deadline = item.state->started + std::chrono::milliseconds(call_timeout_ms_);
                running_ = item.state;
            }
        }
        if (item.job.timed) cv_watchdog_.notify_all();

        json response;
        try {
            response = item.job.run(*item.state);
        } catch (const std::exception& e) {
            spdlog::error("MCP internal error: {}", e.what());
            if (!item.job.id.is_null()) response = make_error(item.job.id, kInternalError, "Internal error");
        } catch (...) {
            if (!item.job.id.is_null()) response = make_error(item.job.id, kInternalError, "Internal error");
        }

        bool answer = true;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (item.job.timed) {
                running_.reset();
                if (item.state->responded) answer = false;  // cancelled, or already answered as timed out
                else item.state->responded = true;
            }
        }
        if (item.job.timed) cv_watchdog_.notify_all();
        if (answer && item.job.deliver && !response.is_null()) {
            try {
                item.job.deliver(response);
            } catch (const std::exception& e) {
                spdlog::error("MCP deliver failed: {}", e.what());
            }
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            active_ = false;
        }
    }
    request_stop();
    watchdog.join();
}

} // namespace fairyfly::mcp
