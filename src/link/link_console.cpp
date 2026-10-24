#include "include/link/link_console.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <cstdio>
#include <iostream>

#include <spdlog/sinks/base_sink.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fairyfly::link {

std::string render_qr(const ffy::qr::Matrix& m, QrStyle style) {
    if (style == QrStyle::Blocks) return ffy::qr::to_terminal(m);
    // Same picture as to_terminal (light modules drawn, 2-module quiet zone), one module row per line; two
    // characters per module keep the modules roughly square in a terminal cell. The quiet zone shrinks to one
    // module when that keeps the code within 80 columns (a pairing link is 37 modules wide: 82 or 78 columns).
    std::string out;
    const int b = (m.size + 4) * 2 <= 80 ? 2 : 1;
    for (int y = -b; y < m.size + b; y++) {
        for (int x = -b; x < m.size + b; x++) out += m.at(x, y) ? "  " : "##";
        out += "\n";
    }
    return out;
}

PairGateway pair_gateway(const std::string& flag, const std::string& env) {
    if (!flag.empty()) return {flag, false};
    if (!env.empty()) return {env, false};
    return {kDefaultGateway, true};
}

std::string pair_gateway_prompt(const std::string& prefill, bool in_line) {
    if (in_line) return "Web address of the fairyfly service (Enter accepts it, or change it): ";
    return "Web address of the fairyfly service [" + prefill + "] (Enter accepts it, or type another): ";
}

std::string pair_gateway_answer(const std::string& typed, const std::string& prefill) {
    const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    auto b = std::find_if_not(typed.begin(), typed.end(), blank);
    auto e = std::find_if_not(typed.rbegin(), typed.rend(), blank).base();
    const std::string t = b < e ? std::string(b, e) : std::string();
    return t.empty() ? prefill : t;
}

std::optional<std::string> gateway_url_problem(const std::string& url, GatewayUrlKind kind) {
    (void)url;
    (void)kind;
    return std::nullopt;  // TODO
}

std::string link_run_lock_name(const std::string& state_path) {
    std::filesystem::path p(state_path);
    std::error_code ec;
    const auto abs = std::filesystem::absolute(p, ec);
    std::string key = (ec ? p : abs).lexically_normal().string();
    std::replace(key.begin(), key.end(), '/', '\\');
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::uint64_t h = 1469598103934665603ULL;   // FNV-1a: mutex names may not contain more backslashes
    for (unsigned char c : key) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(h));
    return std::string("Local\\fairyfly-link-run-") + hex;
}

std::string pair_next_step(bool link_tray_running, bool link_running) {
    if (link_running)
        return "a link for this desktop is already running with the previous pairing: quit it (tray: Quit, console: Ctrl+C) "
               "and start it again";
    if (link_tray_running) return "nothing: the link tray connects by itself now";
    return "the Start-menu entry \"fairyfly\" (fairyfly link run --tray), or fairyfly link run in a console";
}

QrStyle choose_qr_style(const std::string& requested) {
    if (requested == "ascii") return QrStyle::Ascii;
    if (requested == "blocks") return QrStyle::Blocks;
    return console_shows_blocks() ? QrStyle::Blocks : QrStyle::Ascii;
}

#ifdef _WIN32
namespace {
HANDLE resolve(void* handle) { return handle ? static_cast<HANDLE>(handle) : GetStdHandle(STD_ERROR_HANDLE); }
bool is_console(HANDLE h) {
    DWORD mode = 0;
    return h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode);
}
}  // namespace

bool write_utf8(std::string_view text, void* handle) {
    HANDLE h = resolve(handle);
    if (!h || h == INVALID_HANDLE_VALUE) return false;
    if (text.empty()) return true;
    if (!handle) std::cerr.flush();  // keep the order with anything written through std::cerr before
    if (is_console(h)) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
        if (n <= 0) return false;
        std::wstring wide(size_t(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), wide.data(), n);
        for (size_t done = 0; done < wide.size();) {  // large writes may be split by the console
            DWORD written = 0;
            const DWORD chunk = DWORD(std::min<size_t>(wide.size() - done, 8192));
            if (!WriteConsoleW(h, wide.data() + done, chunk, &written, nullptr) || written == 0) return false;
            done += written;
        }
        return true;
    }
    for (size_t done = 0; done < text.size();) {
        DWORD written = 0;
        if (!WriteFile(h, text.data() + done, DWORD(text.size() - done), &written, nullptr) || written == 0) return false;
        done += written;
    }
    return true;
}

