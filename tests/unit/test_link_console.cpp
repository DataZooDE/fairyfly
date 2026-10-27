// What `fairyfly link pair|run` shows at the desktop (D10): the QR code must decode as the person sees it - in a
// Windows console with a legacy code page as well (it used to come out as mojibake there) - pairing shows that it
// is contacting the gateway at once and stops promptly on Ctrl+C, and `link run` prints its connection state.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
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
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include "include/link/link_console.h"
#include "include/link/link_control.h"
#include "include/link/link_runner.h"
#include "include/tray/tray_win32.h"
#include "crypto.hpp"
#include "link_agent.hpp"
#include "link_state.hpp"
#include "qr.hpp"
#include "support/env_guard.h"

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

// ---- link pair: the service's address and the next step ---------------------------------------------------------

TEST_CASE("link pair: https://fairyfly.io is prefilled; --gateway and FAIRYFLY_LINK_GATEWAY override it", "[link][pair]") {
    auto g = pair_gateway("", "");
    CHECK(g.ask);
    CHECK(g.address == "https://fairyfly.io");
    CHECK(std::string(kDefaultGateway) == "https://fairyfly.io");
    g = pair_gateway("", "https://gw.acme.example");
    CHECK_FALSE(g.ask);
    CHECK(g.address == "https://gw.acme.example");
    g = pair_gateway("https://flag.example", "https://gw.acme.example");   // the flag wins over the environment
    CHECK_FALSE(g.ask);
    CHECK(g.address == "https://flag.example");

    CHECK(pair_gateway_answer("", "https://fairyfly.io") == "https://fairyfly.io");          // Enter accepts the prefill
    CHECK(pair_gateway_answer("  \r", "https://fairyfly.io") == "https://fairyfly.io");
    CHECK(pair_gateway_answer(" https://gw.acme.example \r", "https://fairyfly.io") == "https://gw.acme.example");   // edited

    const std::string shown = pair_gateway_prompt("https://fairyfly.io", false);
    CHECK_THAT(shown, ContainsSubstring("[https://fairyfly.io]"));
    CHECK_THAT(shown, ContainsSubstring("Enter"));
    const std::string in_line = pair_gateway_prompt("https://fairyfly.io", true);   // already typed in the input line
    CHECK_THAT(in_line, !ContainsSubstring("[https://fairyfly.io]"));
    CHECK_THAT(in_line, ContainsSubstring("Enter"));
}

TEST_CASE("link pair: asks for the address with https://fairyfly.io prefilled; a typed address replaces it", "[link][pair]") {
    // the live checks have developers set FAIRYFLY_LINK_GATEWAY: clear it for this test, restore it afterwards
    fairyfly::test::EnvGuard env_cleared("FAIRYFLY_LINK_GATEWAY", "");
    FakePairing gw;
    SharedOut buf;
    std::ostream out(&buf);
    std::istringstream in(gw.url() + "\n");
    std::atomic<bool> stop{false};
    LinkOptions o;   // no --gateway
    const TempDir dir("pair-ask");
    o.state_path = (dir / "state.json").string();
    fairyfly::Result result;
    std::atomic<bool> done{false};
    std::thread t([&] { result = link_pair(o, PairIo{&out, &in, &stop}); done = true; });
    REQUIRE(buf.wait_for("Contacting " + gw.url(), std::chrono::seconds(5)));
    CHECK_THAT(buf.text(), ContainsSubstring("[https://fairyfly.io]"));
    stop = true;
    gw.release();
    t.join();
    CHECK(result.error["code"] == "PAIRING_CANCELLED");
}

// ---- the gateway address: https only, plain http only to this computer ------------------------------------------

