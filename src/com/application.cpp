#include "include/com/hresult.h"
#include "include/com/wrapper.h"
#include "include/com/invoke.h"
#include "include/com/utf8.h"
#include "include/com/wrapper_helpers.h"
#include "include/core.h"
#include "include/constants.h"
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <iostream>
#include <chrono>
#include <windows.h>

namespace fairyfly {
namespace sap {

// ============================================================================
// ComGuiApplication Implementation
// ============================================================================

namespace {

/// This thread's single-threaded apartment: entered at the first SAP GUI call and left only when the thread ends
/// (thread_local destructor). One apartment per COM thread for its whole life: no SAP GUI object (and no
/// IUIAutomation cached for the thread, html_viewer_reader.cpp) ever outlives the apartment it was made in, however
/// often SAP Logon comes and goes.
struct ThreadApartment {
    bool entered = false;
    int entries = 0;
    ~ThreadApartment() {
        if (entered) CoUninitialize();
    }
};
thread_local ThreadApartment t_apartment;

}  // namespace

void ensure_thread_apartment() {
    if (t_apartment.entered) return;
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr))  // RPC_E_CHANGED_MODE: the thread is in the multithreaded apartment; SAP GUI needs an STA
        throw ComInitializationException("Failed to initialize COM library", hr);
    t_apartment.entered = true;  // S_OK or S_FALSE: one reference, released when the thread ends
    ++t_apartment.entries;
    spdlog::debug("COM apartment (STA) entered for this thread");
}

int thread_apartment_entries() { return t_apartment.entries; }

ComGuiApplication::ComGuiApplication(IDispatchPtr sap_gui_app) : SapGuiObject(sap_gui_app) {
    if (!sap_gui_app) {
        throw ComException("Invalid SAP GUI application pointer");
    }
    spdlog::debug("ComGuiApplication initialized");
}

namespace {

/// The SAPGUI entry of the running object table through SAP's official SapROTWr.SapROTWrapper (AddRef'd), or null.
IDispatch* sapgui_rot_entry() {
    IDispatch* sap_gui = nullptr;
    CLSID clsid_rot;
    HRESULT rot_hr = CLSIDFromProgID(L"SapROTWr.SapROTWrapper", &clsid_rot);
    spdlog::debug("SapROTWr CLSID lookup hr={}", hresult_hex(rot_hr));
    if (SUCCEEDED(rot_hr)) {
        IDispatch* rot_wrapper = nullptr;
        rot_hr = CoCreateInstance(clsid_rot, nullptr, CLSCTX_INPROC_SERVER, IID_IDispatch, (void**)&rot_wrapper);
        spdlog::debug("SapROTWr creation hr={}", hresult_hex(rot_hr));
        if (SUCCEEDED(rot_hr)) {
            OLECHAR* rot_entry_name = (OLECHAR*)L"GetROTEntry";
            DISPID dispid_rot;
            rot_hr = rot_wrapper->GetIDsOfNames(IID_NULL, &rot_entry_name, 1, LOCALE_USER_DEFAULT, &dispid_rot);
            spdlog::debug("SapROTWr GetROTEntry lookup hr={}", hresult_hex(rot_hr));
            if (SUCCEEDED(rot_hr)) {
                VARIANT res;
                VariantInit(&res);
                HRESULT hr_call = invoke(rot_wrapper, dispid_rot, DISPATCH_METHOD, {_variant_t(L"SAPGUI")}, &res);
                spdlog::debug("SapROTWr GetROTEntry invoke hr={}, result type={}",
                              hresult_hex(hr_call), res.vt);
                if (SUCCEEDED(hr_call) && res.vt == VT_DISPATCH && res.pdispVal) {
                    sap_gui = res.pdispVal;
                    spdlog::debug("SapROTWr.SapROTWrapper returned SAPGUI ROT entry successfully");
                }
                if (res.vt != VT_DISPATCH || !res.pdispVal) VariantClear(&res);
            }
            rot_wrapper->Release();
        }
    }
    return sap_gui;
}

/// The GuiApplication behind SAPGUI's ROT entry (SapGuiAuto.GetScriptingEngine), or null. Some builds expose it as
/// a PROPERTYGET, others as a METHOD: PROPERTYGET first.
IDispatchPtr scripting_engine_of(IDispatch* sap_gui) {
    _bstr_t ge_name(L"GetScriptingEngine");
    DISPID ge_id{};
    HRESULT ge_hr = get_dispid_via_typeinfo(sap_gui, ge_name, &ge_id);

    if (SUCCEEDED(ge_hr)) {
        _variant_t vr;

        // Try PROPERTYGET
        HRESULT hr_prop = invoke(sap_gui, ge_id, DISPATCH_PROPERTYGET, {}, &vr);
        if (SUCCEEDED(hr_prop) && vr.vt == VT_DISPATCH && vr.pdispVal) {
            spdlog::debug("Got GuiApplication via ROT GetScriptingEngine (PROPERTYGET)");
            return IDispatchPtr(vr.pdispVal);
        }

        // Try METHOD
        VariantClear(&vr);
        HRESULT hr_meth = invoke(sap_gui, ge_id, DISPATCH_METHOD, {}, &vr);
        if (SUCCEEDED(hr_meth) && vr.vt == VT_DISPATCH && vr.pdispVal) {
            spdlog::debug("Got GuiApplication via ROT GetScriptingEngine (METHOD)");
            return IDispatchPtr(vr.pdispVal);
        }

        spdlog::debug("GetScriptingEngine failed: prop_hr={}, meth_hr={}", hresult_hex(hr_prop), hresult_hex(hr_meth));
    } else {
        spdlog::debug("GetScriptingEngine name resolution failed (hr={})", hresult_hex(ge_hr));
    }
    return nullptr;
}

}  // namespace

