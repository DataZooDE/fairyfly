#include "include/link/main_thread_tools.h"

#include "util.hpp"  // libffl: ffy::Error

namespace fairyfly::link {

using json = nlohmann::json;

void MainThreadTools::set_handlers(ListFn list, CallFn call) {
    std::lock_guard<std::mutex> l(mu_);
    list_ = std::move(list);
    call_ = std::move(call);
}

json MainThreadTools::rpc(const std::string& method, const json& params, int timeout_ms, const ffy::link::CallInfo& info) {
    auto job = std::make_shared<Job>();
    job->method = method;
    job->params = params;
    job->info = info;
    std::unique_lock<std::mutex> l(mu_);
    if (shutdown_) throw ffy::Error("fairyfly link is stopping");
    queue_.push_back(job);
    work_cv_.notify_one();
    if (!done_cv_.wait_for(l, std::chrono::milliseconds(timeout_ms), [&] { return job->done || shutdown_; })) {
        job->abandoned = true;  // never started: the main thread skips it
        throw ffy::Error("fairyfly is busy: the call did not start within " + std::to_string(timeout_ms / 1000) + " s");
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
