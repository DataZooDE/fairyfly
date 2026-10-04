#include "include/mcp/session_worker_broker.h"

#include <stdexcept>
#include <thread>
#include <utility>

namespace fairyfly::mcp {
namespace {

json error(const std::string& code, const std::string& message) {
    return {{"status", "error"}, {"error", {{"code", code}, {"message", message}}}};
}

std::future<json> ready(json value) {
    std::promise<json> promise;
    auto future = promise.get_future();
    promise.set_value(std::move(value));
    return future;
}

WorkerGateResult check(const SessionWorkerBroker::Gate& gate, const WorkerCall& call,
                       const char* failure_code) {
    if (!gate) return {false, failure_code, "session policy check is unavailable"};
    try { return gate(call); }
    catch (...) { return {false, failure_code, "session policy check failed"}; }
}

} // namespace

SessionWorkerBroker::SessionWorkerBroker(std::wstring executable, std::wstring argument,
                                         std::size_t max_sessions, std::size_t max_queue,
                                         int call_timeout_ms)
    : executable_(std::move(executable)), argument_(std::move(argument)),
      max_processes_(max_sessions),
      lanes_(max_sessions, max_queue, call_timeout_ms) {}

SessionWorkerBroker::~SessionWorkerBroker() { shutdown(); }

std::shared_ptr<SessionWorkerProcess> SessionWorkerBroker::process_for(const WorkerCall& call) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) throw WorkerTransportError("WORKER_UNAVAILABLE", "session worker broker is stopping");
    const auto found = processes_.find(call.connection);
    if (found != processes_.end()) {
        const bool changed = !found->second.identity.empty() &&
            (found->second.identity != call.session_identity || found->second.owner != call.owner_identity);
        if (changed || !found->second.process->healthy()) {
            if (found->second.process.use_count() != 1)
                throw WorkerTransportError("WORKER_SESSION_MISMATCH", "previous session worker is still active");
            found->second.process->terminate();
            processes_.erase(found);
        } else {
            if (found->second.identity.empty()) {
                found->second.identity = call.session_identity;
                found->second.owner = call.owner_identity;
            }
            return found->second.process;
        }
    }
    if (processes_.size() >= max_processes_) {
        for (auto it = processes_.begin(); it != processes_.end(); ++it) {
            if (it->second.process.use_count() != 1) continue;
            it->second.process->terminate();
            processes_.erase(it);
            break;
        }
    }
    if (processes_.size() >= max_processes_)
        throw WorkerTransportError("WORKER_UNAVAILABLE", "maximum session workers are active");
    auto process = std::make_shared<SessionWorkerProcess>(executable_, argument_);
    processes_.emplace(call.connection,
                       BoundProcess{call.session_identity, call.owner_identity, process});
    return process;
}

std::shared_ptr<SessionWorkerProcess> SessionWorkerBroker::process_for_probe(int connection) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) throw WorkerTransportError("WORKER_UNAVAILABLE", "session worker broker is stopping");
    if (connection < -1 || connection > 1000000)
        throw WorkerTransportError("WORKER_BAD_REQUEST", "invalid saved connection");
    if (const auto found = processes_.find(connection); found != processes_.end()) {
        if (found->second.process->healthy()) return found->second.process;
        if (found->second.process.use_count() != 1)
            throw WorkerTransportError("WORKER_UNAVAILABLE", "failed session worker is still active");
        found->second.process->terminate();
        processes_.erase(found);
    }
    if (processes_.size() >= max_processes_) {
        for (auto it = processes_.begin(); it != processes_.end(); ++it) {
            if (it->second.process.use_count() != 1) continue;
            it->second.process->terminate();
            processes_.erase(it);
            break;
        }
    }
    if (processes_.size() >= max_processes_)
        throw WorkerTransportError("WORKER_UNAVAILABLE", "maximum session workers are active");
    auto process = std::make_shared<SessionWorkerProcess>(executable_, argument_);
    processes_.emplace(connection, BoundProcess{"", "", process});
    return process;
}

json SessionWorkerBroker::probe(int connection) {
    WorkerCall call;
    call.connection = connection;
    call.probe = true;
    return process_for_probe(connection)->invoke(call);
}

