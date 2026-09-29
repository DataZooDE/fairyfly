#include "include/mcp/server.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>

#include <spdlog/spdlog.h>

#include "include/mcp/json_rpc.h"

namespace fairyfly::mcp {

namespace {

using Clock = std::chrono::steady_clock;

/// Shared state of one running tools/call. `responded` is the single arbiter of "who answers":
/// the main thread (normal completion), the watchdog (timeout) or the reader (cancellation,
/// which claims it without sending anything). Guarded by Engine::mu_.
struct CallState {
    json id;
    std::atomic<bool> cancelled{false};
    bool responded = false;
    bool timed_out = false;
    Clock::time_point deadline;
};

struct Pending {
    json id;
    std::string method;
    json params;
};

class Engine {
public:
    Engine(Transport& transport, ToolProvider& provider, const ServerOptions& options)
        : transport_(transport), provider_(provider), options_(options) {}

    int run() {
        std::thread reader([this] { reader_loop(); });
        std::thread watchdog([this] { watchdog_loop(); });
        main_loop();
        {
            std::lock_guard<std::mutex> lock(mu_);
            stop_ = true;
        }
        cv_watchdog_.notify_all();
        watchdog.join();
        reader.join();
        return 0;
    }

private:
    // ---- output -------------------------------------------------------------------------
    void send(const json& message) {
        try {
            transport_.write_line(message.dump(-1, ' ', false, json::error_handler_t::replace));
        } catch (const std::exception& e) {
            spdlog::error("MCP write failed: {}", e.what());
        }
    }

    static json text_result(const std::string& text, bool is_error) {
        json result{{"content", json::array({json{{"type", "text"}, {"text", text}}})}};
        if (is_error) result["isError"] = true;
        return result;
    }

    // ---- reader thread ------------------------------------------------------------------
    void reader_loop() {
        std::string line;
        while (true) {
            bool got = false;
            try {
                got = transport_.read_line(line);
            } catch (const std::exception& e) {
                spdlog::error("MCP read failed: {}", e.what());
            }
            if (!got) break;
            try {
                handle_line(line);
            } catch (const std::exception& e) {
                spdlog::error("MCP reader error: {}", e.what());
            }
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            eof_ = true;
        }
        cv_main_.notify_all();
    }

    void handle_line(const std::string& line) {
        json error;
        Message message = parse_message(line, error);
        switch (message.kind) {
        case Message::Kind::Invalid:
            if (!error.is_null()) send(error);
            return;
        case Message::Kind::Response:
            return;
        case Message::Kind::Notification:
            if (message.method == "notifications/cancelled") handle_cancel(message.params);
            else enqueue(Pending{nullptr, message.method, message.params}, false);
            return;
        case Message::Kind::Request:
            break;
        }

        if (message.method == "ping") {
            send(make_result(message.id, json::object()));
            return;
        }
        if (message.method == "tools/call") {
            std::lock_guard<std::mutex> lock(mu_);
            if (running_ && running_->timed_out) {
                // The main thread is still stuck in the call that timed out.
                send(make_result(message.id, text_result("SERVER_BUSY: a previous SAP call is still running; retry shortly", true)));
                return;
            }
        }
        enqueue(Pending{message.id, message.method, message.params}, true);
    }

    void enqueue(Pending pending, bool is_request) {
        bool overflow = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (queue_.size() >= options_.max_queue) overflow = true;
            else queue_.push_back(std::move(pending));
        }
        if (overflow) {
            if (is_request) {
                // pending was moved-from only when queued, so it is intact here.
                send(make_error(pending.id, kServerBusy, "server busy", json{{"retry", true}}));
            }
            return;
        }
        cv_main_.notify_all();
    }

    void handle_cancel(const json& params) {
        if (!params.is_object() || !params.contains("requestId")) return;
        const json& id = params["requestId"];
        std::lock_guard<std::mutex> lock(mu_);
        if (running_ && running_->id == id) {
            running_->cancelled = true;
            running_->responded = true;  // suppress the response; the call finishes on its own
            return;
        }
        for (auto it = queue_.begin(); it != queue_.end(); ++it) {
            if (!it->id.is_null() && it->id == id) {
                queue_.erase(it);
                return;
            }
        }
    }

