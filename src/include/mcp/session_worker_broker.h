#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "include/mcp/session_executor_pool.h"
#include "include/mcp/session_worker_process.h"

namespace fairyfly::mcp {

struct WorkerGateResult {
    bool allowed = true;
    std::string code;
    std::string message;
};

enum class BrokerSubmitResult { Queued, Busy, QueueFull, Rejected };

/// Owns bounded per-session queues and their private process workers. The
/// caller supplies current authorization/live-target checks at admission and
/// again on the lane immediately before the IPC request is sent.
class SessionWorkerBroker {
public:
    using Gate = std::function<WorkerGateResult(const WorkerCall&)>;
    using Completion = std::function<void(const WorkerCall&, const json& final_result)>;
    SessionWorkerBroker(std::wstring executable, std::wstring argument,
                        std::size_t max_sessions, std::size_t max_queue, int call_timeout_ms);
    ~SessionWorkerBroker();
    SessionWorkerBroker(const SessionWorkerBroker&) = delete;
    SessionWorkerBroker& operator=(const SessionWorkerBroker&) = delete;

    std::future<json> submit(WorkerCall call, Gate admission, Gate before_dispatch,
                             BrokerSubmitResult* status = nullptr, Completion completion = {});
    /// Read-only facts probe in the private child for a saved connection.
    /// A later submitted action binds that same child to a validated identity.
    json probe(int connection);
    /// One-shot owner discovery in a disposable process. A hung SAP COM getter
    /// is terminated at the caller's deadline and never holds a session lane.
    json enumerate_sessions(std::chrono::milliseconds budget,
                            const std::function<bool()>& cancelled = {});
    /// For an already serialized, authorized endpoint session lane. Caller
    /// must perform current token/lease/policy checks immediately before use.
    json invoke_direct(const WorkerCall& call, std::function<void()> before_send = {});
    /// After disconnect succeeds, remove only the worker still bound to that
    /// exact saved connection generation.
    bool retire_connection(int connection, const std::string& session_identity);
    bool retire_idle(const std::string& session_identity);
    void shutdown();
private:
    struct BoundProcess {
        std::string identity;
        std::string owner;
        std::shared_ptr<SessionWorkerProcess> process;
    };
    std::shared_ptr<SessionWorkerProcess> process_for(const WorkerCall& call);
    std::shared_ptr<SessionWorkerProcess> process_for_probe(int connection);
    std::wstring executable_;
    std::wstring argument_;
    std::size_t max_processes_;
    std::mutex lifecycle_mutex_;
    std::mutex mutex_;
    bool stopping_ = false;
    std::map<int, BoundProcess> processes_;
    std::shared_ptr<std::atomic<bool>> discovery_active_ = std::make_shared<std::atomic<bool>>(false);
    std::weak_ptr<SessionWorkerProcess> discovery_process_;
    SessionExecutorPool lanes_;
};

} // namespace fairyfly::mcp
