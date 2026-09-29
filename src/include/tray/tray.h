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
// Every OS effect (tray icon/window, registry, processes, files, mutex, message boxes) sits behind a
// small interface below; unit tests use fakes, the Win32 implementations live in src/tray/tray_win32.cpp
// and are exercised by hidden manual tests only.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "include/mcp/principal.h"
#include "include/mcp/types.h"

namespace fairyfly::tray {

// ---- Pure model ---------------------------------------------------------------------------------
enum class IconState { Running, Warning, Stopped, Error };   ///< green, yellow, grey, red

enum class TrayAction {
    None,
    Start, Stop, Restart,
    ToggleReadOnly,
    OpenLog, OpenAuditFolder, OpenConfig,
    TokenList, TokenCreate,
    Quit,
};

/// State that belongs to the tray itself (not to the server).
struct TrayState {
    bool hard_read_only = false;   ///< FAIRYFLY_READ_ONLY=1: the toggle is locked
    std::string last_error;        ///< non-empty: the server failed (red icon)
    bool start_notice = true;      ///< balloon once when the server is up (always shown when it has warnings)
};

struct MenuItem {
    std::string label;
    TrayAction action = TrayAction::None;
    bool enabled = true;
    bool checked = false;
    bool checkable = false;
    bool separator = false;
    std::vector<MenuItem> children;   ///< non-empty: a submenu
};

struct TrayMenuModel {
    std::vector<MenuItem> items;
    IconState icon = IconState::Stopped;
    std::string tooltip;   ///< at most 127 characters (NOTIFYICONDATA limit)
};

/// Builds the menu exactly as specified: status line, warnings submenu, separator, Start/Stop/Restart,
/// read-only toggle, separator, Open log/audit folder/config, Tokens, separator, Quit.
TrayMenuModel build_menu(const mcp::ServerStatus& status, const TrayState& state);
IconState icon_for(const mcp::ServerStatus& status, const TrayState& state);
/// Text of the balloon shown on left-click (status + warnings). Never contains secrets.
std::string status_balloon_text(const mcp::ServerStatus& status);

// ---- Pure command-line builders -----------------------------------------------------------------
/// Windows command-line quoting of one argument (CommandLineToArgvW rules).
std::string quote_arg(const std::string& arg);
std::string join_command_line(const std::string& exe, const std::vector<std::string>& args);

struct RelaunchPlan {
    bool relaunch = false;               ///< parent must spawn the detached child and exit
    std::string command_line;            ///< quoted exe + the same args
    std::vector<std::pair<std::string, std::string>> extra_env;   ///< {"FAIRYFLY_TRAY_CHILD","1"}
    bool no_window = true;               ///< CREATE_NO_WINDOW | DETACHED_PROCESS
};
/// `args` = argv without the program name. Relaunch only when --tray was given, this is not already
/// the marked child and a console is attached.
RelaunchPlan plan_relaunch(const std::string& exe, const std::vector<std::string>& args, bool tray_requested,
                           bool is_child, bool has_console);

inline constexpr const char* kTrayChildEnv = "FAIRYFLY_TRAY_CHILD";
inline constexpr const char* kTrayMutexName = "Local\\fairyfly-tray";
inline constexpr const char* kAutostartValueName = "fairyfly-mcp";

/// Value of HKCU\...\Run\fairyfly-mcp: "<exe>" mcp --tray [-c "<config>"].
std::string autostart_command(const std::string& exe, const std::string& config_path);
/// `cmd.exe /k` command line that runs `fairyfly <args>` in a console that stays open.
std::string console_command_line(const std::string& exe, const std::vector<std::string>& args);

/// %LOCALAPPDATA%\fairyfly\logs (fallback: temp dir), used for mcp.log.
std::filesystem::path log_directory(const std::function<std::string(const char*)>& env);

// ---- OS interfaces (fakes in tests) ---------------------------------------------------------------
class FileOpener {
public:
    virtual ~FileOpener() = default;
    virtual bool open(const std::filesystem::path& path) = 0;   ///< file or folder, default shell action
};

struct LaunchRequest {
    std::string command_line;   ///< complete, already quoted
    bool new_console = false;   ///< CREATE_NEW_CONSOLE (token shortcuts)
    bool detached = false;      ///< CREATE_NO_WINDOW | DETACHED_PROCESS (tray relaunch)
    std::vector<std::pair<std::string, std::string>> extra_env;
};
class ProcessLauncher {
public:
    virtual ~ProcessLauncher() = default;
    /// Returns the new process id, or nullopt on failure.
    virtual std::optional<std::uint32_t> launch(const LaunchRequest& request) = 0;
};

class RegistryRunKey {
public:
    virtual ~RegistryRunKey() = default;
    virtual bool set(const std::string& name, const std::string& command) = 0;
    virtual bool remove(const std::string& name) = 0;
    virtual std::optional<std::string> get(const std::string& name) = 0;
};

/// Named-mutex based single-instance guard.
class SingleInstance {
public:
    virtual ~SingleInstance() = default;
    virtual bool acquire() = 0;   ///< true = we are the only instance and now hold the guard
    virtual bool exists() = 0;    ///< probe without acquiring (another instance holds it)
};

class Clipboard {
public:
    virtual ~Clipboard() = default;
    virtual bool set_text(const std::string& text) = 0;
};

// ---- UI host ------------------------------------------------------------------------------------
struct TrayView {
    IconState icon = IconState::Stopped;
    std::string tooltip;
};

/// Implemented by TrayController; called by the host ON THE UI THREAD.
class TrayListener {
public:
    virtual ~TrayListener() = default;
    virtual TrayMenuModel menu() = 0;
    virtual TrayView view() = 0;
    virtual void on_action(TrayAction action) = 0;
    virtual std::string left_click_text() = 0;
    /// Periodic (about every 2 s) UI-thread tick: announcements; the host then re-reads view().
    virtual void on_tick() = 0;
};

/// The Win32 layer (thin): hidden window, notify icon, popup menu, TaskbarCreated re-add, message box.
class TrayHost {
public:
    virtual ~TrayHost() = default;
    /// UI thread body: creates window + icon, calls `ready(ok)` once, pumps messages until post_quit().
    /// Returns false when the window/icon could not be created (ready(false) was called).
    virtual bool run(TrayListener& listener, const std::function<void(bool)>& ready) = 0;
    /// Thread-safe: ends run().
    virtual void post_quit() = 0;
    /// Thread-safe: re-reads listener.view() (icon + tooltip).
    virtual void refresh() = 0;
    /// Thread-safe: shows a balloon notification.
    virtual void balloon(const std::string& title, const std::string& text) = 0;
    /// Called on the UI thread from on_action: modal Yes/No.
    virtual bool confirm(const std::string& title, const std::string& text) = 0;
    /// Modal information box ("already running"); may be called without a running message loop.
    virtual void message_box(const std::string& title, const std::string& text) = 0;
};

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

// ---- Controller -----------------------------------------------------------------------------------
struct TrayPaths {
    std::filesystem::path log_file;
    std::filesystem::path audit_dir;
    std::filesystem::path config_file;
    std::string exe;             ///< full path of fairyfly.exe (token shortcuts)
};

struct TrayServices {
    TrayHost* host = nullptr;
    FileOpener* opener = nullptr;
    ProcessLauncher* launcher = nullptr;
};

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

private:
    void notify(const std::string& text);

