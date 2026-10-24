#pragma once
// What `fairyfly link pair|run` shows the person at the desktop (D10). A Windows console decodes the bytes written
// to it with its code page (437 or 1252 on most desktops), so UTF-8 text such as the QR code's block characters
// came out as mojibake. Text here goes to a console through WriteConsoleW (UTF-16, independent of the code page)
// and to a file or pipe as UTF-8; the QR code falls back to pure ASCII where block characters cannot be shown.
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <streambuf>
#include <string>
#include <string_view>

#include <spdlog/common.h>

#include "qr.hpp"  // libffl: ffy::qr::Matrix

namespace fairyfly::link {

enum class QrStyle {
    Blocks,  ///< UTF-8 half blocks, two module rows per line (libffl's ffy::qr::to_terminal)
    Ascii,   ///< "##" for a light module, two spaces for a dark one, one module row per line
};

/// Both styles draw the light modules (scans on dark terminals, where the text is light) with a 2-module quiet zone.
std::string render_qr(const ffy::qr::Matrix& matrix, QrStyle style);

// ---- `link pair`: which service, and what next -------------------------------------------------------------------
/// The fairyfly cloud, prefilled when `link pair` asks for the service's address.
inline constexpr const char* kDefaultGateway = "https://fairyfly.io";

struct PairGateway {
    std::string address;   ///< the address to use, or the one to prefill when `ask`
    bool ask = false;      ///< ask the person (Enter accepts `address`)
};
/// --gateway, else FAIRYFLY_LINK_GATEWAY: used as given, without a question. Neither: asked, https://fairyfly.io
/// prefilled.
PairGateway pair_gateway(const std::string& flag, const std::string& env);
/// The question. `in_line`: the prefill already stands, editable, in the console's input line; otherwise it is shown as
/// "[<prefill>]" and an empty answer takes it.
std::string pair_gateway_prompt(const std::string& prefill, bool in_line);
/// The typed line without surrounding spaces and the CR; empty (Enter) = `prefill`.
std::string pair_gateway_answer(const std::string& typed, const std::string& prefill);

/// Which address gateway_url_problem() checks: the service's web address (`link pair`) or the link's WebSocket
/// address the service hands out at pairing (state.json link_url, used by `link run`).
enum class GatewayUrlKind { Web, WebSocket };
/// Why `url` must not be used as a gateway address, or nullopt when it may. Parsed with libffl's strict parse_url (the
/// parser the HTTP and WebSocket clients use): no userinfo, no whitespace/control characters/backslashes, no
/// percent-encoding in the authority, IPv6 only in brackets, no fragment (and for Web no query). https:// (wss://) is
/// required; plain http:// (ws://) only for exactly localhost, 127.0.0.1 or [::1] with an optional port.
std::optional<std::string> gateway_url_problem(const std::string& url, GatewayUrlKind kind = GatewayUrlKind::Web);

/// Name of the per-pairing lock (a named mutex in the session) that a serving `link run`, in a console or in the link
/// tray, holds: one link per pairing. Derived from the file's final path (on Windows junctions,
/// symbolic links, subst drives and 8.3 names resolved, Unicode case folded), so any spelling of the file gives one name.
std::string link_run_lock_name(const std::string& state_path);

/// The "next" step `link pair` reports. `link_tray_running`: the link tray runs (it starts the link itself once
/// paired). `link_running`: a link for this pairing already serves, still with the previous pairing.
std::string pair_next_step(bool link_tray_running, bool link_running);

/// `--qr-style`: "blocks", "ascii", or "auto" (blocks when stderr is a console whose font can show them).
QrStyle choose_qr_style(const std::string& requested);

/// Writes UTF-8 text to the stream behind `handle` (a Windows HANDLE; nullptr = stderr): WriteConsoleW when it is a
/// console, the UTF-8 bytes otherwise. False when nothing could be written.
bool write_utf8(std::string_view text, void* handle = nullptr);

/// True when `handle` (nullptr = stderr) is a console whose font and code page can show U+2580, U+2584 and U+2588.
bool console_shows_blocks(void* handle = nullptr);

/// std::ostream onto write_utf8(stderr): libffl's prompts (std::ostream&) reach the console intact as well.
/// Output is passed on at every flush (std::endl, std::flush) and when the stream is destroyed.
class ConsoleStream : public std::ostream {
public:
    ConsoleStream();
    ~ConsoleStream() override;

private:
    class Buf : public std::streambuf {
    public:
        ~Buf() override;
    protected:
        int_type overflow(int_type ch) override;
        std::streamsize xsputn(const char* s, std::streamsize n) override;
        int sync() override;
    private:
        std::string pending_;
    };
    Buf buf_;
};

/// The status lines of `fairyfly link run`. libffl's agent reports its connection only through spdlog (info "link
/// up: ...", warn "link down ..., reconnecting in N ms", info "gateway asked to reconnect (...)"); the printer turns
/// those into one line per change for the person at the desktop.
class LinkStatusPrinter {
public:
    using Writer = std::function<void(const std::string& line)>;
    /// host: gateway host shown (fairyfly.io); account/organisation from the pairing.
    LinkStatusPrinter(std::string host, std::string account, std::string organisation, Writer writer);

    void starting(bool paused);
    void paused();
    void resumed();
    void stopping();
    void stopped();
    /// One libffl log message (spdlog payload, without level or time).
    void on_agent_log(std::string_view message);

    /// The connected line, e.g. "connected to fairyfly.io as a@b.example in 'acme' - Ctrl+C stops".
    std::string connected_line() const;

private:
    void emit(const std::string& line);
    std::string host_, account_, organisation_;
    Writer writer_;
    std::mutex mu_;
    bool ever_connected_ = false, up_ = false, stopping_ = false;
    std::string last_down_;
};

/// spdlog sink for the default logger during `link run`: hands every message to a LinkStatusPrinter.
spdlog::sink_ptr make_status_sink(std::shared_ptr<LinkStatusPrinter> printer);

/// Host part of a ws(s)/http(s) URL ("wss://fairyfly.io/link" -> "fairyfly.io"); the input when it has none.
std::string url_host(const std::string& url);

}  // namespace fairyfly::link