json SessionWorkerBroker::enumerate_sessions(std::chrono::milliseconds budget,
                                               const std::function<bool()>& cancelled) {
    if (budget <= std::chrono::milliseconds::zero() ||
        discovery_active_->exchange(true))
        return error("OWNER_IDENTITY_UNKNOWN", "SAP session discovery is unavailable");
    const auto active = discovery_active_;
    std::shared_ptr<SessionWorkerProcess> process;
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            active->store(false);
            return error("OWNER_IDENTITY_UNKNOWN", "SAP session discovery is unavailable");
        }
        process = std::make_shared<SessionWorkerProcess>(executable_, argument_);
        discovery_process_ = process;
    } catch (...) {
        active->store(false);
        return error("OWNER_IDENTITY_UNKNOWN", "SAP session discovery is unavailable");
    }
    WorkerCall call;
    call.enumerate = true;
    auto promise = std::make_shared<std::promise<json>>();
    auto result = promise->get_future();
    std::thread worker;
    try {
        worker = std::thread([process, active, promise, call] {
            try { promise->set_value(process->invoke(call)); }
            catch (...) { try { promise->set_value(error("OWNER_IDENTITY_UNKNOWN", "SAP session discovery failed")); } catch (...) {} }
            active->store(false);
        });
    } catch (...) {
        active->store(false);
        return error("OWNER_IDENTITY_UNKNOWN", "SAP session discovery is unavailable");
    }
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline && !(cancelled && cancelled())) {
        if (result.wait_for(std::chrono::milliseconds(20)) == std::future_status::ready) {
            worker.join();
            return result.get();
        }
    }
    process->terminate();
    if (result.wait_for(std::chrono::milliseconds(250)) == std::future_status::ready) worker.join();
    else worker.detach(); // the active flag stays set until the worker actually exits
    return error("OWNER_IDENTITY_UNKNOWN", "SAP session discovery did not finish in time");
}

json SessionWorkerBroker::invoke_direct(const WorkerCall& call, std::function<void()> before_send) {
    if (call.probe) throw WorkerTransportError("WORKER_BAD_REQUEST", "probe is not a session action");
    try { (void)encode_worker_call(call); }
    catch (...) { throw WorkerTransportError("WORKER_BAD_REQUEST", "invalid session worker command"); }
    return process_for(call)->invoke(call, std::move(before_send));
}

bool SessionWorkerBroker::retire_connection(int connection, const std::string& session_identity) {
    std::shared_ptr<SessionWorkerProcess> retired;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = processes_.find(connection);
        if (found == processes_.end() || found->second.identity != session_identity) return false;
        retired = std::move(found->second.process);
        processes_.erase(found);
    }
    retired->terminate();
    return true;
}

std::future<json> SessionWorkerBroker::submit(WorkerCall call, Gate admission,
                                                Gate before_dispatch, BrokerSubmitResult* status,
                                                Completion completion) {
    try { (void)encode_worker_call(call); }
    catch (...) {
        if (status) *status = BrokerSubmitResult::Rejected;
        return ready(error("WORKER_BAD_REQUEST", "invalid session worker command"));
    }
    const WorkerGateResult admitted = check(admission, call, "OWNER_SESSION_UNAVAILABLE");
    if (!admitted.allowed) {
        if (status) *status = BrokerSubmitResult::Rejected;
        return ready(error(admitted.code.empty() ? "OWNER_SESSION_UNAVAILABLE" : admitted.code,
                           admitted.message.empty() ? "session unavailable" : admitted.message));
    }

    ExecJob job;
    const std::string key = call.session_identity;
    job.id = call.id;
    job.timed = true;
    job.tool = call.argv[0] + " " + call.argv[1];
    job.timeout_response = [](const CallInfo&) {
        return error("CALL_TIMEOUT", "session worker is still running the command");
    };
    job.run = [this, call = std::move(call), gate = std::move(before_dispatch),
               completion = std::move(completion)](CallState& state) {
        const auto finish = [&](json result) {
            if (completion) { try { completion(call, result); } catch (...) {} }
            return result;
        };
        if (state.cancelled.load()) return finish(error("CANCELLED", "session call was cancelled"));
        const WorkerGateResult allowed = check(gate, call, "AUTH_UNAVAILABLE");
        if (!allowed.allowed)
            return finish(error(allowed.code.empty() ? "AUTH_UNAVAILABLE" : allowed.code,
                                allowed.message.empty() ? "authorization changed before execution" : allowed.message));
        try { return finish(process_for(call)->invoke(call)); }
        catch (const WorkerTransportError& failure) { return finish(error(failure.code(), failure.what())); }
        catch (...) { return finish(error("OUTCOME_UNKNOWN", "session worker command outcome is unknown")); }
    };
    SubmitResult lane_status = SubmitResult::QueueFull;
    std::future<json> future;
    {
        std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
        future = lanes_.submit_future(key, std::move(job), &lane_status);
    }
    if (status)
        *status = lane_status == SubmitResult::Queued ? BrokerSubmitResult::Queued
                : lane_status == SubmitResult::Busy ? BrokerSubmitResult::Busy
                : BrokerSubmitResult::QueueFull;
    return future;
}

bool SessionWorkerBroker::retire_idle(const std::string& identity) {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    if (!lanes_.retire_idle(identity)) return false;
    std::shared_ptr<SessionWorkerProcess> retired;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto found = processes_.begin(); found != processes_.end(); ++found) {
            if (found->second.identity != identity) continue;
            retired = std::move(found->second.process);
            processes_.erase(found);
            break;
        }
    }
    if (retired) retired->terminate();
    return true;
}

void SessionWorkerBroker::shutdown() {
    lanes_.request_stop();
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    if (auto discovery = discovery_process_.lock()) discovery->terminate();
    for (const auto& [identity, bound] : processes_) bound.process->terminate();
}

} // namespace fairyfly::mcp
