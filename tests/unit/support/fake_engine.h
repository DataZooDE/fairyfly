#pragma once
// An AutomationEngine without SAP: records what it was asked and answers with scripted results. For testing
// CommandHandler (connection resolution, read-only refusals, error mapping) without a SAP GUI session.
#include "include/automation_engine.h"
#include "include/sap_gui_scripting.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fairyfly::test {

/// SAP GUI scripting without SAP: records the calls, answers with `next`; no COM session.
class FakeScripting : public SapGuiScripting {
public:
    std::vector<std::string> calls;
    Result next = Result::ok(json::object());
    std::function<void(const std::string& call)> on_call;   ///< runs after each action is recorded, before it answers

    std::shared_ptr<sap::ComGuiSession> session;   ///< what unchecked_session answers (null: no COM session)
    std::string connection_description;           ///< what peek_session_connection_description answers
    std::shared_ptr<sap::ComGuiSession> unchecked_session() const override { return session; }
    void set_owner_window_guard(bool) noexcept override {}
    bool current_session_owner_window_allowed() const noexcept override { return true; }
    audit::SapFacts facts;              ///< what peek_session_facts answers (the live session's SAP facts)
    mutable int facts_reads = 0;        ///< how often SAP was asked for them
    audit::SapFacts peek_session_facts(const std::string&, const std::string&) const noexcept override {
        ++facts_reads;
        return facts;
    }
    std::uintptr_t peek_session_window_handle(const std::string&) const noexcept override { return 0; }
    std::string peek_session_connection_description(const std::string&) const noexcept override { return connection_description; }
    sap::ScreenSnapshot capture_screen_snapshot() const override { return {}; }
    nlohmann::json detect_modal_dialog() const override { return json::object(); }
    Result read_field(const ElementId& e, bool activate_tab) override {
        return record("read_field " + e.path + (activate_tab ? " +tab" : ""));
    }
    Result send_key(int vkey, const std::string& window) override { return record("send_key " + std::to_string(vkey) + " " + window); }
    Result close_popup(int vkey) override { return record("close_popup " + std::to_string(vkey)); }
    Result read_menu(const std::string& window) override { return record("read_menu " + window); }
    Result select_menu(const std::string& path, const std::string& window, bool read_only) override {
        return record("select_menu " + path + " " + window + (read_only ? " ro" : ""));
    }
    Result fill_grid_cell(const ElementId& e, int row, const std::string& column, const std::string& value, bool, bool) override {
        return record("fill_grid_cell " + e.path + " " + std::to_string(row) + " " + column + "=" + value);
    }
    Result select_grid_row(const ElementId& e, int row, const std::string& column) override {
        return record("select_grid_row " + e.path + " " + std::to_string(row) + " " + column);
    }
    Result doubleclick_grid_cell(const ElementId& e, int row, const std::string& column) override {
        return record("doubleclick_grid_cell " + e.path + " " + std::to_string(row) + " " + column);
    }

private:
    Result record(std::string call) {
        calls.push_back(call);
        if (on_call) on_call(call);
        return next;
    }
};

class FakeEngine : public AutomationEngine {
public:
    /// Set to give the engine SAP GUI scripting (key send, popups, menus, grid cells).
    FakeScripting* scripting_fake = nullptr;
    SapGuiScripting* scripting() override { return scripting_fake; }

    /// What get_application_info answers (the SAP GUI connections and sessions the doctor reports).
    nlohmann::json application_info = nlohmann::json::object();
    /// What engine_status answers (SAP Logon found, not running, scripting engine missing).
    EngineStatus status;
    int refreshes = 0;   ///< how often SAP Logon was looked up again (refresh_application)
    void refresh_application() override { ++refreshes; }
    EngineStatus engine_status() const override { return status; }
    Result start_sap_logon() override { return record("start_sap_logon"); }
    std::uint64_t generation = 1;   ///< what application_generation answers (++: SAP Logon was replaced)
    std::uint64_t application_generation() const override { return generation; }

