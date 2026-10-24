#pragma once
// Windows UI of the desktop approval (src/include/link/desktop_approval.h): a toast under fairyfly's AppUserModelID
// with Approve / Deny / Details, answered also from the notification centre (a COM activator registered while the
// link runs), and the approval window (a task dialog with the link tray's teal icon, every step numbered, a countdown;
// Deny is the default and Esc).
#include <functional>
#include <memory>
#include <string>

#include "include/link/desktop_approval.h"

namespace fairyfly::link::approval {

/// The AppUserModelID of fairyfly's notifications: the Start-menu entry's (packaging/fairyfly.wxs) and this process's.
inline constexpr wchar_t kAppUserModelId[] = L"DataZoo.fairyfly";
/// The COM class the notification centre starts or calls for an answer (the shortcut's ToastActivatorCLSID).
inline constexpr wchar_t kToastActivatorClsid[] = L"{DAAF1065-EF17-4CFF-B3D2-EFAF70594959}";

/// True while a person can answer here: an interactive window station, the session active and not locked.
bool desktop_can_ask();

/// The toast's XML: headline, transaction and summary, who it is for and when it expires, Approve / Deny / Details
/// with answer_text() as their arguments (a click on the toast itself is Details).
std::wstring toast_xml(const Request& r);
/// The approval window's text: the assistant, the user, the SAP session and every step, numbered.
std::wstring window_text(const Request& r);

/// The toast and window UI; `on_answer` receives a notification's answer text (Broker::answer_text).
std::shared_ptr<Ui> make_win32_ui(std::function<void(const std::string&)> on_answer);

/// libffl's Approver for this desktop: the Broker over the Windows UI. `sap` reads the SAP session a change would run
/// in (called once per question, may return an empty context).
std::shared_ptr<ffy::link::Approver> make_desktop_approver(std::function<SapContext()> sap, Options options = {});

}  // namespace fairyfly::link::approval
