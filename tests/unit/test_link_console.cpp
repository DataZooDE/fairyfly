// What `fairyfly link pair|run` shows at the desktop (D10): the QR code must decode as the person sees it - in a
// Windows console with a legacy code page as well (it used to come out as mojibake there) - pairing shows that it
// is contacting the gateway at once and stops promptly on Ctrl+C, and `link run` prints its connection state.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include <httplib.h>
#include <ZXing/ReadBarcode.h>
#include <spdlog/spdlog.h>

#include "include/link/link_console.h"
#include "include/link/link_runner.h"
#include "crypto.hpp"
#include "link_agent.hpp"
#include "qr.hpp"

using namespace fairyfly::link;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;
using nlohmann::json;

namespace {

const std::string kLink = "https://fairyfly.io/p#p_8f3c2a91d04b.Zk3vQ9xT2mLr7NwB5yHc1sEa";

std::u32string utf32(const std::string& s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        char32_t cp = n == 1 ? c : n == 2 ? (c & 0x1f) : n == 3 ? (c & 0x0f) : (c & 0x07);
        for (int k = 1; k < n && i + k < s.size(); k++) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3f);
        out += cp;
        i += n;
    }
    return out;
}

std::vector<std::u32string> lines_of(const std::string& text) {
    std::vector<std::u32string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(utf32(line));
    }
    return out;
}

/// The cells a terminal lights up (drawn characters are light on the dark background), one bool per module.
/// Block rendering: one character is two module rows; ASCII rendering: "##" is one light module. Lines that are
/// not part of the code (text around it) are skipped.
std::vector<std::vector<bool>> light_modules(const std::string& screen) {
    std::vector<std::vector<bool>> grid;
    for (const auto& line : lines_of(screen)) {
        const bool blocks = line.find_first_of(U"▀▄█") != std::u32string::npos;
        const bool ascii = line.find(U"##") != std::u32string::npos && line.find_first_not_of(U"# ") == std::u32string::npos;
        if (blocks) {
            std::vector<bool> top, bottom;
            for (char32_t c : line) {
                top.push_back(c == U'█' || c == U'▀');
                bottom.push_back(c == U'█' || c == U'▄');
            }
            grid.push_back(top);
            grid.push_back(bottom);
        } else if (ascii) {
            std::vector<bool> row;
            for (size_t i = 0; i + 1 < line.size(); i += 2) row.push_back(line[i] == U'#');
            grid.push_back(row);
        } else if (std::any_of(line.begin(), line.end(), [](char32_t c) { return c > 0x7f; })) {
            // mojibake where the code should be: keep it, as the person would see it
            std::vector<bool> row;
            for (char32_t c : line) row.push_back(c != U' ');
            grid.push_back(row);
        }
    }
    return grid;
}

/// Photographs the cells (6 px per module, dark surround like a terminal window) and decodes them with ZXing.
std::string decode_screen(const std::string& screen) {
    const auto grid = light_modules(screen);
    if (grid.empty()) return {};
    size_t cols = 0;
    for (const auto& r : grid) cols = std::max(cols, r.size());
    const int scale = 6, margin = 8;
    const int w = int(cols + 2 * margin) * scale, h = int(grid.size() + 2 * margin) * scale;
    std::vector<uint8_t> px(size_t(w) * h, 0);
    for (size_t y = 0; y < grid.size(); y++)
        for (size_t x = 0; x < grid[y].size(); x++)
            if (grid[y][x])
                for (int dy = 0; dy < scale; dy++)
                    for (int dx = 0; dx < scale; dx++)
                        px[size_t((y + margin) * scale + dy) * w + (x + margin) * scale + dx] = 255;
    ZXing::ImageView image(px.data(), w, h, ZXing::ImageFormat::Lum);
    auto options = ZXing::ReaderOptions().setFormats(ZXing::BarcodeFormat::QRCode).setTryInvert(false);
    auto barcode = ZXing::ReadBarcode(image, options);
    return barcode.isValid() ? barcode.text() : std::string();
}