TEST_CASE("gateway url: https anywhere, http only to exactly localhost, 127.0.0.1 or [::1]", "[link][pair][gateway-url]") {
    const auto ok = [](const std::string& url) { return !gateway_url_problem(url).has_value(); };
    // allowed
    for (const char* url : {"https://fairyfly.io", "https://fairyfly.io/", "https://gw.acme.example:8443/base", "https://[2001:db8::1]:443",
                            "HTTPS://fairyfly.io", "http://localhost", "http://localhost:8080", "http://localhost:8080/", "http://127.0.0.1",
                            "http://127.0.0.1:18080/gw", "http://[::1]", "http://[::1]:9000", "http://LOCALHOST:80", "HTTP://LocalHost"}) {
        INFO(url);
        CHECK(ok(url));
    }
    // refused: the prefix tricks the old check let through, and other ways to reach a remote host without TLS
    for (const char* url : {"http://localhost.evil.example", "http://127.0.0.1@evil.example", "http://localhost:80@evil.example",
                            "http://127.0.0.1.nip.io", "http://127.0.0.1.nip.io:8080/", "http://localhost.", "http://localhost.:8080",
                            "http://127.0.0.1.", "http://LOCALHOST.EVIL.EXAMPLE", "HTTP://evil.example", "http://evil.example",
                            "http://user:pass@localhost", "http://localhost@localhost", "https://user@fairyfly.io",
                            "https://fairyfly.io:443@evil.example", "http://[::1].evil.example", "http://[::2]", "http://[::1",
                            "http://::1", "http://[0:0:0:0:0:0:0:1]", "http://0.0.0.0", "http://127.0.0.2", "http://127.1",
                            "http://2130706433", "http://0x7f.0.0.1", "http://%6cocalhost", "http://localhost%2eevil.example",
                            "http://127.0.0.1%40evil.example", " http://localhost", "http://localhost ", "http://local host",
                            "http://localhost\t", "\nhttp://localhost", "http:\\\\localhost", "http://localhost\\@evil.example",
                            "https://fairyfly.io\\@evil.example", "http://evil.example#@localhost", "http://evil.example?@localhost",
                            "http://localhost#frag", "https://fairyfly.io/#x", "https://fairyfly.io/?x=1", "http://localhost:0",
                            "http://localhost:99999", "http://localhost:", "http://localhost:80a", "ftp://fairyfly.io",
                            "wss://fairyfly.io", "ws://localhost", "fairyfly.io", "localhost:8080", "//localhost", "", "https://",
                            "https://:443", "https:///fairyfly.io", "https://fairyfly.\xC4\xB1o", "file:///C:/x"}) {
        INFO(url);
        CHECK_FALSE(ok(url));
    }
    CHECK_THAT(*gateway_url_problem("http://localhost.evil.example"), ContainsSubstring("https://"));
}

TEST_CASE("gateway url: the link's WebSocket address needs wss://, ws:// only to this computer", "[link][run][gateway-url]") {
    const auto ok = [](const std::string& url) { return !gateway_url_problem(url, GatewayUrlKind::WebSocket).has_value(); };
    for (const char* url : {"wss://fairyfly.io/link", "wss://gw.acme.example:8443/link?v=1", "ws://127.0.0.1:9000/link", "ws://localhost/link",
                            "ws://[::1]:9000/link"}) {
        INFO(url);
        CHECK(ok(url));
    }
    for (const char* url : {"ws://evil.example/link", "ws://localhost.evil.example/link", "ws://127.0.0.1@evil.example/link",
                            "ws://127.0.0.1.nip.io/link", "ws://localhost./link", "https://fairyfly.io/link", "http://127.0.0.1/link",
                            "wss://fairyfly.io/link#x", "wss://user@fairyfly.io/link", " wss://fairyfly.io/link", ""}) {
        INFO(url);
        CHECK_FALSE(ok(url));
    }
}

namespace {
using EnvSet = fairyfly::test::EnvGuard;
}  // namespace

