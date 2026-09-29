#include "include/mcp/http_tray_runner.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>

#include "include/mcp/run_mcp.h"

namespace fairyfly::mcp {

namespace {

// IServerRunner for `mcp --http --tray`. run_blocking() owns the main (COM/STA) thread: it runs the HTTP server
// via run_mcp() and, after a tray "Stop", idles (server not listening) until Start/Restart/Quit. The IServerControl
// handed to the tray is a proxy that stays valid while the inner server is stopped.
class HttpTrayRunner final : public tray::IServerRunner, public IServerControl {
public:
    HttpTrayRunner(ServeOptions options, std::function<cli::CommandHandler&()> get_handler,
                   commands::GlobalOptions global, audit::AuditSink* sink,
                   std::function<cli::CommandHandler*()> peek, RunMcpFunction run_fn)
        : run_fn_(std::move(run_fn)), options_(std::move(options)), get_handler_(std::move(get_handler)), global_(std::move(global)), sink_(sink),
          peek_(std::move(peek)) {
        read_only_ = !options_.allow_write || options_.read_only;
    }

    int run_blocking() override {
        int exit_code = 0;
        std::unique_lock<std::mutex> lock(mutex_);
        while (!quit_) {
            if (!want_run_) {
                cv_.wait(lock, [&] { return quit_ || want_run_; });
                continue;
            }
            ServeOptions opts = options_;
            opts.read_only = read_only_.load();
            opts.allow_write = !opts.read_only;
            bool restart = false;
            HttpRunHooks hooks;
            hooks.restart_requested = &restart;
            hooks.keep_logging = true;
            hooks.on_control = [this](IServerControl& control) {
                std::lock_guard<std::mutex> guard(mutex_);
                inner_ = &control;
                if (!want_run_ || quit_) control.request_stop();  // stop arrived before the server was up
            };
            lock.unlock();
            exit_code = run_fn_(opts, get_handler_, global_, sink_, peek_, &hooks);
            lock.lock();
            inner_ = nullptr;
            if (exit_code != 0 && !quit_) {
                // could not start (port in use, invalid option): stay in the tray, idle, so the user sees the state
                last_error_ = "server exited with code " + std::to_string(exit_code);
                want_run_ = false;
            } else if (!restart) {
                want_run_ = false;
            } else {
                last_error_.clear();
            }
        }
        return exit_code;
    }

    IServerControl& control() override { return *this; }

    void request_quit() override {
        std::lock_guard<std::mutex> guard(mutex_);
        quit_ = true;
        if (inner_) inner_->request_stop();
        cv_.notify_all();
    }

    // IServerControl
    ServerStatus status() const override {
        std::lock_guard<std::mutex> guard(mutex_);
        if (inner_) return inner_->status();
        ServerStatus st;
        st.running = false;
        st.endpoint = "http://" + (options_.host.empty() ? std::string("127.0.0.1") : options_.host) + ":" +
                      std::to_string(options_.port == 0 ? 8383 : options_.port) + "/mcp";
        st.read_only = read_only_.load();
        if (!last_error_.empty()) st.warnings.push_back(last_error_);
        return st;
    }

    void set_read_only(bool read_only) override {
        read_only_ = read_only;
        std::lock_guard<std::mutex> guard(mutex_);
        if (inner_) inner_->set_read_only(read_only);
    }

    void request_stop() override {
        std::lock_guard<std::mutex> guard(mutex_);
        want_run_ = false;
        if (inner_) inner_->request_stop();
    }

    void request_start() override {
        std::lock_guard<std::mutex> guard(mutex_);
        if (inner_) return;  // already running
        want_run_ = true;
        cv_.notify_all();
    }

    void request_restart() override {
        std::lock_guard<std::mutex> guard(mutex_);
        want_run_ = true;
        if (inner_) inner_->request_restart();
        else cv_.notify_all();
    }

private:
    RunMcpFunction run_fn_;
    ServeOptions options_;
    std::function<cli::CommandHandler&()> get_handler_;
    commands::GlobalOptions global_;
    audit::AuditSink* sink_;
    std::function<cli::CommandHandler*()> peek_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    IServerControl* inner_ = nullptr;
    bool want_run_ = true;
    bool quit_ = false;
    std::string last_error_;
    std::atomic<bool> read_only_{true};
};

} // namespace

std::unique_ptr<tray::IServerRunner> make_http_tray_runner(const ServeOptions& options,
                                                           std::function<cli::CommandHandler&()> get_handler,
                                                           const commands::GlobalOptions& global, audit::AuditSink* sink,
                                                           std::function<cli::CommandHandler*()> peek) {
    return make_http_tray_runner(options, std::move(get_handler), global, sink, std::move(peek), RunMcpFunction(run_mcp));
}

std::unique_ptr<tray::IServerRunner> make_http_tray_runner(const ServeOptions& options,
                                                           std::function<cli::CommandHandler&()> get_handler,
                                                           const commands::GlobalOptions& global, audit::AuditSink* sink,
                                                           std::function<cli::CommandHandler*()> peek,
                                                           RunMcpFunction run_fn) {
    return std::make_unique<HttpTrayRunner>(options, std::move(get_handler), global, sink, std::move(peek),
                                            std::move(run_fn));
}

} // namespace fairyfly::mcp