/// A fresh directory under %TEMP%, removed again at the end of the test.
struct TempDir {
    std::filesystem::path path;
    explicit TempDir(const std::string& name)
        : path(std::filesystem::temp_directory_path() /
               ("ffy-link-console-" + name + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::filesystem::path operator/(const std::string& file) const { return path / file; }
};

}  // namespace

TEST_CASE("link qr: the decoder of these tests reads libffl's own block rendering", "[link][qr]") {
    REQUIRE(decode_screen(ffy::qr::to_terminal(ffy::qr::encode(kLink))) == kLink);
}

TEST_CASE("link qr: both renderings decode to the pairing link", "[link][qr]") {
    const auto m = ffy::qr::encode(kLink);
    CHECK(decode_screen(render_qr(m, QrStyle::Blocks)) == kLink);
    const std::string ascii = render_qr(m, QrStyle::Ascii);
    CHECK(ascii.find_first_not_of("# \n") == std::string::npos);  // pure ASCII
    CHECK(decode_screen(ascii) == kLink);
}

TEST_CASE("link qr: --qr-style picks the rendering", "[link][qr]") {
    CHECK(choose_qr_style("ascii") == QrStyle::Ascii);
    CHECK(choose_qr_style("blocks") == QrStyle::Blocks);
    // auto: blocks only when stderr is a console that can show them (under ctest stderr is a pipe)
    CHECK(choose_qr_style("auto") == (console_shows_blocks() ? QrStyle::Blocks : QrStyle::Ascii));
}

#ifdef _WIN32
namespace {
/// Runs link_console_fixture in a new (hidden) console with the given output code page; returns what that console
/// shows afterwards, read back from its screen buffer.
std::string console_screen(const std::string& mode, int codepage) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const auto fixture = std::filesystem::path(self).parent_path() / L"link_console_fixture.exe";
    REQUIRE(std::filesystem::exists(fixture));
    const TempDir dir("screen");
    const auto out = dir / "screen.txt";
    std::wstring cmd = L"\"" + fixture.wstring() + L"\" " + std::wstring(mode.begin(), mode.end()) + L" " + std::to_wstring(codepage) + L" \"" +
                       std::wstring(kLink.begin(), kLink.end()) + L"\" \"" + out.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    REQUIRE(CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi));
    REQUIRE(WaitForSingleObject(pi.hProcess, 30000) == WAIT_OBJECT_0);
    DWORD code = 99;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    REQUIRE(code == 0);
    std::ifstream in(out, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}
}  // namespace

TEST_CASE("link qr: written to a code page 437 console, the code decodes as shown on screen", "[link][qr][console]") {
    // The bug: UTF-8 bytes into a cp437 console are shown as three characters each (U+2588 shows as GAMMA, u-circumflex, e-circumflex).
    const std::string before = console_screen("raw", 437);
    CHECK_THAT(before, ContainsSubstring("\xCE\x93\xC3\xBB"));  // U+0393 U+00FB, as UTF-8
    CHECK(decode_screen(before).empty());

    const std::string blocks = console_screen("blocks", 437);
    CHECK(decode_screen(blocks) == kLink);
    CHECK(decode_screen(console_screen("blocks", 1252)) == kLink);
    CHECK(decode_screen(console_screen("ascii", 437)) == kLink);
    // the text around the code reaches the console intact too
    CHECK_THAT(blocks, ContainsSubstring("Contacting https://fairyfly.io\xE2\x80\xA6"));
}
#endif

// ---- link pair: feedback at once, Ctrl+C ------------------------------------------------------------------------

namespace {
struct FakePairing {
    httplib::Server server;
    std::thread thread;
    int port = 0;
    std::mutex mu;
    std::condition_variable cv;
    bool release_start = false;   // /pair/start answers once the test allows it
    std::atomic<int> collects{0};
    json collect_result;          // empty: pending (202)