TEST_CASE("link pair: a plain-http gateway that is not this computer is refused before anything is sent", "[link][pair][gateway-url]") {
    // Each address resolves to (or is tried at) a port nothing listens on: the old prefix check let them through to
    // "Contacting ..." and a network error. Refused, nothing is contacted and nothing saved.
    const auto pair_with = [](LinkOptions o, const std::string& typed, const std::string& dir_name) {
        std::ostringstream out;
        std::istringstream in(typed);
        std::atomic<bool> stop{false};
        const TempDir dir(dir_name);
        o.state_path = (dir / "state.json").string();
        o.pair_timeout_s = 1;
        auto r = link_pair(o, PairIo{&out, &in, &stop});
        CHECK_FALSE(std::filesystem::exists(o.state_path));
        CHECK(out.str().find("Contacting") == std::string::npos);
        return r;
    };
    for (const char* bad : {"http://127.0.0.1@127.0.0.1:1", "http://localhost:1@127.0.0.1:1", "http://127.0.0.1.nip.io:1", "http://localhost.:1"}) {
        INFO(bad);
        {   // --gateway
            EnvSet env("FAIRYFLY_LINK_GATEWAY", "");
            LinkOptions o;
            o.gateway = bad;
            auto r = pair_with(o, "", "pair-bad-flag");
            CHECK(r.error["code"] == "INVALID_ARGUMENT");
        }
        {   // FAIRYFLY_LINK_GATEWAY
            EnvSet env("FAIRYFLY_LINK_GATEWAY", bad);
            auto r = pair_with(LinkOptions{}, "", "pair-bad-env");
            CHECK(r.error["code"] == "INVALID_ARGUMENT");
        }
        {   // typed at the prompt
            EnvSet env("FAIRYFLY_LINK_GATEWAY", "");
            auto r = pair_with(LinkOptions{}, std::string(bad) + "\n", "pair-bad-typed");
            CHECK(r.error["code"] == "INVALID_ARGUMENT");
        }
    }
}

TEST_CASE("link pair: a gateway that hands out a plain ws:// link to another host is not saved", "[link][pair][gateway-url]") {
    FakePairing gw;
    gw.release();
    auto gk = ffy::ec_generate();
    gw.collect_result = {{"device_id", "dev-1"}, {"tenant", "t-1"}, {"tenant_name", "data zoo"}, {"email", "jr@data-zoo.de"},
                         {"email_verified", true}, {"provider", "google"}, {"issuer", "https://fairyfly.io"},
                         {"link_url", "ws://127.0.0.1.nip.io/link"}, {"gateway_keys", json::array({ffy::jwk_from_ec(gk).to_json()})}};
    std::ostringstream out;
    std::istringstream in("yes\n");
    std::atomic<bool> stop{false};
    LinkOptions o;
    o.gateway = gw.url();
    const TempDir dir("pair-insecure-link");
    o.state_path = (dir / "state.json").string();
    auto result = link_pair(o, PairIo{&out, &in, &stop});
    CHECK(result.status == fairyfly::Result::Status::Error);
    CHECK(result.error["code"] == "PAIRING_FAILED");
    CHECK_FALSE(std::filesystem::exists(o.state_path));
}

TEST_CASE("link status: a stored pairing with a plain ws:// link to another host is refused", "[link][run][gateway-url]") {
    const TempDir dir("state-insecure");
    ffy::link::State st;
    st.link_url = "ws://localhost.evil.example/link";
    st.device_id = "dev-1";
    st.tenant = "t-1";
    st.email = "jr@data-zoo.de";
    st.issuer = "https://fairyfly.io";
    st.key = ffy::ec_generate();
    st.gateway_keys.push_back(ffy::jwk_from_ec(ffy::ec_generate()));
    LinkOptions o;
    o.state_path = (dir / "state.json").string();
    ffy::link::save_state(st, o.state_path);
    auto r = link_status(o);
    CHECK(r.status == fairyfly::Result::Status::Error);
    CHECK(r.error["code"] == "LINK_STATE_INSECURE");

    st.link_url = "wss://fairyfly.io/link";   // the same pairing with a wss:// link is fine
    ffy::link::save_state(st, o.state_path);
    CHECK(link_status(o).status == fairyfly::Result::Status::Success);
}

