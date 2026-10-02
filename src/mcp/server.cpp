#include "include/mcp/server.h"

#include <exception>
#include <thread>

#include <spdlog/spdlog.h>

#include "include/mcp/call_executor.h"
#include "include/mcp/json_rpc.h"
#include "include/mcp/protocol_session.h"

namespace fairyfly::mcp {

namespace {

/// The stdio engine: one implicit ProtocolSession (legacy era) fed by a reader thread; the calling
/// (main) thread runs the CallExecutor loop. Behaviour is unchanged from the pre-split Engine.
class StdioEngine {
public:
    StdioEngine(Transport& transport, ToolProvider& provider, const ServerOptions& options)
        : transport_(transport), options_(options), session_(provider, options),
          executor_(options.max_queue, options.call_timeout_ms) {}

    int run() {
        std::thread reader([this] { reader_loop(); });
        executor_.run();
        // Main loop ended (EOF): the reader has already returned.
        reader.join();
        return 0;
    }

private:
    void send(const json& message) {
        if (message.is_null()) return;  // dropped job (shutdown)
        try {
            transport_.write_line(message.dump(-1, ' ', false, json::error_handler_t::replace));
        } catch (const std::exception& e) {
            spdlog::error("MCP write failed: {}", e.what());
        }
    }

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
        executor_.request_stop();
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
        enqueue(Pending{message.id, message.method, message.params}, true);
    }

    void enqueue(Pending pending, bool is_request) {
        const bool timed = is_request && pending.method == "tools/call";
        ExecJob job;
        job.id = pending.id;
        job.timed = timed;
        job.deliver = [this](const json& message) { send(message); };
        if (timed) {
            const json id = pending.id;
            if (pending.params.is_object() && pending.params.contains("name") && pending.params["name"].is_string())
                job.tool = pending.params["name"].get<std::string>();
            job.timeout_response = [id](const CallInfo& info) { return make_result(id, busy_call_result("CALL_TIMEOUT", info)); };
        }
        job.run = [this, pending = std::move(pending)](CallState& state) {
            return session_.process(pending, [&state] { return state.cancelled.load(); });
        };
        const json id = job.id;
        switch (executor_.submit(std::move(job))) {
        case SubmitResult::Queued:
            return;
        case SubmitResult::Busy:
            // The main thread is still stuck in the call that timed out.
            send(make_result(id, busy_call_result("SERVER_BUSY", executor_.running_info().value_or(CallInfo{}))));
            return;
        case SubmitResult::QueueFull:
            if (is_request) send(make_error(id, kServerBusy, "server busy", json{{"retry", true}}));
            return;
        }
    }

    void handle_cancel(const json& params) {
        if (!params.is_object() || !params.contains("requestId")) return;
        executor_.cancel_by_id(params["requestId"]);
    }

    Transport& transport_;
    const ServerOptions& options_;
    ProtocolSession session_;
    CallExecutor executor_;
};

} // namespace

int McpServer::run() {
    StdioEngine engine(transport_, provider_, options_);
    return engine.run();
}

} // namespace fairyfly::mcp
