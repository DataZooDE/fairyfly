#pragma once
// The trays' shared parts: menu model, command-line builders, OS interfaces (fakes in tests), UI host, entry decision,
// and the link tray (`link run --tray`). The MCP server's tray is in mcp_tray.h; this header does not depend on mcp/.
//
// Every OS effect (tray icon/window, registry, processes, files, mutex, message boxes) sits behind a
// small interface below; unit tests use fakes, the Win32 implementations live in src/tray/tray_win32.cpp
// and are exercised by hidden manual tests only.

#include "include/tray/tray_contract.h"
#include "include/link/link_state_names.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>


namespace fairyfly::tray {

// ---- Pure model ---------------------------------------------------------------------------------
/// The mark in the tray's own colour (link: teal, MCP: violet); a yellow or red badge for warning/paused and error, faded
/// when stopped.
enum class IconState { Running, Warning, Stopped, Error };

/// Which tray a process shows: its icon colour, window title (a second start finds the running one by it) and texts.
enum class TrayKind { Mcp, Link };

enum class TrayAction {
    None,
    Start, Stop, Restart,
    ToggleReadOnly,
    OpenLog, OpenAuditFolder, OpenConfig,
    TokenList, TokenCreate,
    LinkPair, LinkStart, LinkPause, LinkResume, LinkStatus,   ///< "Cloud link" submenu (D10)
    LinkToggleWrites,                                          ///< link tray: "Allow changes in SAP"
    UpdateCheck,                                               ///< `fairyfly update check` in a console
    StartMcpServer,                                            ///< link tray: the local MCP server in its own tray
    ToggleAutostart,                                           ///< link tray: "Start with Windows"
    Quit,
};

/// What the tray shows about the desktop's cloud link (`fairyfly link`), read from the pairing state and the running
/// link's status file. Never contains keys or tokens.
struct LinkView {
    bool paired = false;
    std::string organisation, account;
    std::string state = link::state::kNotRunning;   ///< link::state: connected | paused | not running
    bool paused = false;
    /// The audit trail (the running link's status file): while on, the person at this desktop must see it.
    bool trail_on = false;
    std::string trail_mode = "every";    ///< every | changes
    int trail_retention_days = 90;
    bool trail_blur = true;
    /// The desktop clock's offset to the gateway in seconds (> 0: behind) while the link refuses calls for clock skew
    /// (the running link's status file); 0 when the clock is fine.
    std::int64_t clock_skew_s = 0;
    /// A call SAP GUI has not answered for 60 s or more (the running link's status file, `stuck_call`): its tool, its
    /// connection ("" when it named none) and the seconds so far. Empty `stuck_tool`: none.
    std::string stuck_tool, stuck_connection;
    std::int64_t stuck_s = 0;
};

/// The menu line, tooltip part and balloon while the desktop clock is off (clock_skew_s != 0); empty otherwise.
std::string clock_skew_text(const LinkView& link);
/// The menu line and balloon while SAP GUI does not answer a call (stuck_tool set); empty otherwise.
std::string stuck_call_text(const LinkView& link);

/// The persistent menu entry while the audit trail is on; empty while it is off.
std::string trail_menu_text(const LinkView& link);
/// The balloon when the audit trail turns on or off.
std::string trail_balloon_text(const LinkView& link);

/// State that belongs to the tray itself (not to the server).
struct TrayState {
    bool hard_read_only = false;   ///< FAIRYFLY_READ_ONLY=1: the toggle is locked
    std::string last_error;        ///< non-empty: the server failed (red icon)
    bool start_notice = true;      ///< balloon once when the server is up (always shown when it has warnings)
    LinkView link;                 ///< refreshed on every tick
    bool self_update = true;       ///< "Check for updates"; false in an MSIX build (packaging::self_updates)
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

/// The menu entries both trays build from.
MenuItem item(std::string label, TrayAction action, bool enabled = true);
MenuItem separator();
/// "fairyfly <version>", greyed out above "Check for updates": which fairyfly answers.
MenuItem version_item();
/// A tooltip as the notification area shows it: at most 127 UTF-16 units, cut between characters, with "...".
std::string fit_tooltip(const std::string& text);

struct TrayMenuModel {
    std::vector<MenuItem> items;
    IconState icon = IconState::Stopped;
    std::string tooltip;   ///< fit_tooltip(): at most 127 UTF-16 units (NOTIFYICONDATA limit)
};

// ---- Pure command-line builders -----------------------------------------------------------------
/// Windows command-line quoting of one argument (CommandLineToArgvW rules).
std::string quote_arg(const std::string& arg);
std::string join_command_line(const std::string& exe, const std::vector<std::string>& args);

struct RelaunchPlan {
    bool relaunch = false;               ///< parent must spawn the detached child and exit
    std::string command_line;            ///< quoted exe + the same args
    std::vector<std::pair<std::string, std::string>> extra_env;   ///< {config::env::kTrayChild, "1"}
    bool no_window = true;               ///< CREATE_NO_WINDOW | DETACHED_PROCESS
};
/// `args` = argv without the program name. Relaunch only when --tray was given, this is not already
/// the marked child and a console is attached.
RelaunchPlan plan_relaunch(const std::string& exe, const std::vector<std::string>& args, bool tray_requested,
                           bool is_child, bool has_console);

/// The windowless starter next to fairyfly.exe (Windows subsystem, so Explorer opens no console for it).
inline constexpr const char* kTrayLauncherName = "fairyfly-tray.exe";
inline constexpr const char* kTrayMutexName = "Local\\fairyfly-tray";
inline constexpr const char* kAutostartValueName = contract::kMcpRunValue;
inline constexpr const char* kLinkTrayMutexName = "Local\\fairyfly-link-tray";
inline constexpr const char* kLinkAutostartValueName = contract::kLinkRunValue;
/// Exit code of `link run` when another link already serves the pairing (LINK_ALREADY_RUNNING from claim_link_run):
/// the link tray then waits for that link instead of reporting a failure.
inline constexpr int kLinkAlreadyRunningExitCode = 3;

/// Value of HKCU\...\Run\fairyfly-mcp: "<exe>" mcp --tray [-c "<config>"].
std::string autostart_command(const std::string& exe, const std::string& config_path);
/// Value of HKCU\...\Run\fairyfly-link: "<exe>" and the `link run --tray` arguments (argv without the program
/// name) without --install-autostart/--remove-autostart; --tray is added when missing.
std::string link_autostart_command(const std::string& exe, const std::vector<std::string>& args);
/// The program the Run values start: fairyfly-tray.exe next to `exe` when it is there (no console at logon), else `exe`.
std::string tray_start_program(const std::string& exe, const std::function<bool(const std::filesystem::path&)>& exists);
/// `cmd.exe /k` command line that runs `fairyfly <args>` in a console that stays open.
std::string console_command_line(const std::string& exe, const std::vector<std::string>& args);

/// Arguments (without the program) that start the local MCP server in its own tray from the link tray: the tray
/// needs the HTTP transport, so `mcp --tray` alone would refuse to start (TRAY_REQUIRES_HTTP).
std::vector<std::string> mcp_tray_args();

/// A startup error of a tray mode (`mcp --tray`, `link run --tray`) must reach the person. The console only does when
/// it stays open after this process ends, i.e. a shell shares it. Started from the Start menu, the autostart or the
/// link tray the console is this process's own (or there is none) and vanishes with it: then a message box as well.
/// `console_processes` = the processes attached to this console (GetConsoleProcessList), 0 without one.
bool startup_error_needs_dialog(bool is_tray_child, bool has_console, unsigned console_processes);

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

/// Small per-user settings of the trays (HKCU, contract::kSettingsKey).
class TraySettings {
public:
    virtual ~TraySettings() = default;
    virtual std::optional<std::string> get(const std::string& name) = 0;
    virtual bool set(const std::string& name, const std::string& value) = 0;
};

/// "Start with Windows" of the link tray: its Run value, on by default.
struct AutostartPlan {
    std::string name;                 ///< the Run value (kLinkAutostartValueName)
    std::string line;                 ///< what "Start with Windows" writes (link_autostart_command of the start program)
    std::string exe;                  ///< fairyfly.exe; a Run value that starts it directly is moved to `program`
    std::string program;              ///< tray_start_program(exe)
};
/// The values the trays keep in TraySettings: every name lives here (docs/MSIX.md lists them for the package move).
inline constexpr const char* kAutostartDefaultedSetting = "link-autostart-defaulted";
inline constexpr const char* kLinkAllowChangesSetting = "link_allow_changes";   ///< "Allow changes in SAP" (approval::Standing)
inline constexpr const char* kTraySettingNames[] = {kAutostartDefaultedSetting, kLinkAllowChangesSetting};
/// At the start of a tray that fairyfly-tray.exe started (`launched`): the very first time it turns "Start with Windows"
/// on (and notes that, so turning it off in the menu stays off); a Run value of an older version that starts fairyfly.exe
/// (a console at every logon) starts `program` instead, its options kept. Started any other way it changes nothing.
/// Returns true when the Run value was written.
bool settle_autostart(const AutostartPlan& plan, bool launched, RegistryRunKey& run, TraySettings& settings);

/// Named-mutex based single-instance guard.
class SingleInstance {
public:
    virtual ~SingleInstance() = default;
    virtual bool acquire() = 0;   ///< true = we are the only instance and now hold the guard
    virtual bool exists() = 0;    ///< probe without acquiring (another instance holds it)
};

/// The desktop's cloud link as the tray sees it (implemented in src/link, a fake in tests).
class LinkControl {
public:
    virtual ~LinkControl() = default;
    virtual LinkView view() = 0;
    virtual bool set_paused(bool paused) = 0;   ///< `fairyfly link pause|resume`; a running link follows within a second
};

/// "Allow changes in SAP" of the link tray: the standing approval at this desktop (approval::Standing). On: the
/// cloud's changes run without asking; off: every change asks at this desktop. Saved for the user.
class LinkWriteMode {
public:
    virtual ~LinkWriteMode() = default;
    virtual bool allowed() = 0;   ///< the switch is on
    virtual bool locked() = 0;    ///< --read-only / FAIRYFLY_READ_ONLY: no change can run here, the switch is off
    virtual bool enforced() { return false; }   ///< the organisation requires approval: the switch is off
    virtual bool set_allowed(bool allowed) = 0;   ///< false: it could not be switched (or saved)
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
    /// A second start of the same tray: asks the running one to open its menu at its icon. False when there is none to
    /// ask (the caller then says "already running" in a message box).
    virtual bool show_running_instance() { return false; }
};

/// Prints "error <code>: <message>" through `print` and, with `dialog` and a host, shows it in a message box titled
/// `title`. Returns `exit_code`.
int report_startup_error(const std::string& title, const std::string& code, const std::string& message, int exit_code,
                         bool dialog, TrayHost* host, const std::function<void(const std::string&)>& print);

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
    LinkControl* link = nullptr;   ///< optional: without it the Cloud link submenu only offers pairing
    /// optional: asks the update channel once, in the background, after update_check_delay; returns the newer
    /// version or nullopt. Runs on its own thread; may block (network) and is never waited for on Quit.
    std::function<std::optional<std::string>()> latest_update;
    std::chrono::milliseconds update_check_delay{30000};
};

/// The effects both trays' menus share: open a file or folder, run a command in a console that stays open, start
/// fairyfly detached (another tray). A failure becomes a notice; without an opener or launcher nothing happens.
struct TrayActions {
    FileOpener* opener = nullptr;
    ProcessLauncher* launcher = nullptr;
    std::string exe;                                     ///< fairyfly.exe
    std::function<void(const std::string&)> notify;

