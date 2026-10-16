#pragma once
// `fairyfly link` (D10): connects this desktop's fairyfly to the fairyfly cloud gateway, outbound only.
//   link pair    QR + number pairing with a phone; the person at the desktop confirms the account
//   link status  shows the pairing (device, account, organisation, gateway)
//   link run     serves the cloud's tool calls through fairyfly's own MCP dispatcher, in-process
//   link unpair  forgets the pairing on this desktop
// The protocol code is libffl (third_party/libffl), shared with the gateway so it cannot drift.
#include <functional>
#include <string>
#include <vector>

#include "include/audit_log.h"
#include "include/cli_handler.h"
#include "include/commands/global_options.h"
#include "include/core.h"

namespace fairyfly::link {

/// %LOCALAPPDATA%\fairyfly\link\state.json (the device key inside is DPAPI-protected).
std::string default_state_path();

struct LinkOptions {
    std::string state_path;
    // pair
    std::string gateway;          ///< web address of the service, e.g. https://app.fairyfly.cloud
    std::string qr_png;           ///< also write the QR code as a PNG file
    int pair_timeout_s = 300;
    // run
    bool allow_write = false;     ///< like `mcp --allow-write`; each write still needs the gateway's grant and an approval
    bool read_only = false;
    std::vector<std::string> families;
    std::string approve = "desktop";  ///< desktop (dialog) | no
};

Result link_pair(const LinkOptions& options);
Result link_status(const LinkOptions& options);
Result link_unpair(const LinkOptions& options);

/// Runs until Ctrl+C. Returns the process exit code. Calls are audited with source "link".
int run_link(const LinkOptions& options, const std::function<cli::CommandHandler&()>& get_handler,
             const commands::GlobalOptions& global, audit::AuditSink* sink, const std::function<cli::CommandHandler*()>& peek);

}  // namespace fairyfly::link
