#pragma once
// `fairyfly link` (D10): connects this desktop's fairyfly to the fairyfly cloud gateway, outbound only.
//   link pair    QR + number pairing with a phone; the person at the desktop confirms the account
//   link status  shows the pairing (device, account, organisation, gateway)
//   link run     serves the cloud's tool calls through fairyfly's own MCP dispatcher, in-process
//   link pause   disconnects a running link until `link resume` (the cloud cannot reach this desktop meanwhile)
//   link resume  lets a paused link connect again
//   link unpair  forgets the pairing on this desktop
// The protocol code is libffl (third_party/libffl), shared with the gateway so it cannot drift.
#include <atomic>
#include <functional>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include "include/audit_log.h"
#include "include/cli_handler.h"
#include "include/commands/global_options.h"
#include "include/core.h"
#include "include/tray/tray.h"

namespace fairyfly::link {

/// %LOCALAPPDATA%\fairyfly\link\state.json (the device key inside is DPAPI-protected).
std::string default_state_path();

struct LinkOptions {
    std::string state_path;
    // pair
    std::string gateway;          ///< web address of the service, e.g. https://app.fairyfly.cloud
    std::string qr_png;           ///< also write the QR code as a PNG file
    std::string qr_style = "auto";  ///< auto | blocks | ascii (see link_console.h)
    int pair_timeout_s = 300;
    // run
    bool allow_write = false;     ///< like `mcp --allow-write`; each write still needs the gateway's grant and an approval
    bool read_only = false;
    std::vector<std::string> families;
    std::string approve = "desktop";  ///< desktop (dialog) | no
    bool tray = false;               ///< `link run --tray`: the link with its own tray icon, logging to link.log
    bool install_autostart = false;  ///< with --tray: start the link tray at logon (HKCU Run fairyfly-link)
    bool remove_autostart = false;
};

/// Where `link pair` talks to the person: by default the console (stderr through WriteConsoleW, stdin) and Ctrl+C.
struct PairIo {
    std::ostream* out = nullptr;              ///< nullptr: the console
    std::istream* in = nullptr;               ///< nullptr: std::cin
    const std::atomic<bool>* stop = nullptr;  ///< nullptr: set by Ctrl+C while pairing
};

/// Shows "Contacting <gateway>..." at once, then the QR code, the code and the number; waits for the phone and the
/// confirmation at the desktop. Ctrl+C (or *io.stop) ends it within a fraction of a second: PAIRING_CANCELLED.
/// io.stop (or Ctrl+C) is looked at before each prompt; a prompt already reading the console is ended by Ctrl+C
/// itself (the console aborts the read), not by io.stop.
Result link_pair(const LinkOptions& options, const PairIo& io = {});

/// Threads that `link pair` or `link run` gave up waiting for after Ctrl+C and that are still running. main()
/// ends the process with std::_Exit when there are any, so they cannot race static destructors and atexit handlers.
int background_threads();
Result link_status(const LinkOptions& options);
Result link_unpair(const LinkOptions& options);
Result link_pause(const LinkOptions& options);
Result link_resume(const LinkOptions& options);

/// The tray's view of the link (pairing state + the running link's status file) and its pause switch.
std::unique_ptr<tray::LinkControl> make_link_control(const std::string& state_path = {});

/// Runs until Ctrl+C (or request_stop()). Returns the process exit code. Calls are audited with source "link".
int run_link(const LinkOptions& options, const std::function<cli::CommandHandler&()>& get_handler,
             const commands::GlobalOptions& global, audit::AuditSink* sink, const std::function<cli::CommandHandler*()>& peek);
/// Thread-safe: makes a running run_link() return, like Ctrl+C (the tray's Quit).
void request_stop();

/// `link run --tray` (and its --install-autostart/--remove-autostart): relaunches detached from the console, then runs
/// run_link() on this thread under the link tray. No MCP server, no HTTP. Returns the process exit code.
int run_link_tray(const LinkOptions& options, const std::function<cli::CommandHandler&()>& get_handler,
                  const commands::GlobalOptions& global, audit::AuditSink* sink, const std::function<cli::CommandHandler*()>& peek);

}  // namespace fairyfly::link
