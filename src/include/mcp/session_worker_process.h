#pragma once

#include <memory>
#include <functional>
#include <stdexcept>
#include <string>

#include "include/mcp/session_worker_protocol.h"

namespace fairyfly::mcp {

class WorkerTransportError : public std::runtime_error {
public:
    WorkerTransportError(std::string code, const std::string& message)
        : std::runtime_error(message), code_(std::move(code)) {}
    const std::string& code() const noexcept { return code_; }
private:
    std::string code_;
};

/// A private child of the interactive tray process. One instance belongs to one
/// serial session lane; it never reconnects after an uncertain call outcome.
class SessionWorkerProcess {
public:
    struct Impl;
    explicit SessionWorkerProcess(std::wstring executable, std::wstring argument = L"--mcp-session-worker");
    ~SessionWorkerProcess();
    SessionWorkerProcess(const SessionWorkerProcess&) = delete;
    SessionWorkerProcess& operator=(const SessionWorkerProcess&) = delete;

    /// Returns the worker's Result JSON. Transport failure after submission has
    /// code OUTCOME_UNKNOWN; callers must never replay a state-changing command.
    /// before_send runs under the process pipe mutex immediately before the
    /// request is written. It must not call this worker again.
    json invoke(const WorkerCall& call, std::function<void()> before_send = {});
    bool healthy() const noexcept;
    /// Safe from another thread while invoke is blocked in a COM call. The
    /// pending invocation then returns OUTCOME_UNKNOWN.
    void terminate();
private:
    std::unique_ptr<Impl> impl_;
};

} // namespace fairyfly::mcp