bool console_shows_blocks(void* handle) {
    HANDLE h = resolve(handle);
    if (!is_console(h)) return false;
    CONSOLE_FONT_INFOEX font{};
    font.cbSize = sizeof font;
    // TrueType console fonts (Consolas, Lucida Console, Cascadia; Windows Terminal) all have the block elements.
    if (GetCurrentConsoleFontEx(h, FALSE, &font) && (font.FontFamily & TMPF_TRUETYPE)) return true;
    // A raster font shows what the output code page has: the OEM pages (437, 850, ...) have the blocks, 1252 not.
    const UINT cp = GetConsoleOutputCP();
    if (cp == CP_UTF8) return false;
    const wchar_t blocks[] = L"▀▄█";
    char out[16];
    BOOL used_default = FALSE;
    const int n = WideCharToMultiByte(cp, WC_NO_BEST_FIT_CHARS, blocks, 3, out, sizeof out, nullptr, &used_default);
    return n == 3 && !used_default;
}
#else
bool write_utf8(std::string_view text, void*) {
    std::cerr.flush();
    return std::fwrite(text.data(), 1, text.size(), stderr) == text.size() && std::fflush(stderr) == 0;
}
bool console_shows_blocks(void*) { return false; }
#endif

// ---- ConsoleStream ------------------------------------------------------------------------------------------------

ConsoleStream::ConsoleStream() : std::ostream(&buf_) {}
ConsoleStream::~ConsoleStream() { flush(); }

ConsoleStream::Buf::~Buf() { sync(); }

ConsoleStream::Buf::int_type ConsoleStream::Buf::overflow(int_type ch) {
    if (ch != traits_type::eof()) pending_ += traits_type::to_char_type(ch);
    return traits_type::not_eof(ch);
}

std::streamsize ConsoleStream::Buf::xsputn(const char* s, std::streamsize n) {
    pending_.append(s, size_t(n));
    return n;
}

int ConsoleStream::Buf::sync() {
    if (pending_.empty()) return 0;
    // a UTF-8 sequence split across two flushes would turn into two replacement characters: keep the tail
    size_t keep = 0;
    for (size_t i = pending_.size(); i > 0 && keep < 4; i--) {
        const unsigned char c = static_cast<unsigned char>(pending_[i - 1]);
        if ((c & 0xC0) != 0x80) {  // a lead byte or ASCII: complete if the sequence it starts is complete
            const size_t need = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
            keep = pending_.size() - (i - 1) < need ? pending_.size() - (i - 1) : 0;
            break;
        }
    }
    const std::string now = pending_.substr(0, pending_.size() - keep);
    pending_.erase(0, pending_.size() - keep);
    return write_utf8(now) ? 0 : -1;
}

// ---- LinkStatusPrinter --------------------------------------------------------------------------------------------

namespace {
const char* const kDash = " \xE2\x80\x93 ";  // " - " as an en dash
const char* const kEllipsis = "\xE2\x80\xA6";

std::string seconds_text(std::string_view ms_text) {
    long long ms = 0;
    for (char c : ms_text) {
        if (c < '0' || c > '9') break;
        ms = ms * 10 + (c - '0');
    }
    const long long s = std::max<long long>(1, std::llround(double(ms) / 1000.0));
    return std::to_string(s) + " s";
}

/// "websocket connect wss://host/link: Could not establish connection" -> "Could not establish connection"
std::string short_reason(std::string reason) {
    const std::string prefix = "websocket connect ";
    if (reason.rfind(prefix, 0) == 0) {
        const auto colon = reason.find(": ", prefix.size());
        if (colon != std::string::npos) reason = reason.substr(colon + 2);
    }
    return reason;
}
}  // namespace

