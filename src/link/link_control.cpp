#include "include/link/link_control.h"

#include <fstream>
#include <memory>
#include <system_error>
#include <thread>

#ifdef _WIN32
#include <process.h>
#define FFY_GETPID _getpid
#else
#include <unistd.h>
#define FFY_GETPID getpid
#endif

namespace fairyfly::link {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
std::int64_t now_s() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
fs::path status_file(const std::string& state_path) { return fs::path(state_path + ".status.json"); }
}  // namespace

fs::path pause_marker(const std::string& state_path) { return fs::path(state_path + ".paused"); }

bool is_paused(const std::string& state_path) {
    std::error_code ec;
    return fs::exists(pause_marker(state_path), ec);
}

bool set_paused(const std::string& state_path, bool paused) {
    std::error_code ec;
    const auto marker = pause_marker(state_path);
    if (!paused) {
        fs::remove(marker, ec);
        return !ec && !fs::exists(marker, ec);
    }
    fs::create_directories(marker.parent_path(), ec);
    std::ofstream(marker, std::ios::trunc) << "paused at " << now_s() << "\n";
    return fs::exists(marker, ec);
}

void write_status(const std::string& state_path, const std::string& state) {
    std::error_code ec;
    const auto f = status_file(state_path);
    fs::create_directories(f.parent_path(), ec);
    const auto tmp = fs::path(f.string() + ".tmp");
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << json{{"state", state}, {"pid", static_cast<std::int64_t>(FFY_GETPID())}, {"updated", now_s()}}.dump();
    }
    fs::rename(tmp, f, ec);  // readers never see a half-written file
    if (ec) fs::remove(tmp, ec);
}

json read_status(const std::string& state_path) {
    std::ifstream in(status_file(state_path));
    if (!in) return json();
    auto j = json::parse(in, nullptr, false);
    if (!j.is_object() || !j.contains("state") || !j["state"].is_string() || !j.contains("updated") || !j["updated"].is_number_integer())
        return json();
    return j;
}

std::string describe(const json& status, std::int64_t now_unix) {
    if (!status.is_object() || !status.contains("state") || !status.contains("updated")) return "not running";
    const std::string s = status["state"].get<std::string>();
    if (s == "stopped") return "not running";
    if (now_unix - status["updated"].get<std::int64_t>() > kStatusStaleSeconds) return "not running";
    return s;
}

void supervise(const std::string& state_path, const std::function<void(const std::atomic<bool>&)>& agent, const std::atomic<bool>& stop,
               const SupervisorOptions& o) {
    struct Running {
        std::atomic<bool> net_stop{false}, ended{false};
        std::thread thread;
    };
    std::unique_ptr<Running> cur;
    auto halt = [&] {
        if (!cur) return;
        cur->net_stop = true;
        cur->thread.join();
        cur.reset();
    };
    std::string last;
    auto last_write = std::chrono::steady_clock::now();
    auto set = [&](const std::string& s) {
        auto now = std::chrono::steady_clock::now();
        if (s != last || now - last_write >= o.heartbeat) {
            write_status(state_path, s);
            if (s != last && o.on_change) o.on_change(s);
            last = s;
            last_write = now;
        }
    };
    auto next_start = std::chrono::steady_clock::now();
    while (!stop) {
        if (is_paused(state_path)) {
            halt();
            set("paused");
        } else {
            if (cur && cur->ended) {  // the agent gave up by itself (e.g. revoked or a fatal error): back off, retry
                halt();
                next_start = std::chrono::steady_clock::now() + o.restart_delay;
            }
            if (o.reconnect && o.reconnect->exchange(false) && cur) {
                halt();
                next_start = std::chrono::steady_clock::now();
            }
            if (!cur && std::chrono::steady_clock::now() >= next_start) {
                cur = std::make_unique<Running>();
                Running* r = cur.get();
                r->thread = std::thread([r, &agent] {
                    agent(r->net_stop);
                    r->ended = true;
                });
            }
            set("connected");
        }
        std::this_thread::sleep_for(o.poll);
    }
    halt();
    write_status(state_path, "stopped");
}

}  // namespace fairyfly::link
