#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <functional>
#include <optional>
#include <spdlog/spdlog.h>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "include/mcp/json_rpc.h"
#include "include/mcp/server.h"
#include "include/mcp/transport.h"

using namespace fairyfly::mcp;
using namespace std::chrono_literals;

namespace {

/// Blocking in-memory transport: the test feeds lines and waits for responses.
class PipeTransport : public Transport {
public:
    bool read_line(std::string& line) override {
        std::unique_lock<std::mutex> lock(m_);
        cv_in_.wait(lock, [this] { return closed_ || !in_.empty(); });
        if (in_.empty()) return false;
        line = std::move(in_.front());
        in_.pop_front();
        return true;
    }
    void write_line(std::string_view text) override {
        std::lock_guard<std::mutex> lock(m_);
        out_.emplace_back(text);
        cv_out_.notify_all();
    }
    void feed(const std::string& line) {
        std::lock_guard<std::mutex> lock(m_);
        in_.push_back(line);
        cv_in_.notify_all();
    }
    void close() {
        std::lock_guard<std::mutex> lock(m_);
        closed_ = true;
        cv_in_.notify_all();
    }
    /// Waits for the response with this id; returns null json on timeout.
    json wait_for(const json& id, std::chrono::milliseconds timeout = 5000ms) {
        std::unique_lock<std::mutex> lock(m_);
        json found;
        cv_out_.wait_for(lock, timeout, [&] {
            for (const auto& raw : out_) {
                json j = json::parse(raw, nullptr, false);
                if (j.is_object() && j.contains("id") && j["id"] == id) { found = j; return true; }
            }
            return false;
        });
        return found;
    }
    std::vector<std::string> lines() {
        std::lock_guard<std::mutex> lock(m_);
        return std::vector<std::string>(out_.begin(), out_.end());
    }
    int count_id(const json& id) {
        int n = 0;
        for (const auto& raw : lines()) {
            json j = json::parse(raw, nullptr, false);
            if (j.is_object() && j.contains("id") && j["id"] == id) ++n;
        }
        return n;
    }
private:
    std::mutex m_;
    std::condition_variable cv_in_, cv_out_;
    std::deque<std::string> in_;
    std::vector<std::string> out_;
    bool closed_ = false;
};

class FakeProvider : public ToolProvider {
public:
    std::vector<ToolDef> list_tools() const override {
        ToolDef echo;
        echo.name = "echo";
        echo.title = "Echo";
        echo.description = "echoes";
        echo.input_schema = json{{"type", "object"}};
        echo.annotations = json{{"readOnlyHint", true}};
        ToolDef slow;
        slow.name = "slow";
        slow.title = "Slow";
        slow.description = "blocks until released";
        slow.input_schema = json{{"type", "object"}};
        slow.meta = json{{"k", "v"}};
        ToolDef boom;
        boom.name = "boom";
        boom.title = "Boom";
        boom.description = "throws";
        boom.input_schema = json{{"type", "object"}};
        return {echo, slow, boom};
    }
    ToolResult call_tool(const std::string& name, const json& args, const CallContext& ctx) override {
        ++calls;
        if (name == "boom") throw std::runtime_error("kaboom");
        ToolResult r;
        if (name == "slow") {
            {
                std::unique_lock<std::mutex> lock(m);
                started = true;
                cv.notify_all();
                for (int i = 0; i < 1000 && !released && !(ctx.cancelled && ctx.cancelled()); ++i) cv.wait_for(lock, 10ms);
                if (ctx.cancelled && ctx.cancelled()) saw_cancel = true;
                finished = true;
                cv.notify_all();
            }
            r.content = json::array({json{{"type", "text"}, {"text", "slow done"}}});
            return r;
        }
        last_request_id = ctx.request_id;
        last_progress = ctx.progress_token;
        r.content = json::array({json{{"type", "text"}, {"text", args.dump()}}});
        r.structured = json{{"ok", true}};
        return r;
    }
    void set_client_info(const json& info) override { client_info = info; }