    void open(const std::filesystem::path& path, const std::string& what) const;
    void console(const std::vector<std::string>& args) const;
    /// `fairyfly <args>` without a window (its own tray); false when it could not be started.
    bool start_detached(const std::vector<std::string>& args) const;
};

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
    std::string autostart_name = kAutostartValueName;   ///< the Run value
    std::string autostart_line;      ///< non-empty: registered instead of autostart_command(exe, config_path)
    std::string name = "fairyfly MCP tray";   ///< in the console messages
    /// false in an MSIX build (packaging::run_key_autostart): the package's StartupTask starts the tray, and
    /// --install-autostart/--remove-autostart write nothing (TRAY_AUTOSTART_PACKAGED)
    bool run_key_autostart = true;
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

// ---- Link tray (`fairyfly link run --tray`) --------------------------------------------------------
// The cloud link on its own, without the MCP server: `link run` stays on the main (COM) thread, the tray UI runs on
// its own thread and sees the link only through LinkControl (the pairing state and the status file).

/// The local MCP server's tray as the link tray's menu shows it ("Start local MCP server").
struct McpTrayView {
    bool available = false;   ///< false: no item (nothing to start it with)
    bool running = false;     ///< its single-instance guard is held: the item is checked and disabled
};

/// "Allow changes in SAP" as the menu shows it.
struct LinkWritesView {
    bool available = false;   ///< false: no switch (the item is not shown)
    bool allowed = false;
    bool locked = false;      ///< --read-only / FAIRYFLY_READ_ONLY
    bool enforced = false;    ///< the organisation requires approval
};

/// "Start with Windows" as the menu shows it.
struct AutostartView {
    bool available = false;   ///< false: no item (no Run key access)
    bool on = false;
};

/// Status line, Pause/Resume, Allow changes in SAP, Health check, Pair again (unpaired: Pair with the fairyfly cloud),
/// Start local MCP server, Start with Windows, Open log/audit trail folder, Check for updates (not without
/// `self_update`: an MSIX build), Quit.
TrayMenuModel build_link_menu(const LinkView& link, const LinkWritesView& writes = {}, const McpTrayView& mcp = {},
                              const AutostartView& autostart = {}, bool self_update = true);

struct LinkTrayServices {
    TrayHost* host = nullptr;
    FileOpener* opener = nullptr;
    ProcessLauncher* launcher = nullptr;
    LinkControl* link = nullptr;
    LinkWriteMode* writes = nullptr;   ///< optional: "Allow changes in SAP"
    SingleInstance* mcp_tray = nullptr;   ///< optional probe of the MCP tray's guard (kTrayMutexName); never acquired
    RegistryRunKey* run_key = nullptr;    ///< optional: "Start with Windows"
    AutostartPlan autostart;              ///< the Run value that item switches
    std::function<void()> stop_link;   ///< thread-safe: makes the running `link run` return
    /// Unpaired at start: how often run_link_with_tray looks whether `link pair` (from the menu) has paired the desktop.
    std::chrono::milliseconds pairing_poll{1000};
    /// optional: true while another link (e.g. `link run` in a console) serves this pairing; the tray then waits, with
    /// a notice, instead of failing, and starts its link once that one ends. Polled every pairing_poll.
    std::function<bool()> link_busy;
    bool self_update = true;   ///< "Check for updates"; false in an MSIX build (packaging::self_updates)
};

class LinkTrayController : public TrayListener {
public:
    LinkTrayController(LinkTrayServices services, TrayPaths paths);