IDispatchPtr ComGuiApplication::running_sap_logon() {
    // SapROTWr.SapROTWrapper (SAP's official entry point), then CoGetObject("SAPGUI"). Both only look the SAPGUI
    // entry up in the running object table; neither starts anything.
    if (IDispatch* entry = sapgui_rot_entry()) return IDispatchPtr(entry, false);  // adopts the AddRef'd entry
    IDispatch* sap_gui = nullptr;
    spdlog::debug("Attempting to get SAPGUI ROT entry via CoGetObject...");
    const HRESULT hr = CoGetObject(L"SAPGUI", nullptr, IID_IDispatch, (void**)&sap_gui);
    if (SUCCEEDED(hr) && sap_gui) return IDispatchPtr(sap_gui, false);
    spdlog::debug("No SAPGUI entry in the running object table (hr={})", hresult_hex(hr));
    return nullptr;
}

ComGuiApplicationPtr ComGuiApplication::create() { return create(&ComGuiApplication::running_sap_logon); }

ComGuiApplicationPtr ComGuiApplication::create(const SapLogonLocator& locate) {
    ensure_thread_apartment();  // this thread's apartment for its whole life; never left here

    try {
        // Only ever SAP Logon's own scripting engine. fairyfly never creates one (no SapGui.ScriptingCtrl): an
        // engine started inside this process before SAP Logon ran had no connections forever, and its SAPGUI entry
        // in the running object table made every other process's GetObject("SAPGUI") hang (live 2026-10-28).
        IDispatchPtr sap_gui;
        if (locate) sap_gui = locate();
        if (!sap_gui) {
            throw SapLogonNotRunningException(
                "SAP Logon (saplogon.exe) is not running on this desktop (no SAPGUI entry in the running object "
                "table). Start SAP Logon in the interactive session, then retry; fairyfly never starts a SAP GUI "
                "scripting engine of its own.", MK_E_UNAVAILABLE);
        }
        spdlog::debug("SAPGUI ROT entry obtained successfully - SAP Logon is running");
        IDispatchPtr engine = scripting_engine_of(sap_gui);
        if (!engine) {
            throw ComException("SAP Logon is running, but its scripting engine is not available (is scripting "
                               "enabled in the SAP GUI options?)", E_FAIL);
        }
        return std::make_shared<ComGuiApplication>(engine);
    } catch (const ComException&) {
        throw;
    } catch (const std::exception& e) {
        throw ComException(std::string("Failed to create ComGuiApplication: ") + e.what());
    }
}