    bool wait_started() {
        std::unique_lock<std::mutex> lock(m);
        return cv.wait_for(lock, 5s, [&] { return started; });
    }
    bool wait_finished() {
        std::unique_lock<std::mutex> lock(m);
        return cv.wait_for(lock, 5s, [&] { return finished; });
    }
    void release() {
        std::lock_guard<std::mutex> lock(m);
        released = true;
        cv.notify_all();
    }

    std::mutex m;
    std::condition_variable cv;
    bool started = false, released = false, finished = false, saw_cancel = false;
    std::atomic<int> calls{0};
    json client_info;
    json last_request_id;
    std::optional<json> last_progress;
};

struct Harness {
    explicit Harness(ServerOptions opts = defaults()) : options(std::move(opts)) {
        thread = std::thread([this] {
            McpServer server(transport, provider, options);
            exit_code = server.run();
        });
    }
    ~Harness() { finish(); }
    static ServerOptions defaults() {
        ServerOptions o;
        o.version = "9.9.9";
        o.instructions = "be careful";
        return o;
    }
    int finish() {
        transport.close();
        if (thread.joinable()) thread.join();
        return exit_code;
    }
    static std::string req(const json& id, const std::string& method, const json& params = nullptr) {
        json m{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
        if (!params.is_null()) m["params"] = params;
        return m.dump();
    }
    static std::string note(const std::string& method, const json& params = nullptr) {
        json m{{"jsonrpc", "2.0"}, {"method", method}};
        if (!params.is_null()) m["params"] = params;
        return m.dump();
    }
    json call(const json& id, const std::string& method, const json& params = nullptr) {
        transport.feed(req(id, method, params));
        return transport.wait_for(id);
    }
    json init(const std::string& version = "2025-06-18") {
        json r = call(1000, "initialize",
                      json{{"protocolVersion", version},
                           {"capabilities", json::object()},
                           {"clientInfo", json{{"name", "tester"}, {"version", "1"}}}});
        transport.feed(note("notifications/initialized"));
        return r;
    }
    json tool(const json& id, const std::string& name, const json& args = json::object()) {
        return call(id, "tools/call", json{{"name", name}, {"arguments", args}});
    }
    /// Ensures the reader has handled everything fed so far (it processes lines in order).
    void sync() {
        static std::atomic<int> counter{900000};
        const int id = counter++;
        REQUIRE(call(id, "ping").is_object());
    }

    PipeTransport transport;
    FakeProvider provider;
    ServerOptions options;
    std::thread thread;
    std::atomic<int> exit_code{-1};
};

std::string text_of(const json& response) { return response["result"]["content"][0]["text"].get<std::string>(); }

} // namespace

TEST_CASE("MCP handshake echoes every supported protocol version", "[mcp][protocol]") {
    for (const std::string version : {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"}) {
        INFO(version);
        Harness h;
        json r = h.init(version);
        REQUIRE(r.contains("result"));
        CHECK(r["result"]["protocolVersion"] == version);
        CHECK(r["result"]["capabilities"]["tools"]["listChanged"] == false);
        CHECK(r["result"]["capabilities"].contains("logging"));
        CHECK(r["result"]["serverInfo"]["name"] == "fairyfly");
        CHECK(r["result"]["serverInfo"]["title"] == "fairyfly SAP GUI");
        CHECK(r["result"]["serverInfo"]["version"] == "9.9.9");
        CHECK(r["result"]["serverInfo"].contains("description"));
        CHECK(r["result"]["instructions"] == "be careful");
    }
}

TEST_CASE("MCP handshake answers an unsupported version with the newest", "[mcp][protocol]") {
    Harness h;
    json r = h.init("1999-01-01");
    CHECK(r["result"]["protocolVersion"] == "2025-11-25");
    h.sync();
    CHECK(h.provider.client_info["name"] == "tester");
}

TEST_CASE("MCP initialize twice and invalid initialize params", "[mcp][protocol]") {
    Harness h;
    json bad = h.call(1, "initialize", json{{"capabilities", json::object()}});
    CHECK(bad["error"]["code"] == kInvalidParams);
    h.init();
    json again = h.call(2, "initialize", json{{"protocolVersion", "2025-06-18"}});
    CHECK(again["error"]["code"] == kInvalidRequest);
}

TEST_CASE("MCP requests before initialize are refused", "[mcp][protocol]") {
    Harness h;
    CHECK(h.call(1, "tools/list")["error"]["code"] == kServerNotInitialized);
    CHECK(h.call(2, "tools/call", json{{"name", "echo"}})["error"]["code"] == kServerNotInitialized);
    CHECK(h.call(3, "logging/setLevel", json{{"level", "info"}})["error"]["code"] == kServerNotInitialized);
    CHECK(h.call(4, "ping")["result"] == json::object());
    CHECK(h.call(5, "server/discover")["error"]["code"] == kMethodNotFound);
    CHECK(h.provider.calls == 0);
}

TEST_CASE("MCP ping works at any time", "[mcp][protocol]") {
    Harness h;
    CHECK(h.call(1, "ping")["result"] == json::object());
    h.init();
    CHECK(h.call("p", "ping")["result"] == json::object());
}

TEST_CASE("MCP ping is answered while a tool call is running", "[mcp][protocol]") {
    Harness h;
    h.init();
    h.transport.feed(Harness::req(1, "tools/call", json{{"name", "slow"}}));
    REQUIRE(h.provider.wait_started());
    CHECK(h.call(2, "ping")["result"] == json::object());
    CHECK(h.transport.count_id(1) == 0);
    h.provider.release();
    json done = h.transport.wait_for(1);
    REQUIRE(done.is_object());
    CHECK(text_of(done) == "slow done");
}

TEST_CASE("MCP logging/setLevel", "[mcp][protocol]") {
    Harness h;
    h.init();
    for (const char* level : {"debug", "info", "notice", "warning", "error", "critical", "alert", "emergency"}) {
        INFO(level);
        CHECK(h.call(std::string("l-") + level, "logging/setLevel", json{{"level", level}})["result"] == json::object());
    }
    CHECK(h.call(50, "logging/setLevel", json{{"level", "loud"}})["error"]["code"] == kInvalidParams);
    CHECK(h.call(51, "logging/setLevel", json{{"level", 3}})["error"]["code"] == kInvalidParams);
    CHECK(h.call(52, "logging/setLevel")["error"]["code"] == kInvalidParams);
    spdlog::set_level(spdlog::level::info);  // do not leak a debug/critical level into other tests
}

TEST_CASE("MCP tools/list shape and cursor rejection", "[mcp][protocol]") {
    Harness h;
    h.init();
    json r = h.call(1, "tools/list");
    REQUIRE(r["result"]["tools"].is_array());
    CHECK_FALSE(r["result"].contains("nextCursor"));
    REQUIRE(r["result"]["tools"].size() == 3);
    const json& echo = r["result"]["tools"][0];
    CHECK(echo["name"] == "echo");
    CHECK(echo["title"] == "Echo");
    CHECK(echo["description"] == "echoes");
    CHECK(echo["inputSchema"]["type"] == "object");
    CHECK(echo["annotations"]["readOnlyHint"] == true);
    CHECK_FALSE(echo.contains("_meta"));
    const json& slow = r["result"]["tools"][1];
    CHECK_FALSE(slow.contains("annotations"));
    CHECK(slow["_meta"]["k"] == "v");

    CHECK(h.call(2, "tools/list", json{{"cursor", "abc"}})["error"]["code"] == kInvalidParams);
    CHECK(h.call(3, "tools/list", json::object())["result"].contains("tools"));
}

TEST_CASE("MCP tools/call validates parameters", "[mcp][protocol]") {
    Harness h;
    h.init();
    CHECK(h.call(1, "tools/call")["error"]["code"] == kInvalidParams);
    CHECK(h.call(2, "tools/call", json{{"arguments", json::object()}})["error"]["code"] == kInvalidParams);
    CHECK(h.call(3, "tools/call", json{{"name", 5}})["error"]["code"] == kInvalidParams);
    CHECK(h.call(4, "tools/call", json{{"name", "echo"}, {"arguments", "no"}})["error"]["code"] == kInvalidParams);
    json unknown = h.call(5, "tools/call", json{{"name", "nope"}});
    CHECK(unknown["error"]["code"] == kInvalidParams);
    CHECK(unknown["error"]["message"] == "Unknown tool: nope");
    CHECK(h.provider.calls == 0);
}

TEST_CASE("MCP tools/call result mapping", "[mcp][protocol]") {
    Harness h;
    h.init();
    json r = h.call(1, "tools/call", json{{"name", "echo"}, {"arguments", json{{"a", 1}}}, {"_meta", json{{"progressToken", "tok"}}}});
    REQUIRE(r.contains("result"));
    CHECK(r["result"]["content"][0]["type"] == "text");
    CHECK(text_of(r) == "{\"a\":1}");
    CHECK(r["result"]["structuredContent"]["ok"] == true);
    CHECK_FALSE(r["result"].contains("isError"));
    CHECK(h.provider.last_request_id == 1);
    REQUIRE(h.provider.last_progress.has_value());
    CHECK(*h.provider.last_progress == "tok");

    // Missing arguments default to {}.
    json d = h.call(2, "tools/call", json{{"name", "echo"}});
    CHECK(text_of(d) == "{}");
}

TEST_CASE("MCP provider exceptions become isError results", "[mcp][protocol]") {
    Harness h;
    h.init();
    json r = h.tool(1, "boom");
    REQUIRE(r.contains("result"));
    CHECK(r["result"]["isError"] == true);
    CHECK(text_of(r) == "INTERNAL_ERROR: kaboom");
    CHECK(h.call(2, "ping").contains("result"));  // server survived
}

TEST_CASE("MCP cancel of a queued request removes it silently", "[mcp][protocol]") {
    Harness h;
    h.init();
    h.transport.feed(Harness::req(1, "tools/call", json{{"name", "slow"}}));
    REQUIRE(h.provider.wait_started());
    h.transport.feed(Harness::req(2, "tools/call", json{{"name", "echo"}}));
    h.transport.feed(Harness::note("notifications/cancelled", json{{"requestId", 2}}));
    h.sync();
    h.provider.release();
    REQUIRE(h.transport.wait_for(1).is_object());
    json third = h.tool(3, "echo");
    REQUIRE(third.is_object());
    CHECK(h.transport.count_id(2) == 0);
    CHECK(h.provider.calls == 2);  // slow + third echo; the cancelled one never ran
}

TEST_CASE("MCP cancel of the running request suppresses its response", "[mcp][protocol]") {
    Harness h;
    h.init();
    h.transport.feed(Harness::req(1, "tools/call", json{{"name", "slow"}}));
    REQUIRE(h.provider.wait_started());
    h.transport.feed(Harness::note("notifications/cancelled", json{{"requestId", 1}}));
    REQUIRE(h.provider.wait_finished());  // the tool observed ctx.cancelled() and returned
    CHECK(h.provider.saw_cancel);
    json next = h.tool(2, "echo");
    REQUIRE(next.is_object());
    CHECK(h.transport.count_id(1) == 0);
}

TEST_CASE("MCP cancel of an unknown or finished id is ignored", "[mcp][protocol]") {
    Harness h;
    h.init();
    CHECK(h.tool(1, "echo").contains("result"));
    h.transport.feed(Harness::note("notifications/cancelled", json{{"requestId", 1}}));
    h.transport.feed(Harness::note("notifications/cancelled", json{{"requestId", 4242}}));
    h.transport.feed(Harness::note("notifications/cancelled", json{{"nothing", true}}));
    h.transport.feed(Harness::note("notifications/whatever"));
    h.sync();
    CHECK(h.tool(2, "echo").contains("result"));
    CHECK(h.transport.count_id(1) == 1);
}

TEST_CASE("MCP queue overflow answers -32000", "[mcp][protocol]") {
    ServerOptions opts = Harness::defaults();
    opts.max_queue = 2;
    Harness h(opts);
    h.init();
    h.transport.feed(Harness::req(1, "tools/call", json{{"name", "slow"}}));
    REQUIRE(h.provider.wait_started());
    h.transport.feed(Harness::req(2, "tools/call", json{{"name", "echo"}}));
    h.transport.feed(Harness::req(3, "tools/call", json{{"name", "echo"}}));
    json overflow = h.call(4, "tools/call", json{{"name", "echo"}});
    REQUIRE(overflow.contains("error"));
    CHECK(overflow["error"]["code"] == kServerBusy);
    CHECK(overflow["error"]["message"] == "server busy");
    CHECK(overflow["error"]["data"]["retry"] == true);
    h.provider.release();
    CHECK(h.transport.wait_for(2).contains("result"));
    CHECK(h.transport.wait_for(3).contains("result"));
}

TEST_CASE("MCP soft timeout answers once, drops the late result and recovers", "[mcp][protocol]") {
    ServerOptions opts = Harness::defaults();
    opts.call_timeout_ms = 50;
    Harness h(opts);
    h.init();
    h.transport.feed(Harness::req(1, "tools/call", json{{"name", "slow"}}));
    json timeout = h.transport.wait_for(1);
    REQUIRE(timeout.is_object());
    CHECK(timeout["result"]["isError"] == true);
    CHECK(text_of(timeout).rfind("CALL_TIMEOUT:", 0) == 0);

    const int calls_before = h.provider.calls;
    json busy = h.tool(2, "echo");
    REQUIRE(busy.is_object());
    CHECK(busy["result"]["isError"] == true);
    CHECK(text_of(busy).rfind("SERVER_BUSY", 0) == 0);
    CHECK(h.provider.calls == calls_before);  // provider not invoked while busy
    CHECK(h.call(3, "ping")["result"] == json::object());

    h.provider.release();
    REQUIRE(h.provider.wait_finished());
    // Recovery: retry until the main thread is free again.
    json ok;
    for (int i = 0; i < 100; ++i) {
        ok = h.tool(100 + i, "echo");
        REQUIRE(ok.is_object());
        if (!ok["result"].contains("isError")) break;
        std::this_thread::sleep_for(20ms);
    }
    CHECK_FALSE(ok["result"].contains("isError"));
    h.sync();
    CHECK(h.transport.count_id(1) == 1);  // the late "slow done" result was suppressed
}

TEST_CASE("MCP unknown methods and notifications", "[mcp][protocol]") {
    Harness h;
    h.init();
    CHECK(h.call(1, "server/discover")["error"]["code"] == kMethodNotFound);
    CHECK(h.call(2, "resources/list")["error"]["code"] == kMethodNotFound);
    h.transport.feed(Harness::note("notifications/unknown"));
    CHECK(h.call(3, "ping").contains("result"));
    auto lines = h.transport.lines();
    CHECK(lines.size() == 4);  // init result, 2 errors, ping: no output for the notification
}

TEST_CASE("MCP survives malformed input and exits 0 on EOF", "[mcp][protocol]") {
    Harness h;
    h.init();
    h.transport.feed("{not json");
    h.transport.feed("");
    h.transport.feed("[1,2]");
    h.transport.feed("42");
    h.transport.feed(R"({"jsonrpc":"2.0","id":null,"method":"ping"})");
    CHECK(h.tool(1, "echo").contains("result"));
    CHECK(h.transport.count_id(nullptr) >= 4);  // parse error, array, scalar, null id
    for (const auto& raw : h.transport.lines()) {
        json j = json::parse(raw, nullptr, false);
        REQUIRE(j.is_object());
        CHECK(j["jsonrpc"] == "2.0");
    }
    CHECK(h.finish() == 0);
}

TEST_CASE("MCP run returns 0 on immediate EOF", "[mcp][protocol]") {
    std::istringstream in("");
    std::ostringstream out;
    StringTransport transport(in, out);
    FakeProvider provider;
    McpServer server(transport, provider, Harness::defaults());
    CHECK(server.run() == 0);
    CHECK(out.str().empty());
}

#ifdef _WIN32
namespace {

std::wstring find_fairyfly_exe() {
    wchar_t module[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    std::wstring dir(module);
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    const std::vector<std::wstring> candidates = {dir + L"fairyfly.exe", L"build\\Release\\fairyfly.exe",
                                                  dir + L"..\\Release\\fairyfly.exe"};
    for (const auto& c : candidates)
        if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) return c;
    return L"";
}

} // namespace

TEST_CASE("fairyfly serve speaks clean MCP over a real pipe", "[!mayfail][mcp-spawn]") {
    const std::wstring exe = find_fairyfly_exe();
    if (exe.empty()) { WARN("fairyfly.exe not built; skipping spawn test"); return; }

    SetEnvironmentVariableW(L"FAIRYFLY_AUDIT", L"0");
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE in_r = nullptr, in_w = nullptr, out_r = nullptr, out_w = nullptr;
    REQUIRE(CreatePipe(&in_r, &in_w, &sa, 0));
    REQUIRE(CreatePipe(&out_r, &out_w, &sa, 0));
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = nul;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe + L"\" serve";
    BOOL created = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(in_r);
    CloseHandle(out_w);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    REQUIRE(created);

    auto write_all = [&](const std::string& s) {
        DWORD written = 0;
        WriteFile(in_w, s.data(), static_cast<DWORD>(s.size()), &written, nullptr);
    };
    std::string collected;
    auto pump = [&](std::chrono::milliseconds budget, const std::function<bool()>& done) {
        const auto end = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < end && !done()) {
            DWORD avail = 0;
            if (!PeekNamedPipe(out_r, nullptr, 0, nullptr, &avail, nullptr)) break;
            if (avail == 0) { std::this_thread::sleep_for(20ms); continue; }
            char buf[4096];
            DWORD got = 0;
            if (!ReadFile(out_r, buf, sizeof(buf), &got, nullptr) || got == 0) break;
            collected.append(buf, got);
        }
    };
    auto count_lines = [&] { return std::count(collected.begin(), collected.end(), '\n'); };

    write_all(Harness::req(1, "initialize", json{{"protocolVersion", "2025-06-18"}, {"capabilities", json::object()}, {"clientInfo", json{{"name", "spawn-test"}, {"version", "0"}}}}) + "\n");
    write_all(Harness::note("notifications/initialized") + "\n");
    write_all(Harness::req(2, "tools/list") + "\n");
    pump(15000ms, [&] { return count_lines() >= 2; });

    CloseHandle(in_w);  // EOF: the server must exit cleanly
    const DWORD wait = WaitForSingleObject(pi.hProcess, 15000);
    DWORD exit_code = 999;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &exit_code);
    else TerminateProcess(pi.hProcess, 1);
    pump(1000ms, [] { return false; });
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(out_r);

    REQUIRE(wait == WAIT_OBJECT_0);
    CHECK(exit_code == 0);

    std::istringstream stream(collected);
    std::string line;
    int lines = 0;
    bool saw_doctor = false;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        ++lines;
        json j = json::parse(line, nullptr, false);
        INFO("stdout line: " << line);
        REQUIRE(j.is_object());
        CHECK(j["jsonrpc"] == "2.0");
        if (j.contains("id") && j["id"] == 2)
            for (const auto& t : j["result"]["tools"])
                if (t["name"] == "sap_doctor") saw_doctor = true;
    }
    CHECK(lines == 2);
    CHECK(saw_doctor);
}
#endif