    // ---- watchdog -----------------------------------------------------------------------
    void watchdog_loop() {
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
            json response = make_result(
                state->id, text_result("CALL_TIMEOUT: the SAP call is still running; retry after it completes", true));
            lock.unlock();
            send(response);
            lock.lock();
        }
    }

    // ---- main thread --------------------------------------------------------------------
    void main_loop() {
        while (true) {
            Pending pending;
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_main_.wait(lock, [this] { return eof_ || !queue_.empty(); });
                if (eof_) {
                    queue_.clear();
                    return;
                }
                pending = std::move(queue_.front());
                queue_.pop_front();
            }
            try {
                process(pending);
            } catch (const std::exception& e) {
                spdlog::error("MCP internal error in {}: {}", pending.method, e.what());
                if (!pending.id.is_null())
                    send(make_error(pending.id, kInternalError, "Internal error"));
            } catch (...) {
                if (!pending.id.is_null())
                    send(make_error(pending.id, kInternalError, "Internal error"));
            }
        }
    }

    void process(const Pending& p) {
        const bool is_request = !p.id.is_null();
        if (!is_request) {
            if (p.method == "notifications/initialized") ready_ = true;
            return;  // unknown notifications are ignored
        }

        if (p.method == "initialize") return send(handle_initialize(p));
        if (p.method == "tools/list" || p.method == "tools/call" || p.method == "logging/setLevel") {
            if (!initialized_)
                return send(make_error(p.id, kServerNotInitialized, "Server not initialized"));
            if (p.method == "tools/list") return send(handle_tools_list(p));
            if (p.method == "logging/setLevel") return send(handle_set_level(p));
            return handle_tools_call(p);
        }
        // server/discover and everything else: lets dual-era clients fall back to initialize.
        send(make_error(p.id, kMethodNotFound, "Method not found: " + p.method));
    }

    json handle_initialize(const Pending& p) {
        if (initialized_) return make_error(p.id, kInvalidRequest, "Server already initialized");
        if (!p.params.is_object() || !p.params.contains("protocolVersion") ||
            !p.params["protocolVersion"].is_string())
            return make_error(p.id, kInvalidParams, "initialize requires params.protocolVersion");
        const std::string requested = p.params["protocolVersion"].get<std::string>();
        std::string version = options_.protocol_versions.empty() ? std::string("2025-11-25")
                                                                 : options_.protocol_versions.front();
        for (const auto& supported : options_.protocol_versions)
            if (supported == requested) version = requested;

        if (p.params.contains("clientInfo") && p.params["clientInfo"].is_object()) {
            client_info_ = p.params["clientInfo"];
            try {
                provider_.set_client_info(client_info_);
            } catch (const std::exception& e) {
                spdlog::warn("set_client_info failed: {}", e.what());
            }
        }
        initialized_ = true;
        json result{
            {"protocolVersion", version},
            {"capabilities", json{{"tools", json{{"listChanged", false}}}, {"logging", json::object()}}},
            {"serverInfo", json{{"name", options_.name},
                                {"title", "fairyfly SAP GUI"},
                                {"version", options_.version},
                                {"description", "SAP GUI automation through the SAP GUI Scripting API"}}},
            {"instructions", options_.instructions}};
        return make_result(p.id, result);
    }

    static json tool_to_json(const ToolDef& tool) {
        json out{{"name", tool.name},
                 {"title", tool.title},
                 {"description", tool.description},
                 {"inputSchema", tool.input_schema.is_null() ? json{{"type", "object"}} : tool.input_schema}};
        if (!tool.annotations.is_null()) out["annotations"] = tool.annotations;
        if (tool.meta.is_object() && !tool.meta.empty()) out["_meta"] = tool.meta;
        return out;
    }

    json handle_tools_list(const Pending& p) {
        if (p.params.is_object() && p.params.contains("cursor"))
            return make_error(p.id, kInvalidParams, "Invalid cursor: pagination is not supported");
        json tools = json::array();
        for (const auto& tool : provider_.list_tools()) tools.push_back(tool_to_json(tool));
        return make_result(p.id, json{{"tools", tools}});
    }

    json handle_set_level(const Pending& p) {
        if (!p.params.is_object() || !p.params.contains("level") || !p.params["level"].is_string())
            return make_error(p.id, kInvalidParams, "logging/setLevel requires params.level");
        const std::string level = p.params["level"].get<std::string>();
        spdlog::level::level_enum mapped;
        if (level == "debug") mapped = spdlog::level::debug;
        else if (level == "info" || level == "notice") mapped = spdlog::level::info;
        else if (level == "warning") mapped = spdlog::level::warn;
        else if (level == "error") mapped = spdlog::level::err;
        else if (level == "critical" || level == "alert" || level == "emergency") mapped = spdlog::level::critical;
        else return make_error(p.id, kInvalidParams, "Invalid log level: " + level);
        spdlog::set_level(mapped);
        return make_result(p.id, json::object());
    }

    void handle_tools_call(const Pending& p) {
        if (!p.params.is_object() || !p.params.contains("name") || !p.params["name"].is_string())
            return send(make_error(p.id, kInvalidParams, "tools/call requires a string params.name"));
        json args = json::object();
        if (p.params.contains("arguments") && !p.params["arguments"].is_null()) {
            if (!p.params["arguments"].is_object())
                return send(make_error(p.id, kInvalidParams, "params.arguments must be an object"));
            args = p.params["arguments"];
        }
        const std::string name = p.params["name"].get<std::string>();
        bool known = false;
        for (const auto& tool : provider_.list_tools())
            if (tool.name == name) { known = true; break; }
        if (!known) return send(make_error(p.id, kInvalidParams, "Unknown tool: " + name));

        auto state = std::make_shared<CallState>();
        state->id = p.id;
        CallContext ctx;
        ctx.request_id = p.id;
        if (p.params.contains("_meta") && p.params["_meta"].is_object() &&
            p.params["_meta"].contains("progressToken"))
            ctx.progress_token = p.params["_meta"]["progressToken"];
        ctx.cancelled = [state] { return state->cancelled.load(); };

        {
            std::lock_guard<std::mutex> lock(mu_);
            state->deadline = Clock::now() + std::chrono::milliseconds(options_.call_timeout_ms);
            running_ = state;
        }
        cv_watchdog_.notify_all();

        ToolResult result;
        try {
            result = provider_.call_tool(name, args, ctx);
        } catch (const std::exception& e) {
            result = ToolResult{};
            result.content = json::array({json{{"type", "text"}, {"text", std::string("INTERNAL_ERROR: ") + e.what()}}});
            result.is_error = true;
        } catch (...) {
            result = ToolResult{};
            result.content = json::array({json{{"type", "text"}, {"text", "INTERNAL_ERROR: unknown exception"}}});
            result.is_error = true;
        }

        bool answer = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            running_.reset();
            if (!state->responded) {
                state->responded = true;
                answer = true;
            }
        }
        cv_watchdog_.notify_all();
        if (!answer) return;  // cancelled or already answered as timed out: drop the late result

        json out{{"content", result.content.is_array() ? result.content : json::array()}};
        if (result.structured) out["structuredContent"] = *result.structured;
        if (result.is_error) out["isError"] = true;
        send(make_result(p.id, out));
    }

    Transport& transport_;
    ToolProvider& provider_;
    const ServerOptions& options_;

    std::mutex mu_;
    std::condition_variable cv_main_;
    std::condition_variable cv_watchdog_;
    std::deque<Pending> queue_;
    std::shared_ptr<CallState> running_;
    bool eof_ = false;
    bool stop_ = false;

    // Main-thread only.
    bool initialized_ = false;
    bool ready_ = false;
    json client_info_;
};

} // namespace

int McpServer::run() {
    Engine engine(transport_, provider_, options_);
    return engine.run();
}

} // namespace fairyfly::mcp