bool ComGuiApplication::same_object(const ComGuiApplication& other) const {
    return same_com_object(dispatch_, other.dispatch_);
}

bool same_com_object(IDispatch* a, IDispatch* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    // COM identity: two interface pointers are the same object when their IUnknown pointers are equal. For SAP
    // Logon's objects these are proxies; one apartment always gets the same proxy for the same remote object.
    IUnknownPtr ua, ub;
    if (FAILED(a->QueryInterface(IID_IUnknown, (void**)&ua)) || FAILED(b->QueryInterface(IID_IUnknown, (void**)&ub)))
        return false;
    return ua.GetInterfacePtr() == ub.GetInterfacePtr();
}

int ComGuiApplication::get_connection_count() const {
    spdlog::debug("Enumerating GuiApplication.Connections for connections");

    // Try Connections first (matches VBScript behavior app.Connections)
    IDispatchPtr connections = get_dispatch_property(L"Connections");

    if (!connections) {
        spdlog::warn("GuiApplication.Connections returned null, trying Children");

        // Fallback to Children
        IDispatchPtr children = get_dispatch_property(L"Children");
        if (!children) {
            spdlog::error("Both Connections and Children returned null - SAP GUI scripting may be disabled");
            return 0;
        }

        int count = ::fairyfly::sap::get_int_property(children, "Count");
        spdlog::info("Using Children collection: {} items", count);
        return count;
    }
    
    // Get count from Connections
    int count = ::fairyfly::sap::get_int_property(connections, "Count");
    spdlog::debug("GuiApplication.Connections reports {} connection(s)", count);

    // Log detailed diagnostics
    if (count > 0) {
        spdlog::debug("Connections collection has {} items", count);
    } else {
        spdlog::debug("Connections collection is empty; no SAP connections currently open");
    }

    return count;
}

ComGuiConnectionPtr ComGuiApplication::get_connection(int index) const {
    auto connections_col = connections();
    return connections_col.item(index);
}

SapGuiCollection<ComGuiConnection> ComGuiApplication::connections() const {
    auto connections_dispatch = get_dispatch_property(L"Connections");
    if (!connections_dispatch) {
        spdlog::debug("GuiApplication.Connections not available, trying Children");
        connections_dispatch = get_dispatch_property(L"Children");
    }

    if (!connections_dispatch) {
        spdlog::warn("GuiApplication has no Connections/Children collection");
        return SapGuiCollection<ComGuiConnection>(nullptr);
    }

    return SapGuiCollection<ComGuiConnection>(connections_dispatch);
}