    TrayMenuModel menu() override;
    TrayView view() override;
    void on_action(TrayAction action) override;
    std::string left_click_text() override;
    void on_tick() override;

    const LinkView& link() const { return link_; }
    bool quit_requested() const { return quit_requested_; }

private:
    void notify(const std::string& text);
    LinkWritesView writes_view();
    McpTrayView mcp_view();
    AutostartView autostart_view();

    LinkTrayServices services_;
    TrayPaths paths_;
    LinkView link_;
    std::int64_t last_clock_skew_ = 0;   ///< the clock skew seen on the previous tick
    std::string last_stuck_tool_;        ///< the stuck call seen on the previous tick
    std::string last_state_;   ///< the state seen on the previous tick, for the change notices
    bool last_trail_ = false;  ///< the audit trail as seen on the previous tick
    std::atomic<bool> quit_requested_{false};
};

/// Runs the link tray around `run_link` (called on the calling thread, the COM thread): single-instance guard, UI
/// thread, and a message box when the link ends by itself with an error. An unpaired desktop does not end the tray: it
/// says so in a balloon, offers "Pair with the fairyfly cloud" and starts `run_link` once the pairing state appears
/// (Quit before that returns 0). Returns the exit code of `run_link`.
int run_link_with_tray(const std::function<int()>& run_link, LinkTrayServices services, SingleInstance& guard,
                       TrayPaths paths);

/// The tray's UI thread (window, icon, message pump). Ending it (the destructor) posts quit to the host and joins.
class UiThread {
public:
    /// Starts the thread and waits until the UI is up; nullptr: no UI, the thread has already ended.
    static std::unique_ptr<UiThread> start(TrayHost& host, TrayListener& listener);
    ~UiThread();
    UiThread(const UiThread&) = delete;
    UiThread& operator=(const UiThread&) = delete;

private:
    UiThread(TrayHost& host, std::thread thread) : host_(host), thread_(std::move(thread)) {}
    TrayHost& host_;
    std::thread thread_;
};

} // namespace fairyfly::tray