LinkStatusPrinter::LinkStatusPrinter(std::string host, std::string account, std::string organisation, Writer writer)
    : host_(std::move(host)), account_(std::move(account)), organisation_(std::move(organisation)), writer_(std::move(writer)) {}

std::string LinkStatusPrinter::connected_line() const {
    return "connected to " + host_ + " as " + account_ + " in '" + organisation_ + "'" + kDash + "Ctrl+C stops";
}

void LinkStatusPrinter::emit(const std::string& line) {
    if (writer_) writer_(line);
}

void LinkStatusPrinter::starting(bool is_paused) {
    std::lock_guard<std::mutex> l(mu_);
    if (is_paused) emit("fairyfly link: paused" + std::string(kDash) + "it connects to " + host_ + " after: fairyfly link resume (Ctrl+C stops)");
    else emit("fairyfly link: connecting to " + host_ + " as " + account_ + " in '" + organisation_ + "'" + kEllipsis);
}

void LinkStatusPrinter::paused() {
    std::lock_guard<std::mutex> l(mu_);
    up_ = false;
    emit("paused" + std::string(kDash) + "the AI assistant cannot reach this desktop until: fairyfly link resume");
}

void LinkStatusPrinter::resumed() {
    std::lock_guard<std::mutex> l(mu_);
    emit("resumed" + std::string(kDash) + "connecting to " + host_ + kEllipsis);
}

void LinkStatusPrinter::stopping() {
    std::lock_guard<std::mutex> l(mu_);
    stopping_ = true;
    emit(std::string("stopping") + kEllipsis);
}

void LinkStatusPrinter::stopped() {
    std::lock_guard<std::mutex> l(mu_);
    emit("fairyfly link stopped");
}

void LinkStatusPrinter::on_agent_log(std::string_view message) {
    // libffl/src/link_agent.cpp: "link up: device=... gateway=...", "link down[ (<error>)], reconnecting in <n> ms",
    // "gateway asked to reconnect (<kind>)"
    std::lock_guard<std::mutex> l(mu_);
    if (stopping_) return;
    if (message.rfind("link up:", 0) == 0) {
        emit(ever_connected_ ? "reconnected to " + host_ : connected_line());
        ever_connected_ = up_ = true;
        last_down_.clear();
        return;
    }
    if (message.rfind("gateway asked to reconnect (", 0) == 0) {
        auto kind = message.substr(28);
        if (!kind.empty() && kind.back() == ')') kind.remove_suffix(1);
        last_down_ = "gateway asked: " + std::string(kind);
        return;
    }
    if (message.rfind("link down", 0) == 0) {
        const auto at = message.rfind(", reconnecting in ");
        if (at == std::string_view::npos) return;
        const std::string wait = seconds_text(message.substr(at + 18));
        std::string reason;
        if (message.size() > 9 && message.substr(9, 2) == " (") reason = short_reason(std::string(message.substr(11, at - 1 - 11)));
        if (reason.empty()) reason = last_down_;
        last_down_.clear();
        if (up_) emit("connection to " + host_ + " lost" + (reason.empty() ? "" : " (" + reason + ")") + kDash + "reconnecting in " + wait);
        else emit("could not reach " + host_ + (reason.empty() ? "" : " (" + reason + ")") + kDash + "retrying in " + wait);
        up_ = false;
    }
}

namespace {
class StatusSink : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit StatusSink(std::shared_ptr<LinkStatusPrinter> p) : p_(std::move(p)) {}
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override { p_->on_agent_log(std::string_view(msg.payload.data(), msg.payload.size())); }
    void flush_() override {}
private:
    std::shared_ptr<LinkStatusPrinter> p_;
};
}  // namespace

spdlog::sink_ptr make_status_sink(std::shared_ptr<LinkStatusPrinter> printer) { return std::make_shared<StatusSink>(std::move(printer)); }

std::string url_host(const std::string& url) {
    auto start = url.find("://");
    start = start == std::string::npos ? 0 : start + 3;
    const auto end = url.find_first_of(":/?#", start);
    return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

}  // namespace fairyfly::link
