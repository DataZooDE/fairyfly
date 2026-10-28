#include "include/link/main_thread_tools.h"

#include <algorithm>
#include <ctime>

#include <spdlog/spdlog.h>

#include "util.hpp"  // libffl: ffy::Error

namespace fairyfly::link {

using json = nlohmann::json;

namespace {

/// The tools that exist to find out what is wrong: they must not wait 126 s behind a call SAP GUI does not answer.
bool diagnostic_tool(const std::string& tool) { return tool == "gui_doctor" || tool == "gui_session_list"; }

std::string local_hms(std::int64_t unix_s) {
    const std::time_t t = static_cast<std::time_t>(unix_s);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

constexpr const char* kNotRespondingHint =
    "Look in SAP GUI for a dialog or popup that waits for input, possibly hidden behind other windows, and answer or "
    "close it; ending the SAP session (or SAP GUI) also releases fairyfly. Until then every call waits.";

}  // namespace

std::string not_responding_text(const InFlightCall& call) {
    const std::string what = call.tool.empty() ? std::string("the audit trail's screen read") : call.tool;
    const std::string where = call.connection.empty() ? std::string() : " on connection " + call.connection;
    return "SAP GUI has not answered " + what + where + " for " + std::to_string(call.elapsed.count() / 1000) + " s (started " +
           local_hms(call.started_unix) + ")";
}

json not_responding_result(const std::string& tool, const InFlightCall& call) {
    const std::string message = not_responding_text(call) + "; " + tool + " did not run. " + kNotRespondingHint;
    json error = {{"code", "SAP_GUI_NOT_RESPONDING"},   {"message", message},
                  {"running_tool", call.tool},          {"connection", call.connection},
                  {"elapsed_ms", call.elapsed.count()}, {"started", local_hms(call.started_unix)},
                  {"hint", kNotRespondingHint}};
    return {{"content", json::array({json{{"type", "text"}, {"text", "SAP_GUI_NOT_RESPONDING: " + message}}})},
            {"structuredContent", json{{"status", "error"}, {"error", error}}},
            {"isError", true}};
}

json stuck_call_status(const std::optional<InFlightCall>& call, std::chrono::milliseconds after) {
    if (!call || call->elapsed < after) return nullptr;
    return {{"tool", call->tool},
            {"connection", call->connection},
            {"elapsed_s", std::chrono::duration_cast<std::chrono::seconds>(call->elapsed).count()},
            {"started", local_hms(call->started_unix)}};
}

void MainThreadTools::set_handlers(ListFn list, CallFn call) {
    std::lock_guard<std::mutex> l(mu_);
    list_ = std::move(list);
    call_ = std::move(call);
}

void MainThreadTools::set_write_permit(PermitFn permit) {
    std::lock_guard<std::mutex> l(mu_);
    permit_ = std::move(permit);
}

void MainThreadTools::set_clock(Clock now) {
    std::lock_guard<std::mutex> l(mu_);
    now_ = std::move(now);
}

void MainThreadTools::set_diagnostic_wait(std::chrono::milliseconds wait) {
    std::lock_guard<std::mutex> l(mu_);
    diagnostic_wait_ = wait;
}

InFlightCall MainThreadTools::in_flight_locked(const Job& job) const {
    InFlightCall c;
    c.tool = job.tool;
    c.connection = job.connection;
    c.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now_() - job.started);
    c.started_unix = job.started_unix;
    return c;
}

std::optional<InFlightCall> MainThreadTools::in_flight() const {
    std::lock_guard<std::mutex> l(mu_);
    if (!running_) return std::nullopt;
    return in_flight_locked(*running_);
}

json MainThreadTools::rpc(const std::string& method, const json& params, int timeout_ms, const ffy::link::CallInfo& info) {
    auto job = std::make_shared<Job>();
    job->method = method;
    job->params = params;
    job->info = info;
    PermitFn permit;
    {
        std::lock_guard<std::mutex> l(mu_);
        permit = permit_;
    }
    if (permit && method == "tools/call" && info.write_granted) {
        // here, on libffl's thread that asked the approver: the permit is thread-local
        const json args = params.contains("arguments") && params["arguments"].is_object() ? params["arguments"] : json::object();
        job->info.write_granted = permit(params.value("name", std::string()), args);
    }
    if (method == "tools/call") {
        job->tool = params.value("name", std::string());
        if (params.contains("arguments") && params["arguments"].is_object() && params["arguments"].contains("connection")) {
            const json& c = params["arguments"]["connection"];
            job->connection = c.is_string() ? c.get<std::string>() : c.dump();
        }
    }
    std::unique_lock<std::mutex> l(mu_);
    if (shutdown_) throw ffy::Error("fairyfly link is stopping");
    // A diagnostic tool never queues behind a call SAP GUI has not answered for the diagnostic wait: it answers from
    // what this queue knows. It never calls into SAP GUI from here (COM is bound to the main thread).
    const bool diagnostic = diagnostic_tool(job->tool);
    auto not_responding = [&] {
        const InFlightCall call = in_flight_locked(*running_);
        spdlog::warn("SAP_GUI_NOT_RESPONDING: {}; answered {} without running it", not_responding_text(call), job->tool);
        return json{{"jsonrpc", "2.0"}, {"id", 1}, {"result", not_responding_result(job->tool, call)}};
    };
    if (diagnostic && running_ && now_() - running_->started >= diagnostic_wait_) return not_responding();
    // Any other call behind one that is stuck (kStuckCallAfter, the tray is amber) fails at once instead of holding
    // one of libffl's few local slots for its whole time limit, where it would also keep the diagnostic tools out.
    if (method == "tools/call" && running_ && now_() - running_->started >= kStuckCallAfter)
        throw ffy::Error("fairyfly is busy: the call did not start; " + not_responding_text(in_flight_locked(*running_)) + ". " +
                         kNotRespondingHint);
    queue_.push_back(job);
    work_cv_.notify_one();
    const auto finished = [&] { return job->done || shutdown_; };
    auto budget = std::chrono::milliseconds(timeout_ms);
    if (diagnostic) {
        const auto wait = std::min(diagnostic_wait_, budget);
        if (!done_cv_.wait_for(l, wait, finished) && !job->running && running_) {
            job->abandoned = true;  // never started: the main thread skips it
            return not_responding();
        }
        budget -= wait;  // it runs (or nothing blocks it): wait for it as usual
    }
    if (!done_cv_.wait_for(l, budget, finished)) {
        // it ran all this time: SAP GUI has not answered it (it finishes in the background, if ever)
        if (job->running) throw ffy::Error("SAP_GUI_NOT_RESPONDING: " + not_responding_text(in_flight_locked(*job)) + ". " + kNotRespondingHint);
        job->abandoned = true;  // never started: the main thread skips it
        std::string message = "fairyfly is busy: the call did not start within " + std::to_string(timeout_ms / 1000) + " s";
        if (running_) message += "; " + not_responding_text(in_flight_locked(*running_)) + ". " + kNotRespondingHint;
        throw ffy::Error(message);
    }
    if (!job->done) throw ffy::Error("fairyfly link is stopping");
    if (!job->error.empty()) throw ffy::Error(job->error);
    return job->result;
}

bool MainThreadTools::post(std::function<void()> fn) {
    auto job = std::make_shared<Job>();
    job->fn = std::move(fn);
    {
        std::lock_guard<std::mutex> l(mu_);
        if (shutdown_) return false;
        queue_.push_back(job);
    }
    work_cv_.notify_one();
    return true;
}

bool MainThreadTools::invoke(std::function<void()> fn, int timeout_ms) {
    auto job = std::make_shared<Job>();
    job->fn = std::move(fn);
    std::unique_lock<std::mutex> l(mu_);
    if (shutdown_) return false;
    queue_.push_back(job);
    work_cv_.notify_one();
    if (!done_cv_.wait_for(l, std::chrono::milliseconds(timeout_ms), [&] { return job->done || shutdown_; })) {
        job->abandoned = true;  // not started yet: skipped
        return false;
    }
    return job->done && job->error.empty();
}

json MainThreadTools::run(Job& job) {
    if (job.fn) {
        job.fn();
        return json();
    }
    if (job.method == "tools/list") return {{"jsonrpc", "2.0"}, {"id", 1}, {"result", {{"tools", list_ ? list_() : json::array()}}}};
    if (job.method != "tools/call") return {{"jsonrpc", "2.0"}, {"id", 1}, {"error", {{"code", -32601}, {"message", "method not served over the link"}}}};
    if (!call_) throw ffy::Error("fairyfly link: no dispatcher");
    json args = job.params.contains("arguments") && job.params["arguments"].is_object() ? job.params["arguments"] : json::object();
    return {{"jsonrpc", "2.0"}, {"id", 1}, {"result", call_(job.params.value("name", std::string()), args, job.info)}};
}

bool MainThreadTools::pump_once(std::chrono::milliseconds wait) {
    std::shared_ptr<Job> job;
    {
        std::unique_lock<std::mutex> l(mu_);
        work_cv_.wait_for(l, wait, [&] { return !queue_.empty() || shutdown_; });
        while (!queue_.empty() && queue_.front()->abandoned) queue_.pop_front();
        if (queue_.empty() || shutdown_) return false;
        job = queue_.front();
        queue_.pop_front();
        job->running = true;
        job->started = now_();
        job->started_unix = static_cast<std::int64_t>(std::time(nullptr));
        running_ = job;
    }
    json result;
    std::string error;
    try {
        result = run(*job);  // on this (the main, COM) thread, outside the lock
    } catch (const std::exception& e) {
        error = e.what();
    }
    {
        std::lock_guard<std::mutex> l(mu_);
        job->result = std::move(result);
        job->error = std::move(error);
        job->done = true;
        running_.reset();
    }
    done_cv_.notify_all();
    return true;
}

void MainThreadTools::pump(const std::atomic<bool>& stop) {
    while (!stop) pump_once(std::chrono::milliseconds(200));
}

void MainThreadTools::shutdown() {
    {
        std::lock_guard<std::mutex> l(mu_);
        shutdown_ = true;
        queue_.clear();
    }
    work_cv_.notify_all();
    done_cv_.notify_all();
}

}  // namespace fairyfly::link
