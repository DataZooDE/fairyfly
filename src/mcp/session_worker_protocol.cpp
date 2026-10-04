#include "include/mcp/session_worker_protocol.h"

#include <algorithm>
#include <charconv>
#include <istream>
#include <ostream>
#include <set>
#include <stdexcept>
#include <string_view>

namespace fairyfly::mcp {
namespace {

bool is_bound_command(const std::vector<std::string>& argv) {
    if (argv.size() < 2) return false;
    static const std::set<std::pair<std::string, std::string>> allowed = {
        {"screen", "read"}, {"screen", "find"}, {"screen", "capture"},
        {"menu", "list"}, {"menu", "select"},
        {"element", "get"}, {"element", "click"}, {"element", "fill"}, {"element", "f4"},
        {"key", "send"}, {"popup", "close"}, {"transaction", "start"},
        {"session", "disconnect"}};
    return allowed.count({argv[0], argv[1]}) != 0;
}

bool valid_session_identity(const std::string& identity) {
    if (identity.size() > 512 || identity.rfind("/app/con[", 0) != 0 || identity.find("/ses[") == std::string::npos)
        return false;
    const auto first = identity.find('|');
    const auto second = identity.find('|', first == std::string::npos ? 0 : first + 1);
    return first != std::string::npos && second != std::string::npos &&
           first > 0 && second > first + 1 && second + 1 < identity.size() &&
           identity.find('|', second + 1) == std::string::npos;
}

bool valid_owner_identity(const std::string& owner) {
    if (owner.empty() || owner.size() > 256) return false;
    const auto first = owner.find('/');
    const auto second = owner.find('/', first == std::string::npos ? 0 : first + 1);
    return first != std::string::npos && second != std::string::npos &&
           first > 0 && second > first + 1 && second + 1 < owner.size() &&
           owner.find('/', second + 1) == std::string::npos;
}

int parse_connection(const std::string& text) {
    int value = -1;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value < 0 || value > 1000000)
        throw std::invalid_argument("invalid worker connection");
    return value;
}

} // namespace

bool worker_command_allowed(const std::vector<std::string>& argv) { return is_bound_command(argv); }

std::string encode_worker_call(const WorkerCall& call) {
    json request{{"version", 1}, {"id", call.id},
                 {"session", {{"connection", call.connection}, {"identity", call.session_identity},
                              {"owner", call.owner_identity}}},
                 {"argv", call.argv}, {"read_only", call.read_only}};
    if (call.probe) request["probe"] = true;
    if (call.enumerate) request["enumerate"] = true;
    if (!call.expected_screen_guard.empty()) request["expected_screen_guard"] = call.expected_screen_guard;
    if (call.selection_input)
        request["selection_input"] = {{"program", call.selection_program}, {"screen", call.selection_screen}};
    std::string encoded = request.dump(-1, ' ', false, json::error_handler_t::replace);
    (void)decode_worker_call(encoded);  // one validator at both ends of the private pipe
    return encoded;
}