/// GuiApplication.<method>(argument, Sync, Raise) - OpenConnection by SAP Logon entry name or
/// OpenConnectionByConnectionString - and its connection. With `raise_error` a failure throws (with SAP's own
/// description when it gives one), otherwise it is logged and null.
ComGuiConnectionPtr ComGuiApplication::open_connection_with(const wchar_t* method, const char* label,
                                                            const std::string& argument, bool sync, bool raise_error) {
    if (!dispatch_) {
        if (raise_error) throw ComException("GuiApplication dispatch is null");
        return nullptr;
    }

    DISPID dispid = -1;
    HRESULT hr = get_dispid_via_typeinfo(dispatch_, method, &dispid);
    if (FAILED(hr)) {
        _bstr_t name(method);
        hr = dispatch_->GetIDsOfNames(IID_NULL, (LPOLESTR*)&name, 1, LOCALE_USER_DEFAULT, &dispid);
    }

    if (FAILED(hr)) {
        spdlog::error("{} method DISPID not found (hr={})", label, hresult_hex(hr));
        if (raise_error) throw ComException(std::string(label) + " method not found", hr);
        return nullptr;
    }

    // (Description or ConnectString, Sync, Raise)
    _variant_t result;
    EXCEPINFO excepinfo;
    memset(&excepinfo, 0, sizeof(excepinfo));
    UINT argerr = 0;
    hr = invoke(dispatch_, dispid, DISPATCH_METHOD, {bstr_arg(argument), _variant_t(sync), _variant_t(raise_error)},
                &result, &excepinfo, &argerr);

    if (FAILED(hr)) {
        std::string err_detail = "";
        if (excepinfo.bstrDescription) {
            err_detail = com::bstr_to_utf8(excepinfo.bstrDescription);
            SysFreeString(excepinfo.bstrDescription);
        }
        if (excepinfo.bstrSource) SysFreeString(excepinfo.bstrSource);
        if (excepinfo.bstrHelpFile) SysFreeString(excepinfo.bstrHelpFile);

        spdlog::warn("{}('{}') failed: hr={} {}", label, argument, hresult_hex(hr), err_detail);
        if (raise_error) {
            throw ComException(fmt::format("{} failed: {}", label, err_detail.empty() ? fmt::format("hr={}", hresult_hex(hr)) : err_detail), hr);
        }
        return nullptr;
    }

    if (result.vt == VT_DISPATCH && result.pdispVal) {
        spdlog::info("{}('{}') succeeded", label, argument);
        return std::make_shared<ComGuiConnection>(IDispatchPtr(result.pdispVal, true));
    }

    spdlog::warn("{}('{}') returned unexpected result type: {}", label, argument, result.vt);
    return nullptr;
}

ComGuiConnectionPtr ComGuiApplication::open_connection(const std::string& connection_name, bool sync, bool raise_error) {
    return open_connection_with(L"OpenConnection", "OpenConnection", connection_name, sync, raise_error);
}

ComGuiConnectionPtr ComGuiApplication::open_connection_by_connection_string(const std::string& connection_string, bool sync, bool raise_error) {
    return open_connection_with(L"OpenConnectionByConnectionString", "OpenConnectionByConnectionString", connection_string,
                                sync, raise_error);
}

// ============================================================================
// Mouse Hook Implementation for Window Selection
// ============================================================================

// Global state for mouse hook
static HWND g_clicked_hwnd = nullptr;
static HANDLE g_click_event = nullptr;
static HWND g_last_hwnd = nullptr;
static HCURSOR g_crosshair_cursor = nullptr;

// Helper function to check if window is SAP GUI
static bool is_sap_gui_window(HWND hwnd) {
    if (!hwnd) return false;
    
    wchar_t class_name[256];
    GetClassNameW(hwnd, class_name, sizeof(class_name) / sizeof(class_name[0]));
    std::wstring wclass(class_name);
    
    // SAP GUI windows typically have class names containing "SAP"
    return (wclass.find(L"SAP") != std::wstring::npos) ||
           (wclass.find(L"sap") != std::wstring::npos);
}

// Low-level mouse hook callback
LRESULT CALLBACK MouseHookCallback(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0) {
        MSLLHOOKSTRUCT* mouse_data = (MSLLHOOKSTRUCT*)lParam;
        POINT pt = mouse_data->pt;
        HWND current_hwnd = WindowFromPoint(pt);
        
        if (wParam == WM_MOUSEMOVE) {
            // Change cursor to crosshair on mouse move
            if (g_crosshair_cursor) {
                SetCursor(g_crosshair_cursor);
            }
            
            // Only update console if window changed
            if (current_hwnd != g_last_hwnd) {
                g_last_hwnd = current_hwnd;
                
                // Check if this is a SAP window
                bool is_sap = is_sap_gui_window(current_hwnd);
                
                // Log status change
                if (is_sap) {
                    spdlog::debug("[SAP WINDOW DETECTED]");
                } else {
                    spdlog::trace("[Not a SAP window]");
                }
            }
        }
        else if (wParam == WM_LBUTTONDOWN) {
            if (g_click_event) {
                g_clicked_hwnd = current_hwnd;
                spdlog::info("[CLICK DETECTED]");
                SetEvent(g_click_event);
                // Note: We don't consume the event - let it propagate normally
            }
        }
    }
    // Always call next hook - don't consume the event
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