    // ---- what the handler asked ----
    std::vector<std::string> calls;  ///< "fill_field wnd[0]/usr/txtA=x", "select_session /app/con[0]/ses[0]", ...
    int count(const std::string& prefix) const {
        int n = 0;
        for (const auto& c : calls) n += c.rfind(prefix, 0) == 0;
        return n;
    }

    // ---- scripted answers ----
    std::map<std::string, bool> live_sessions;  ///< session id -> alive (select_session / validate_session)
    /// session id -> SAP server session key of the session at that path now; a saved key must match it (none: any key)
    std::map<std::string, std::string> session_keys;
    Result next = Result::ok(json::object());   ///< result of every action
    std::string active_window = "wnd[0]";
    std::string server_key;                                  ///< what current_server_session_key answers
    std::function<void(const std::string& call)> on_call;   ///< runs after each action is recorded, before it answers

    Result attach_by_click(int) override { return record("attach_by_click"); }
    Result attach_by_session_id(const std::string& id) override { return record("attach_by_session_id " + id); }
    Result launch_connection(const std::string& name, bool) override { return record("launch_connection " + name); }
    Result disconnect() override { return record("disconnect"); }
    Result close_current_session() override { return record("close_current_session"); }
    bool is_connected() const override { return true; }
    bool validate_session(const std::string& id, const std::string& key) const override { return same(id, key); }
    bool validate_session_for_cleanup(const std::string& id, const std::string& key) const override { return same(id, key); }
    bool select_session(const std::string& id, const std::string& key) override {
        calls.push_back("select_session " + id);
        return same(id, key);
    }
    std::string current_server_session_key() const override { return server_key; }
    mutable int key_reads = 0;   ///< how often live_session_key was asked
    /// nullopt: no live session at the path; otherwise its key from session_keys ("" when unknown or unreadable)
    std::optional<std::string> live_session_key(const std::string& id) const override {
        ++key_reads;
        if (!alive(id)) return std::nullopt;
        const auto k = session_keys.find(id);
        return k == session_keys.end() ? std::string() : k->second;
    }
    Result execute_transaction(const std::string& tcode) override { return record("execute_transaction " + tcode); }
    Result click_element(const ElementId& e) override { return record("click_element " + e.path); }
    Result fill_field(const ElementId& e, const std::string& value) override { return record("fill_field " + e.path + "=" + value); }
    Result read_field(const ElementId& e) override { return record("read_field " + e.path); }
    Result press_toolbar_button(const ElementId& e, const std::string& button) override {
        return record("press_toolbar_button " + e.path + " " + button);
    }
    Result press_f4(const ElementId& e) override { return record("press_f4 " + e.path); }
    Result read_screen(bool, bool, int) override { return record("read_screen"); }
    Result read_screen_with_tabs(bool, int, const std::string&) override { return record("read_screen_with_tabs"); }
    Result find_screen(const sap::ScreenFindOptions&) override { return record("find_screen"); }
    Result capture_screenshot(const cli::ScreenshotOptions&) override { return record("capture_screenshot"); }
    nlohmann::json get_application_info() const override { return application_info; }
    WindowId get_active_window_id() const override { return WindowId(active_window); }
    ElementId resolve_element_path(const ElementId& e) const override { return e; }

private:
    bool alive(const std::string& id) const {
        const auto i = live_sessions.find(id);
        return i != live_sessions.end() && i->second;
    }
    /// alive, and (when a key is asked for and the fake knows one) the same server session
    bool same(const std::string& id, const std::string& key) const {
        if (!alive(id)) return false;
        const auto k = session_keys.find(id);
        return key.empty() || k == session_keys.end() || k->second == key;
    }
    Result record(std::string call) {
        calls.push_back(call);
        if (on_call) on_call(call);
        return next;
    }
};

}  // namespace fairyfly::test
