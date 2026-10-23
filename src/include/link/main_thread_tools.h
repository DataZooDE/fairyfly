#pragma once
// The desktop link (D10, `fairyfly link run`): libffl's agent receives tool calls on its worker threads, but
// fairyfly's ToolProvider may only run on the main (COM) thread, one call at a time. MainThreadTools queues every
// call and the main thread executes them in order (pump). Portable: depends only on libffl and nlohmann-json.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "link_agent.hpp"  // libffl

namespace fairyfly::link {

class MainThreadTools : public ffy::link::LocalTools {
public:
    /// MCP `tools` array (name, description, inputSchema, annotations).
    using ListFn = std::function<nlohmann::json()>;
    /// MCP tool result object ({"content": [...], "isError": bool, "structuredContent"?}).
    using CallFn = std::function<nlohmann::json(const std::string& name, const nlohmann::json& args, const ffy::link::CallInfo& info)>;

    MainThreadTools() = default;
    /// Main thread: what to run. Set before pumping (the dispatcher exists only inside the run loop).
    void set_handlers(ListFn list, CallFn call);

    /// Any thread (libffl's agent): queues the call and waits for the main thread. Throws ffy::Error on timeout
    /// (nothing ran) or after shutdown.
    nlohmann::json rpc(const std::string& method, const nlohmann::json& params, int timeout_ms, const ffy::link::CallInfo& info) override;
    bool per_call_read_only() const override { return true; }

    /// Any thread: queues `fn` to run on the main thread between calls; does not wait. False after shutdown.
    bool post(std::function<void()> fn);

    /// Main thread: runs queued calls until `stop` becomes true (checks it at least every 200 ms).
    void pump(const std::atomic<bool>& stop);
    /// Main thread: runs at most one queued call, waiting up to `wait`. True if one ran.
    bool pump_once(std::chrono::milliseconds wait);
    /// Refuses queued and future calls (pending callers get an error).
    void shutdown();

private:
    struct Job {
        std::string method;
        nlohmann::json params;
        ffy::link::CallInfo info;
        std::function<void()> fn;  ///< set: a posted task instead of an rpc
        nlohmann::json result;
        std::string error;
        bool done = false, abandoned = false;
    };
    nlohmann::json run(Job& job);
    std::mutex mu_;
    std::condition_variable work_cv_, done_cv_;
    std::deque<std::shared_ptr<Job>> queue_;
    ListFn list_;
    CallFn call_;
    bool shutdown_ = false;
};

}  // namespace fairyfly::link
