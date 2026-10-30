#pragma once
// Windows UI of the desktop approval (src/include/link/desktop_approval.h): a toast under fairyfly's AppUserModelID
// with Approve / Deny / Details, answered also from the notification centre (a COM activator registered while the
// link runs), and the approval window (a task dialog with the link tray's teal icon, every step numbered, a countdown;
// Deny is the default and Esc).
#include <functional>
#include <memory>
#include <string>

#include "include/link/desktop_approval.h"
#include "include/link/toast_identity.h"
#include "include/tray/tray_contract.h"

namespace fairyfly::link::approval {

/// The AppUserModelID of the MSI's notifications: the Start-menu entry's (packaging/fairyfly.wxs) and this process's.
/// A Store build uses its package's tray application instead (this_process_toast_identity).
using tray::contract::kAppUserModelId;

/// This process's toast identity (include/link/toast_identity.h): the build's package and, in a Store build, the
/// package identity Windows gave the process (GetCurrentApplicationUserModelId, GetCurrentPackageFamilyName).
ToastIdentity this_process_toast_identity();

/// True while a person can answer here: an interactive window station, the session active and not locked.
bool desktop_can_ask();

/// The toast's XML: headline, transaction and summary, who it is for and when it expires, Approve / Deny / Details
/// with answer_text() as their arguments (a click on the toast itself is Details).
std::wstring toast_xml(const Request& r);
/// The approval window's text: the assistant, the user, the SAP session and every step, numbered; when the organisation
/// enforces asking, why there is no "Don't ask again".
std::wstring window_text(const Request& r);
/// The window's checkbox; empty (no checkbox) without offer_standing.
std::wstring dont_ask_text(const Request& r);

/// The toast and window UI; `on_answer` receives a notification's answer text (Broker::answer_text).
std::shared_ptr<Ui> make_win32_ui(std::function<void(const std::string&)> on_answer);

/// libffl's Approver for this desktop: the Broker over the Windows UI. `desk.sap` reads the SAP session a change would
/// run in (called once per question, may return an empty context); `desk.can_ask` is set to desktop_can_ask.
std::shared_ptr<ffy::link::Approver> make_desktop_approver(Desk desk, Options options = {});

}  // namespace fairyfly::link::approval