    FakePairing() {
        server.Post("/pair/start", [this](const httplib::Request&, httplib::Response& res) {
            std::unique_lock<std::mutex> l(mu);
            cv.wait_for(l, std::chrono::seconds(10), [this] { return release_start; });
            res.set_content(json{{"pid", "p1"}, {"link", kLink}, {"code", "QX7-K9M"}, {"match", "42"}, {"expires_in", 600}}.dump(), "application/json");
        });
        server.Post("/pair/collect", [this](const httplib::Request&, httplib::Response& res) {
            collects++;
            if (collect_result.is_null()) { res.status = 202; return; }
            res.set_content(collect_result.dump(), "application/json");
        });
        port = server.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { server.listen_after_bind(); });
    }
    ~FakePairing() {
        { std::lock_guard<std::mutex> l(mu); release_start = true; }
        cv.notify_all();
        server.stop();
        thread.join();
    }
    void release() {
        { std::lock_guard<std::mutex> l(mu); release_start = true; }
        cv.notify_all();
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port); }
};

/// std::ostream the pairing writes to while the test watches it from another thread.
class SharedOut : public std::stringbuf {
public:
    std::string text() { std::lock_guard<std::recursive_mutex> l(mu_); return str(); }
    bool wait_for(const std::string& needle, std::chrono::milliseconds limit) {
        auto end = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < end) {
            if (text().find(needle) != std::string::npos) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    }
protected:
    std::streamsize xsputn(const char* s, std::streamsize n) override { std::lock_guard<std::recursive_mutex> l(mu_); return std::stringbuf::xsputn(s, n); }
    int_type overflow(int_type c) override { std::lock_guard<std::recursive_mutex> l(mu_); return std::stringbuf::overflow(c); }
private:
    std::recursive_mutex mu_;  // stringbuf::xsputn calls overflow
};
}  // namespace

TEST_CASE("link pair: says it is contacting the gateway before the gateway answers; Ctrl+C stops it promptly", "[link][pair]") {
    FakePairing gw;
    SharedOut buf;
    std::ostream out(&buf);
    std::istringstream in;
    std::atomic<bool> stop{false};
    LinkOptions o;
    o.gateway = gw.url();
    o.qr_style = "ascii";
    const TempDir dir("pair");
    o.state_path = (dir / "state.json").string();
    PairIo io{&out, &in, &stop};
    fairyfly::Result result;
    std::atomic<bool> done{false};
    std::thread t([&] { result = link_pair(o, io); done = true; });

    REQUIRE(buf.wait_for("Contacting " + gw.url() + "\xE2\x80\xA6", std::chrono::seconds(5)));  // while /pair/start still waits
    CHECK(buf.text().find("QX7-K9M") == std::string::npos);
    gw.release();
    REQUIRE(buf.wait_for("Number to type on the phone: 42", std::chrono::seconds(5)));
    CHECK(decode_screen(buf.text()) == kLink);
    REQUIRE(buf.wait_for("Ctrl+C", std::chrono::seconds(1)));

    const auto pressed = std::chrono::steady_clock::now();
    stop = true;  // what the Ctrl+C handler does
    for (int i = 0; i < 300 && !done; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - pressed);
    t.join();
    CHECK(took.count() < 1000);
    CHECK(result.status == fairyfly::Result::Status::Error);
    CHECK(result.error["code"] == "PAIRING_CANCELLED");
    CHECK_FALSE(std::filesystem::exists(o.state_path));
}