WorkerCall decode_worker_call(const std::string& line) {
    if (line.empty() || line.size() > kWorkerRequestMaxBytes)
        throw std::invalid_argument("invalid worker frame length");
    json request;
    try { request = json::parse(line); }
    catch (...) { throw std::invalid_argument("invalid worker JSON"); }
    if (!request.is_object() || !request.contains("version") || !request["version"].is_number_integer() ||
        request["version"] != 1 ||
        !request.contains("id") || !request["id"].is_number_integer() ||
        !request.contains("session") || !request["session"].is_object() ||
        !request.contains("read_only") || !request["read_only"].is_boolean() ||
        !request.contains("argv") || !request["argv"].is_array())
        throw std::invalid_argument("invalid worker request");
    if (request.contains("probe") && !request["probe"].is_boolean())
        throw std::invalid_argument("invalid worker probe flag");
    if (request.contains("enumerate") && !request["enumerate"].is_boolean())
        throw std::invalid_argument("invalid worker discovery flag");

    const json& session = request["session"];
    if (!session.contains("connection") || !session["connection"].is_number_integer() ||
        !session.contains("identity") || !session["identity"].is_string() ||
        !session.contains("owner") || !session["owner"].is_string())
        throw std::invalid_argument("invalid worker session descriptor");

    WorkerCall call;
    if (request["id"] < 0 || request["id"] > 2147483647LL ||
        session["connection"] < -1 || session["connection"] > 1000000)
        throw std::invalid_argument("invalid worker session descriptor");
    call.id = request["id"].get<int>();
    call.connection = session["connection"].get<int>();
    call.probe = request.value("probe", false);
    call.enumerate = request.value("enumerate", false);
    call.session_identity = session["identity"].get<std::string>();
    call.owner_identity = session["owner"].get<std::string>();
    call.read_only = request["read_only"].get<bool>();
    if (request.contains("expected_screen_guard")) {
        if (!request["expected_screen_guard"].is_string())
            throw std::invalid_argument("invalid worker screen guard");
        call.expected_screen_guard = request["expected_screen_guard"].get<std::string>();
        if (call.expected_screen_guard.size() != 64 ||
            !std::all_of(call.expected_screen_guard.begin(), call.expected_screen_guard.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            })) throw std::invalid_argument("invalid worker screen guard");
    }
    if (call.enumerate) {
        if (call.probe || call.connection != -1 || !call.read_only || !call.session_identity.empty() ||
            !call.owner_identity.empty() || !request["argv"].empty() || request.contains("selection_input") ||
            !call.expected_screen_guard.empty())
            throw std::invalid_argument("worker discovery cannot carry an action");
        return call;
    }
    if (call.probe) {
        if (!call.read_only || !call.session_identity.empty() || !call.owner_identity.empty() ||
            !request["argv"].empty() || request.contains("selection_input") ||
            !call.expected_screen_guard.empty())
            throw std::invalid_argument("worker probe cannot carry an action");
        return call;
    }
    if (call.id < 0 || call.connection < 0 || call.connection > 1000000 ||
        !valid_session_identity(call.session_identity) || !valid_owner_identity(call.owner_identity))
        throw std::invalid_argument("invalid worker session descriptor");
    if (request["argv"].size() < 2 || request["argv"].size() > 64)
        throw std::invalid_argument("invalid worker argv length");
    for (const auto& value : request["argv"]) {
        if (!value.is_string()) throw std::invalid_argument("worker argv must contain strings");
        const std::string arg = value.get<std::string>();
        if (arg.size() > 4096 || arg.find('\0') != std::string::npos)
            throw std::invalid_argument("invalid worker argument");
        call.argv.push_back(arg);
    }
    if (!worker_command_allowed(call.argv)) throw std::invalid_argument("worker command is not session-bound");
    int connection_options = 0;
    for (std::size_t i = 0; i < call.argv.size(); ++i) {
        if (call.argv[i].rfind("--connection=", 0) == 0)
            throw std::invalid_argument("worker connection must be explicit and separate");
        if (call.argv[i] == "--connection") {
            if (++connection_options != 1 || i + 1 >= call.argv.size() ||
                parse_connection(call.argv[i + 1]) != call.connection)
                throw std::invalid_argument("worker connection differs from bound session");
            ++i;
        }
    }
    if (connection_options != 1) throw std::invalid_argument("worker command lacks a bound connection");
    if (request.contains("selection_input")) {
        const auto& mode = request["selection_input"];
        if (!mode.is_object() || !mode.contains("program") || !mode["program"].is_string() ||
            !mode.contains("screen") || !mode["screen"].is_string() ||
            call.argv[0] != "element" || call.argv[1] != "fill")
            throw std::invalid_argument("invalid worker selection-input mode");
        call.selection_input = true;
        call.selection_program = mode["program"].get<std::string>();
        call.selection_screen = mode["screen"].get<std::string>();
        if (call.selection_program.empty() || call.selection_screen.empty() ||
            call.selection_program.size() > 128 || call.selection_screen.size() > 32)
            throw std::invalid_argument("invalid worker selection-input screen");
    }
    return call;
}

int run_worker_loop(std::istream& input, std::ostream& output, const WorkerInvoker& invoke) {
    constexpr std::size_t kMaxResponseBytes = 16 * 1024 * 1024;
    while (true) {
        std::string line;
        bool complete = false;
        while (true) {
            const int next = input.get();
            if (next == std::char_traits<char>::eof()) {
                if (line.empty()) return 0;
                break;
            }
            if (next == '\n') { complete = true; break; }
            if (line.size() >= kWorkerRequestMaxBytes) {
                output << json{{"version", 1}, {"id", nullptr},
                               {"error", {{"code", "WORKER_BAD_REQUEST"}}}}.dump() << '\n' << std::flush;
                return 2;
            }
            line.push_back(static_cast<char>(next));
        }
        if (!complete && line.empty()) return 0;
        WorkerCall call;
        try { call = decode_worker_call(line); }
        catch (...) {
            output << json{{"version", 1}, {"id", nullptr},
                           {"error", {{"code", "WORKER_BAD_REQUEST"}}}}.dump() << '\n' << std::flush;
            return 2;
        }
        Result result;
        bool uncertain = false;
        try { result = invoke(call); }
        catch (...) {
            const bool read_only_control = call.probe || call.enumerate;
            uncertain = !read_only_control;
            result.status = Result::Status::Error;
            result.error = {{"code", read_only_control ? "WORKER_UNAVAILABLE" : "OUTCOME_UNKNOWN"},
                            {"message", read_only_control ? "worker read failed" : "worker command outcome is unknown"}};
        }
        json response{{"version", 1}, {"id", call.id}, {"result", result.to_json()}};
        std::string encoded = response.dump(-1, ' ', false, json::error_handler_t::replace);
        if (encoded.size() > kMaxResponseBytes)
            encoded = json{{"version", 1}, {"id", call.id},
                           {"error", {{"code", "WORKER_RESULT_TOO_LARGE"}}}}.dump();
        output << encoded << '\n' << std::flush;
        if (!output.good()) return 3;
        if (uncertain) return 4;
    }
}

} // namespace fairyfly::mcp