TEST_CASE("link run lock: one name per pairing state file", "[link][run]") {
    const std::string a = link_run_lock_name("C:\\Users\\pat\\AppData\\Local\\fairyfly\\link\\state.json");
    CHECK_THAT(a, StartsWith("Local\\fairyfly-link-run-"));
    CHECK(a.find('\\', 6) == std::string::npos);   // only the namespace separator
    CHECK(a == link_run_lock_name("c:/users/PAT/AppData/Local/fairyfly/link/state.json"));
    CHECK(a == link_run_lock_name("C:\\Users\\pat\\AppData\\Local\\fairyfly\\link\\..\\link\\state.json"));
    CHECK(a != link_run_lock_name("C:\\Tools\\fairyfly-test\\state.json"));
}

#ifdef _WIN32
TEST_CASE("link run lock: the same file through a junction, an 8.3 name or another case gets the same name", "[link][run]") {
    // The name used to come from the path text alone: a junction, a short name or a non-ASCII case difference gave a
    // second lock for the same pairing file, so two links could serve one pairing.
    namespace fs = std::filesystem;
    const TempDir root("lock-identity");
    const fs::path real = root.path / "real";
    const fs::path dir = real / L"Pairing Ärger-Ölweg long directory";
    fs::create_directories(dir);
    const fs::path state = dir / "state.json";
    const fs::path missing = dir / "not-yet.json";
    const auto name = [](const fs::path& p) { return link_run_lock_name(p.string()); };

    // not paired yet: the name does not change when the file appears (the link tray computes it before pairing)
    const std::string before = name(state);
    std::ofstream(state) << "{}";
    const std::string a = name(state);
    CHECK(a == before);
    CHECK(a == name(root.path / "real" / L"pAIRING äRGER-öLWEG LONG DIRECTORY" / "STATE.JSON"));   // case, also non-ASCII
    CHECK(a == name(fs::path(root.path.wstring() + L"\\real\\.\\x\\..\\" + dir.filename().wstring() + L"\\state.json")));
    CHECK(a != name(missing));
    CHECK(a != name(real / "state.json"));

    // a junction to the directory holding it
    const fs::path junction = root.path / "jn";
    const std::wstring cmd = L"cmd /c mklink /J \"" + junction.wstring() + L"\" \"" + real.wstring() + L"\" >NUL";
    REQUIRE(_wsystem(cmd.c_str()) == 0);
    REQUIRE(fs::exists(junction / dir.filename() / "state.json"));
    CHECK(a == name(junction / dir.filename() / "state.json"));
    CHECK(name(missing) == name(junction / dir.filename() / "not-yet.json"));
    CHECK(name(missing) == name(junction / L"PAIRING ÄRGER-ÖLWEG LONG DIRECTORY" / "not-yet.json"));

    // the 8.3 short name of the directory
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetShortPathNameW(dir.c_str(), buf, MAX_PATH * 2);
    const std::wstring shortened = n ? std::wstring(buf, n) : std::wstring();
    fs::remove(junction);   // a junction is removed without its target
    if (shortened.empty() || shortened == dir.wstring()) SKIP("8.3 short names are disabled on this volume");
    CHECK(a == name(fs::path(shortened) / "state.json"));
}
#endif

TEST_CASE("link run: a second link for the same pairing is refused", "[link][run]") {
    const TempDir dir("run-lock");
    const std::string state = (dir / "state.json").string();
    std::unique_ptr<fairyfly::tray::SingleInstance> first, second, other;
    CHECK_FALSE(claim_link_run(state, first).has_value());   // the first link takes it
    REQUIRE(first);
    auto refused = claim_link_run(state, second);
    REQUIRE(refused.has_value());
    CHECK(refused->error["code"] == "LINK_ALREADY_RUNNING");
    CHECK_THAT(refused->error["message"].get<std::string>(), ContainsSubstring("already running"));
    CHECK_FALSE(claim_link_run((dir / "other.json").string(), other).has_value());   // another pairing is independent
    first.reset();   // the first link ended
    CHECK_FALSE(claim_link_run(state, second).has_value());
}