TEST_CASE("link pair: Ctrl+C while the gateway has not answered yet stops it promptly", "[link][pair]") {
    FakePairing gw;  // /pair/start never answers within the test
    SharedOut buf;
    std::ostream out(&buf);
    std::istringstream in;
    std::atomic<bool> stop{false};
    LinkOptions o;
    o.gateway = gw.url();
    const TempDir dir("pair-early");
    o.state_path = (dir / "state.json").string();
    fairyfly::Result result;
    std::atomic<bool> done{false};
    std::thread t([&] { result = link_pair(o, PairIo{&out, &in, &stop}); done = true; });
    REQUIRE(buf.wait_for("Contacting", std::chrono::seconds(5)));
    const auto pressed = std::chrono::steady_clock::now();
    stop = true;
    for (int i = 0; i < 300 && !done; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - pressed);
    t.join();
    CHECK(took.count() < 1000);
    CHECK(result.error["code"] == "PAIRING_CANCELLED");
    // the HTTP call it gave up on still runs: main() must not run static destructors under it
    CHECK(background_threads() >= 1);
    gw.release();
    for (int i = 0; i < 300 && background_threads() > 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(background_threads() == 0);
}

TEST_CASE("link pair: the account check reaches the given stream and 'no' saves nothing", "[link][pair]") {
    FakePairing gw;
    gw.release();
    auto gk = ffy::ec_generate();
    gw.collect_result = {{"device_id", "dev-1"}, {"tenant", "t-1"}, {"tenant_name", "data z\xC3\xB6o"}, {"email", "jr@data-zoo.de"},
                         {"email_verified", true}, {"provider", "google"}, {"issuer", "https://fairyfly.io"},
                         {"link_url", "wss://fairyfly.io/link"}, {"gateway_keys", json::array({ffy::jwk_from_ec(gk).to_json()})}};
    SharedOut buf;
    std::ostream out(&buf);
    std::istringstream in("no\n");
    std::atomic<bool> stop{false};
    LinkOptions o;
    o.gateway = gw.url();
    const TempDir dir("pair-no");
    o.state_path = (dir / "state.json").string();
    auto result = link_pair(o, PairIo{&out, &in, &stop});
    CHECK(result.error["code"] == "PAIRING_NOT_CONFIRMED");
    CHECK_THAT(buf.text(), ContainsSubstring("jr@data-zoo.de"));
    CHECK_THAT(buf.text(), ContainsSubstring("data z\xC3\xB6o"));
    CHECK_FALSE(std::filesystem::exists(o.state_path));
}

// ---- link run: status lines --------------------------------------------------------------------------------------

TEST_CASE("link run: status lines for connect, loss, reconnect, pause and stop", "[link][run]") {
    std::vector<std::string> lines;
    LinkStatusPrinter p("fairyfly.io", "jr@data-zoo.de", "data zoo", [&](const std::string& l) { lines.push_back(l); });
    p.starting(false);
    REQUIRE(lines.size() == 1);
    CHECK_THAT(lines[0], StartsWith("fairyfly link: connecting to fairyfly.io"));

    // libffl's own messages (link_agent.cpp)
    p.on_agent_log("link up: device=dev-1 gateway=wss://fairyfly.io/link");
    REQUIRE(lines.size() == 2);
    CHECK(lines[1] == "connected to fairyfly.io as jr@data-zoo.de in 'data zoo' \xE2\x80\x93 Ctrl+C stops");
    p.on_agent_log("served: tool=gui_screen_read user=jr client=claude");
    CHECK(lines.size() == 2);

    p.on_agent_log("gateway asked to reconnect (deploy)");
    p.on_agent_log("link down, reconnecting in 2400 ms");
    REQUIRE(lines.size() == 3);
    CHECK_THAT(lines[2], StartsWith("connection to fairyfly.io lost (gateway asked: deploy)"));
    CHECK_THAT(lines[2], ContainsSubstring("reconnecting in 2 s"));
    p.on_agent_log("link down (websocket connect wss://fairyfly.io/link: Could not establish connection), reconnecting in 3900 ms");
    REQUIRE(lines.size() == 4);
    CHECK_THAT(lines[3], ContainsSubstring("could not reach fairyfly.io (Could not establish connection)"));
    CHECK_THAT(lines[3], ContainsSubstring("retrying in 4 s"));
    p.on_agent_log("link up: device=dev-1 gateway=wss://fairyfly.io/link");
    REQUIRE(lines.size() == 5);
    CHECK(lines[4] == "reconnected to fairyfly.io");

    p.paused();
    p.resumed();
    REQUIRE(lines.size() == 7);
    CHECK_THAT(lines[5], ContainsSubstring("fairyfly link resume"));
    CHECK_THAT(lines[6], StartsWith("resumed"));
    p.stopping();
    p.on_agent_log("link down, reconnecting in 100 ms");  // while stopping: no reconnect notice
    p.stopped();
    REQUIRE(lines.size() == 9);
    CHECK_THAT(lines[7], StartsWith("stopping"));
    CHECK(lines[8] == "fairyfly link stopped");
}

TEST_CASE("link run: url_host", "[link][run]") {
    CHECK(url_host("wss://fairyfly.io/link") == "fairyfly.io");
    CHECK(url_host("https://fairyfly.io") == "fairyfly.io");
    CHECK(url_host("ws://127.0.0.1:9000/link") == "127.0.0.1");
    CHECK(url_host("fairyfly.io") == "fairyfly.io");
}

namespace {
class StubTools : public ffy::link::LocalTools {
public:
    json rpc(const std::string& method, const json&, int, const ffy::link::CallInfo&) override {
        if (method == "tools/list") return {{"jsonrpc", "2.0"}, {"id", 1}, {"result", {{"tools", json::array()}}}};
        return {{"jsonrpc", "2.0"}, {"id", 1}, {"error", {{"code", -32601}, {"message", "no"}}}};
    }
    bool per_call_read_only() const override { return true; }
};
}  // namespace

TEST_CASE("link run: libffl's agent against a fake gateway prints connected, lost and reconnected", "[link][run]") {
    // A gateway that completes the ffl/1 handshake and drops the first connection after a moment.
    httplib::Server gw;
    std::atomic<int> sessions{0};
    gw.WebSocket("/link", [&](const httplib::Request&, httplib::ws::WebSocket& ws) {
        const int n = ++sessions;
        std::string msg;
        if (ws.read(msg) != httplib::ws::Text) return;  // hello
        ws.send(json{{"t", "challenge"}, {"nonce_g", "g1"}}.dump());
        if (ws.read(msg) != httplib::ws::Text) return;  // proof
        ws.send(json{{"t", "ready"}, {"session", "s" + std::to_string(n)}}.dump());
        if (n == 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            ws.close();
            return;
        }
        while (ws.read(msg) != httplib::ws::Fail) {}
    }, [](const std::vector<std::string>&) { return std::string("ffl.v1"); });
    const int port = gw.bind_to_any_port("127.0.0.1");
    std::thread server([&] { gw.listen_after_bind(); });

    ffy::link::State st;
    st.link_url = "ws://127.0.0.1:" + std::to_string(port) + "/link";
    st.device_id = "dev-1";
    st.tenant = "t-1";
    st.tenant_name = "data zoo";
    st.email = "jr@data-zoo.de";
    st.issuer = "https://gateway.test";
    st.key = ffy::ec_generate();
    st.gateway_keys.push_back(ffy::jwk_from_ec(ffy::ec_generate()));
    ffy::link::AgentConfig cfg;
    cfg.local = std::make_shared<StubTools>();

    std::mutex mu;
    std::vector<std::string> lines;
    auto printer = std::make_shared<LinkStatusPrinter>("127.0.0.1", st.email, st.tenant_name, [&](const std::string& l) {
        std::lock_guard<std::mutex> g(mu);
        lines.push_back(l);
    });
    auto previous = spdlog::default_logger();
    auto logger = std::make_shared<spdlog::logger>("link-test", make_status_sink(printer));
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);
    auto count = [&] { std::lock_guard<std::mutex> g(mu); return lines.size(); };

    std::atomic<bool> stop{false};
    {
        ffy::link::Agent agent(st, cfg);
        std::thread net([&] { agent.run(stop); });
        for (int i = 0; i < 500 && count() < 3; i++) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        stop = true;
        net.join();
    }
    spdlog::set_default_logger(previous);
    gw.stop();
    server.join();

    std::lock_guard<std::mutex> g(mu);
    REQUIRE(lines.size() >= 3);
    CHECK(lines[0] == "connected to 127.0.0.1 as jr@data-zoo.de in 'data zoo' \xE2\x80\x93 Ctrl+C stops");
    CHECK_THAT(lines[1], StartsWith("connection to 127.0.0.1 lost"));
    CHECK(lines[2] == "reconnected to 127.0.0.1");
}
