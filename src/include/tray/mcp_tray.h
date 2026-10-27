#pragma once
// System-tray front end of `fairyfly mcp --tray` (phase 4 of the remote-MCP work).
//
// THREADING MODEL (why it looks like this)
//   SAP GUI scripting is COM in a single-threaded apartment and must run on the interactive desktop.
//   The server's COM executor therefore stays on the PROCESS MAIN THREAD: IServerRunner::run_blocking()
//   is called on it and never returns until the process should end. The tray UI (hidden top-level
//   window, Shell_NotifyIconW icon, popup menu, message pump) runs on a SECOND thread that owns its
//   own message queue. The UI thread talks to the server only through mcp::IServerControl (which is
//   thread-safe by contract); it never touches COM, the transport or the SAP handler.
//
// SEAMS FOR PHASE 1
//   * IServerRunner: implemented by the HTTP server object. run_blocking() = the main-thread loop
//     (COM/STA executor + HTTP workers); control() = the thread-safe IServerControl of that server.
//     A user "Stop" in the tray calls control().request_stop(): the server stops accepting and
//     finishes the running call but run_blocking() KEEPS RUNNING (the tray must be able to Start it
//     again). Only request_quit() (tray "Quit") makes run_blocking() return.
//   * `runner_factory()`: a process-wide std::function the entry point uses to obtain the runner for
//     `mcp --tray`; phase 1 (or the merge in cli_entry.cpp) registers it. Without one the tray
//     refuses to start with TRAY_SERVER_UNAVAILABLE.
//

#include <memory>
#include <mutex>
#include <string>

#include "include/mcp/principal.h"
#include "include/mcp/types.h"
#include "include/tray/tray.h"

namespace fairyfly::tray {

/// Builds the menu exactly as specified: status line, warnings submenu, separator, Start/Stop/Restart,
/// read-only toggle, separator, Open log/audit folder/config, Tokens, Cloud link, Check for updates, separator, Quit.
TrayMenuModel build_menu(const mcp::ServerStatus& status, const TrayState& state);
IconState icon_for(const mcp::ServerStatus& status, const TrayState& state);
/// Text of the balloon shown on left-click (status + warnings). Never contains secrets.
std::string status_balloon_text(const mcp::ServerStatus& status);

/// The runner of the server (phase 1 implements it).
class IServerRunner {
public:
    virtual ~IServerRunner() = default;
    /// Main thread. Blocks until request_quit(); a tray Stop does NOT end it.
    virtual int run_blocking() = 0;
    virtual mcp::IServerControl& control() = 0;
    /// Thread-safe: makes run_blocking() return (after stopping the server).
    virtual void request_quit() = 0;
};

using RunnerFactory = std::function<std::unique_ptr<IServerRunner>(const mcp::ServeOptions&)>;
/// Process-wide registration point (empty until phase 1 sets it).
RunnerFactory& runner_factory();

/// Turns menu actions into effects. Runs on the UI thread; status() calls are thread-safe.
class TrayController : public TrayListener {
public:
    TrayController(mcp::IServerControl& control, IServerRunner* runner, TrayServices services, TrayPaths paths,
                   TrayState state = {});

    TrayMenuModel menu() override;
    TrayView view() override;
    void on_action(TrayAction action) override;
    std::string left_click_text() override;
    void on_tick() override;

    TrayState& state() { return state_; }
    /// Thread-safe: the background update check found a newer release; shown once per version.
    void announce_update(const std::string& version);

private:
    void notify(const std::string& text);

    mcp::IServerControl& control_;
    IServerRunner* runner_;
    TrayServices services_;
    TrayPaths paths_;
    TrayState state_;
    bool announced_ = false;
    bool last_trail_ = false;   ///< the cloud link's audit trail as seen on the previous tick
    std::mutex update_mu_;
    std::string announced_update_;
};

/// Runs the tray around `runner`: single-instance guard, UI thread, server on the calling (main)
/// thread. Returns the process exit code (0 also when another instance already runs).
int run_with_tray(IServerRunner& runner, TrayServices services, SingleInstance& guard, TrayPaths paths,
                  TrayState state = {});

}  // namespace fairyfly::tray