HWND select_window_by_mouse_click(int timeout_seconds) {
    if (timeout_seconds <= 0) timeout_seconds = 10;

    // Load crosshair cursor
    g_crosshair_cursor = LoadCursor(nullptr, IDC_CROSS);
    
    // Create unnamed event for click notification
    g_click_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!g_click_event) {
        g_crosshair_cursor = nullptr;
        throw ComException("Failed to create click event");
    }

    g_clicked_hwnd = nullptr;
    g_last_hwnd = nullptr;

    // Install global mouse hook
    HHOOK mouse_hook = SetWindowsHookEx(WH_MOUSE_LL, MouseHookCallback, nullptr, 0);
    if (!mouse_hook) {
        CloseHandle(g_click_event);
        g_click_event = nullptr;
        g_crosshair_cursor = nullptr;
        throw ComException("Failed to install mouse hook");
    }
    
    // Log hook installation
    spdlog::debug("Mouse hook installed");

    // Show prompt to user
    spdlog::info("Click on SAP GUI window within {} seconds...", timeout_seconds);
    spdlog::info("Hover over SAP windows to see detection status");

    // Process messages to allow hook to work - critical for WH_MOUSE_LL hooks!
    DWORD wait_result = WAIT_TIMEOUT;
    auto start = std::chrono::high_resolution_clock::now();
    
    MSG msg;
    while (true) {
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - start
        ).count();
        
        if (elapsed_ms >= static_cast<long long>(timeout_seconds) * 1000) {
            wait_result = WAIT_TIMEOUT;
            break;
        }
        
        // Check if event was signaled (click detected)
        DWORD check_result = WaitForSingleObject(g_click_event, constants::EVENT_CHECK_TIMEOUT_MS);
        if (check_result == WAIT_OBJECT_0) {
            wait_result = WAIT_OBJECT_0;
            break;
        }
        
        // Process messages to allow hook to work
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    // Unhook
    UnhookWindowsHookEx(mouse_hook);
    CloseHandle(g_click_event);
    g_click_event = nullptr;
    g_crosshair_cursor = nullptr;

    if (wait_result == WAIT_TIMEOUT) {
        throw ComException("Window selection timeout - no click detected within " +
                          std::to_string(timeout_seconds) + " seconds");
    }

    if (wait_result != WAIT_OBJECT_0) {
        throw ComException("Window selection failed");
    }

    if (!g_clicked_hwnd) {
        throw ComException("No window selected");
    }

    // Verify it's a valid window
    wchar_t class_name[256];
    GetClassNameW(g_clicked_hwnd, class_name, sizeof(class_name) / sizeof(class_name[0]));

    const std::string aclass = text::wide_to_utf8_lossy(class_name);

    spdlog::debug("Selected window HWND=0x{:08X}, class={}",
                 reinterpret_cast<uintptr_t>(g_clicked_hwnd),
                 aclass);

    return g_clicked_hwnd;
}