TEST_CASE("link run: a refused second link exits with its own code (the tray waits on it)", "[link][run]") {
    const TempDir dir("run-lock-exit");
    LinkOptions o;
    o.state_path = (dir / "state.json").string();
    std::unique_ptr<fairyfly::tray::SingleInstance> first;
    REQUIRE_FALSE(claim_link_run(o.state_path, first).has_value());   // a console link serves this pairing
    CHECK(fairyfly::tray::kLinkAlreadyRunningExitCode != 0);
    CHECK(fairyfly::tray::kLinkAlreadyRunningExitCode != 1);
    CHECK(fairyfly::tray::kLinkAlreadyRunningExitCode != 2);   // 2: not paired, refused state, ...
    CHECK(run_link(o, nullptr, {}, nullptr, nullptr) == fairyfly::tray::kLinkAlreadyRunningExitCode);
    first.reset();
    o.state_path = (dir / "missing.json").string();   // not paired: still 2
    CHECK(run_link(o, nullptr, {}, nullptr, nullptr) == 2);
}

TEST_CASE("link pair: the next step never asks for a second link", "[link][pair]") {
    CHECK_THAT(pair_next_step(false, false), ContainsSubstring("fairyfly link run"));
    const std::string tray = pair_next_step(true, false);
    CHECK_THAT(tray, ContainsSubstring("tray"));
    CHECK_THAT(tray, !ContainsSubstring("fairyfly link run"));
    const std::string running = pair_next_step(false, true);
    CHECK_THAT(running, ContainsSubstring("already running"));
    CHECK_THAT(running, !StartsWith("fairyfly link run"));
    CHECK_THAT(pair_next_step(true, true), ContainsSubstring("already running"));
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

// Live 2026-10-27: calls were refused for clock skew and libffl logged a warning, but link.log had no entry of the
// whole day: `link run` replaced the tray's logger (flush_on warn) by one that never flushed.
TEST_CASE("link run logger: libffl's warnings and calls reach link.log at once, not at exit", "[link][run][log]") {
    const TempDir dir("logger-flush");
    const auto file = dir / "link.log";
    {
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(file.string());
        auto logger = make_link_logger({sink}, spdlog::level::info);
        logger->warn("assertion rejected (inst.1): assertion: issued in the future: desktop clock 346 s behind the gateway (tolerance 90 s)");
        logger->info("served: tool=gui_session_list user=jr@data-zoo.de client=claude");
        std::ifstream in(file);   // the logger is still alive: nothing flushed it at exit yet
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK_THAT(text, ContainsSubstring("346 s behind"));
        CHECK_THAT(text, ContainsSubstring("served: tool=gui_session_list"));
        logger->debug("not shown at info");
    }
}

TEST_CASE("link control: the running link's clock skew reaches the tray", "[link][run][clock]") {
    const TempDir dir("clock-skew");
    ffy::link::State st;
    st.link_url = "wss://fairyfly.io/link";
    st.device_id = "dev-1";
    st.tenant = "t-1";
    st.tenant_name = "data zoo";
    st.email = "jr@data-zoo.de";
    st.issuer = "https://fairyfly.io";
    st.key = ffy::ec_generate();
    st.gateway_keys.push_back(ffy::jwk_from_ec(ffy::ec_generate()));
    const std::string path = (dir / "state.json").string();
    ffy::link::save_state(st, path);
    auto control = make_link_control(path);
    write_status(path, "connected", json{{"clock_skew_s", 346}});
    auto v = control->view();
    CHECK(v.state == "connected");
    CHECK(v.clock_skew_s == 346);
    write_status(path, "connected", json{{"clock_skew_s", 0}});
    CHECK(control->view().clock_skew_s == 0);
    write_status(path, "stopped", json{{"clock_skew_s", 346}});   // a link that is not running says nothing about now
    CHECK(control->view().clock_skew_s == 0);
}
