#pragma once
// What CommandHandler needs beyond AutomationEngine's basic actions: the SAP GUI scripting session itself and the
// actions only SAP GUI scripting offers (keys, popups, menus, grid cells, owner-window checks, read-only peeks).
// ComAutomationEngine implements it; AutomationEngine::scripting() returns it, or nullptr for an engine without
// SAP GUI scripting (a test fake), so callers ask for the capability instead of downcasting to the COM engine.
#include "audit_log.h"
#include "core.h"
#include "screen_snapshot.h"

#include <cstdint>
#include <memory>
#include <string>

namespace fairyfly {
namespace sap {
class ComGuiSession;
}

class SapGuiScripting {
public:
    virtual ~SapGuiScripting() = default;

    /// The session subsequent calls use, WITHOUT the owner-window check of AutomationEngine::attached_com_session:
    /// a caller that acts for a remote owner checks current_session_owner_window_allowed() itself.
    virtual std::shared_ptr<sap::ComGuiSession> unchecked_session() const = 0;
    virtual void set_owner_window_guard(bool enabled) noexcept = 0;
    virtual bool current_session_owner_window_allowed() const noexcept = 0;

    // read-only lookups of a NAMED session; never select or attach anything
    virtual audit::SapFacts peek_session_facts(const std::string& session_id,
                                               const std::string& server_session_key = "") const noexcept = 0;
    virtual std::uintptr_t peek_session_window_handle(const std::string& session_id) const noexcept = 0;
    virtual std::string peek_session_connection_description(const std::string& session_id) const noexcept = 0;

    virtual sap::ScreenSnapshot capture_screen_snapshot() const = 0;
    virtual nlohmann::json detect_modal_dialog() const = 0;
    /// activate_tab: select the inactive tab page the element lives on (and restore the previous one afterwards).
    virtual Result read_field(const ElementId& element, bool activate_tab) = 0;

    virtual Result send_key(int vkey, const std::string& window = "@active") = 0;
    virtual Result close_popup(int vkey = 12) = 0;
    virtual Result read_menu(const std::string& window = "wnd[0]") = 0;
    /// read_only: refuse (READ_ONLY_REFUSED) when the RESOLVED menu item's real text is state-changing.
    virtual Result select_menu(const std::string& menu_path, const std::string& window = "wnd[0]",
                               bool read_only = false) = 0;

    virtual Result fill_grid_cell(const ElementId& element, int row, const std::string& column, const std::string& value,
                                  bool checkbox, bool commit) = 0;
    virtual Result select_grid_row(const ElementId& element, int row, const std::string& column) = 0;
    virtual Result doubleclick_grid_cell(const ElementId& element, int row, const std::string& column) = 0;
};

}  // namespace fairyfly