SAPGuiWindow ComGuiApplication::find_window_by_hwnd(HWND target_hwnd) const {
    spdlog::debug("Searching for HWND=0x{:08X} in SAP connections", 
                 reinterpret_cast<uintptr_t>(target_hwnd));
    
    // Get window title and class for the clicked window
    wchar_t window_title[512];
    wchar_t class_name[256];
    GetWindowTextW(target_hwnd, window_title, sizeof(window_title) / sizeof(window_title[0]));
    GetClassNameW(target_hwnd, class_name, sizeof(class_name) / sizeof(class_name[0]));
    
    const std::string title = text::wide_to_utf8_lossy(window_title);
    const std::string aclass = text::wide_to_utf8_lossy(class_name);
    
    spdlog::debug("Clicked window title: '{}', class: '{}'", title, aclass);
    
    int conn_count = get_connection_count();
    spdlog::info("Found {} SAP connections to check", conn_count);

    if (conn_count == 0) {
        spdlog::warn("No SAP connections available");
        return SAPGuiWindow();
    }

    // Track if we found connections but no sessions for better error reporting
    int total_sessions = 0;
    std::optional<SAPGuiWindow> best_match;

    // Search for window by HWND first, then fallback to title
    for (int c = 0; c < conn_count; ++c) {
        try {
            auto connection = get_connection(c);
            if (!connection) {
                spdlog::warn("Connection {} returned null", c);
                continue;
            }

            int sess_count = connection->get_session_count();
            spdlog::info("Connection {} has {} sessions", c, sess_count);
            total_sessions += sess_count;

            if (sess_count == 0) {
                spdlog::debug("Connection {} has no sessions - login may not be complete", c);
                continue;
            }

            for (int s = 0; s < sess_count; ++s) {
                try {
                    spdlog::debug("Checking session {}/{}", c, s);
                    auto session = connection->get_session(s);
                    if (!session) {
                        spdlog::warn("Session {}/{} returned null", c, s);
                        continue;
                    }
                    
                    auto active_window = session->get_active_window();
                    if (!active_window) {
                        spdlog::debug("Session {}/{} has no active window", c, s);
                        continue;
                    }
                    
                    std::string active_title = active_window->get_title();
                    HWND active_hwnd = nullptr;
                    try {
                        active_hwnd = (HWND)(intptr_t)active_window->get_int_property(L"Handle");
                    } catch (...) {}

                    spdlog::info("Session {}/{} active window title: '{}', hwnd: 0x{:X}", c, s, active_title, (uintptr_t)active_hwnd);
                    spdlog::info("Target window title: '{}', hwnd: 0x{:X}", title, (uintptr_t)target_hwnd);

                    // 1. Exact HWND match
                    if (active_hwnd && active_hwnd == target_hwnd) {
                        spdlog::info("Exact HWND match found: conn={}, sess={}", c, s);
                        return SAPGuiWindow(active_title, target_hwnd, c, s);
                    }

                    // 2. Title match fallback
                    if (!title.empty() && (active_title == title || active_title.find(title) != std::string::npos || title.find(active_title) != std::string::npos)) {
                        spdlog::info("Title match found: conn={}, sess={}", c, s);
                        if (!best_match.has_value()) {
                            best_match = SAPGuiWindow(active_title, target_hwnd, c, s);
                        }
                    }
                } catch (const std::exception& e) {
                    spdlog::error("Exception checking session {}/{}: {}", c, s, e.what());
                    continue;
                }
            }
        } catch (const std::exception& e) {
            spdlog::error("Exception checking connection {}: {}", c, e.what());
            continue;
        }
    }

    if (best_match.has_value()) {
        spdlog::info("Returning best match SAP session by title");
        return *best_match;
    }

    if (total_sessions == 0) {
        spdlog::error("No active SAP sessions found");
        spdlog::error("Found {} SAP connections, but none have active sessions", conn_count);
        spdlog::error("Possible causes:");
        spdlog::error("  1. You haven't logged into SAP yet");
        spdlog::error("  2. Server-side scripting is disabled (sapgui/user_scripting = FALSE)");
        spdlog::error("  3. No transaction is currently open");
    } else {
        spdlog::warn("No matching SAP session found for clicked window");
        spdlog::warn("Found {} active sessions across {} connections", total_sessions, conn_count);
        spdlog::warn("Make sure you clicked on an active SAP transaction window");
    }

    // Not found - return invalid window
    return SAPGuiWindow();
}

} // namespace sap
} // namespace fairyfly