    mcp::IServerControl& control_;
    IServerRunner* runner_;
    TrayServices services_;
    TrayPaths paths_;
    TrayState state_;
    bool announced_ = false;
};

/// Runs the tray around `runner`: single-instance guard, UI thread, server on the calling (main)
/// thread. Returns the process exit code (0 also when another instance already runs).
int run_with_tray(IServerRunner& runner, TrayServices services, SingleInstance& guard, TrayPaths paths,
                  TrayState state = {});

// ---- Entry point ----------------------------------------------------------------------------------
struct TrayEntry {
    bool tray_requested = false;
    bool is_child = false;
    bool has_console = false;
    bool install_autostart = false;
    bool remove_autostart = false;
    std::string exe;
    std::vector<std::string> args;   ///< argv without the program name
    std::string config_path;         ///< for the autostart command; may be empty
};

struct TrayEntryServices {
    RegistryRunKey* registry = nullptr;
    ProcessLauncher* launcher = nullptr;
    SingleInstance* guard = nullptr;
    std::function<void(const std::string&)> print;   ///< stdout line
};

enum class TrayEntryOutcome {
    NotTray,            ///< --tray absent: the caller runs the normal server path
    AutostartDone,      ///< --install-autostart / --remove-autostart handled, exit code in `exit_code`
    Relaunched,         ///< detached child started, parent exits 0
    AlreadyRunning,     ///< another tray instance exists, exit 0
    RunHere,            ///< this process is the tray child: the caller calls run_with_tray
    Failed,             ///< exit_code != 0
};
struct TrayEntryResult {
    TrayEntryOutcome outcome = TrayEntryOutcome::NotTray;
    int exit_code = 0;
    std::string message;
};

/// Pure decision + effects behind interfaces (no server, no UI): autostart install/remove, relaunch,
/// already-running detection.
TrayEntryResult decide_tray_entry(const TrayEntry& entry, const TrayEntryServices& services);

} // namespace fairyfly::tray
