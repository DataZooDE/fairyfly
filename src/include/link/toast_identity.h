#pragma once
// Under which AppUserModelID fairyfly shows its approval toasts, and what it registers for them (docs/MSIX.md).
// Header-only and pure: the decision takes the build's package and this process's package identity as values, so the
// MSI and the MSIX side are unit-tested in either build; desktop_approval_win32.cpp reads the identity.
//
//   msi   AUMID DataZoo.fairyfly (the Start-menu shortcut's, packaging/fairyfly.wxs): the link registers it in HKCU
//         (Software\Classes\AppUserModelId\DataZoo.fairyfly with CustomActivator, Software\Classes\CLSID\{...}\
//         LocalServer32 = fairyfly-tray.exe) and sets it as the process's explicit AUMID.
//   msix  the package's tray application, <PackageFamilyName>!Tray, whose manifest declares the toast activator
//         (com:ExeServer + desktop:ToastNotificationActivation, packaging/msix/AppxManifest.xml.in). Nothing is written
//         to HKCU (it would land in the package's private hive, which the notification centre never reads) and the
//         process keeps the AUMID Windows gave it. A Store build running without its package identity (an unpackaged
//         copy of the exe, the unit tests) shows no toast: the approval window asks.
#include <string>
#include <string_view>

#include "include/packaging.h"
#include "include/tray/tray_contract.h"

namespace fairyfly::link::approval {

struct ToastIdentity {
    std::wstring app_id;              ///< the notifier's AUMID; empty: no toast (the approval window asks)
    bool register_in_hkcu = false;    ///< write the AUMID and the activator's LocalServer32 to HKCU (MSI only)
    bool set_process_app_id = false;  ///< SetCurrentProcessExplicitAppUserModelID(app_id) (MSI only)
};

/// `process_app_id`: GetCurrentApplicationUserModelId of this process (empty: none), e.g.
/// DataZoo.fairyfly_73q587949tsjt!Tray for the link tray, ...!Cli for `fairyfly` through the execution alias.
/// `package_family`: GetCurrentPackageFamilyName (empty: no package identity), used when the process has a package but
/// no application id. Under MSIX the toast is always the tray application's: only it declares the activator.
inline ToastIdentity toast_identity(packaging::Package build, std::wstring_view process_app_id, std::wstring_view package_family) {
    if (build == packaging::Package::Msi) return {tray::contract::kAppUserModelId, true, true};
    std::wstring_view family = package_family;
    if (const auto bang = process_app_id.find(L'!'); bang != std::wstring_view::npos && bang > 0) family = process_app_id.substr(0, bang);
    if (family.empty()) return {};
    return {std::wstring(family) + L"!" + tray::contract::kPackageTrayApplicationId, false, false};
}

}  // namespace fairyfly::link::approval
