#include "include/com/hresult.h"
#include "include/com/wrapper.h"
#include "include/com/invoke.h"
#include "include/com/utf8.h"
#include "include/com/wrapper_helpers.h"
#include "include/core.h"
#include "include/constants.h"
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <atomic>
#include <iostream>
#include <chrono>
#include <windows.h>

namespace fairyfly {
namespace sap {

namespace {
std::atomic<bool> g_embedded_fallback_allowed{true};
}

// ============================================================================
// ComGuiApplication Implementation
// ============================================================================

void ComGuiApplication::set_embedded_fallback_allowed(bool allowed) { g_embedded_fallback_allowed = allowed; }

bool ComGuiApplication::embedded_fallback_allowed() { return g_embedded_fallback_allowed; }

ComGuiApplication::ComGuiApplication(IDispatchPtr sap_gui_app, int com_uninit_count)
    : SapGuiObject(sap_gui_app), com_uninit_count_(com_uninit_count) {
    if (!sap_gui_app) {
        throw ComException("Invalid SAP GUI application pointer");
    }
    spdlog::debug("ComGuiApplication initialized");
}

ComGuiApplication::ComGuiApplication(IDispatchPtr sap_gui_app, bool owns_com_apartment)
    : ComGuiApplication(sap_gui_app, owns_com_apartment ? 1 : 0) {}

ComGuiApplication::~ComGuiApplication() {
    // Release COM interfaces before tearing down this thread's apartment.
    dispatch_ = nullptr;
    while (com_uninit_count_ > 0) {
        CoUninitialize();
        --com_uninit_count_;
    }
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

/// The embedded fallback: a new SapGui.ScriptingCtrl.1 instance in this process and its GuiApplication (or the
/// control itself when it implements OpenConnection); null when that fails, with `hr` the last HRESULT.
ComGuiApplicationPtr create_embedded_application(HRESULT& hr) {
    IDispatch* sap_gui = nullptr;
    CLSID clsid;
    hr = CLSIDFromProgID(L"SapGui.ScriptingCtrl.1", &clsid);

    if (SUCCEEDED(hr)) {
        spdlog::debug("CLSID found for SapGui.ScriptingCtrl.1, attempting CoCreateInstance...");
        // Try CLSCTX_ALL to allow in-process, local, and remote servers
        hr = CoCreateInstance(clsid, nullptr, CLSCTX_ALL, IID_IDispatch,
                             (void**)&sap_gui);

        if (SUCCEEDED(hr) && sap_gui) {
            spdlog::info("Created SAP GUI COM object via CoCreateInstance (SapGui.ScriptingCtrl.1)");

            // SapGui.ScriptingCtrl.1 provides GetScriptingEngine() which returns GuiApplication
            DISPID ge_id = -1;
            HRESULT ge_hr = get_dispid_via_typeinfo(sap_gui, L"GetScriptingEngine", &ge_id);
            if (FAILED(ge_hr)) {
                _bstr_t ge_name(L"GetScriptingEngine");
                ge_hr = sap_gui->GetIDsOfNames(IID_NULL, (LPOLESTR*)&ge_name, 1, LOCALE_USER_DEFAULT, &ge_id);
            }

            if (SUCCEEDED(ge_hr)) {
                _variant_t vr;
                HRESULT hr_meth = invoke(sap_gui, ge_id, DISPATCH_METHOD | DISPATCH_PROPERTYGET, {}, &vr);
                if (SUCCEEDED(hr_meth) && vr.vt == VT_DISPATCH && vr.pdispVal) {
                    spdlog::info("Got GuiApplication via SapGui.ScriptingCtrl.1 GetScriptingEngine");
                    sap_gui->Release();
                    return std::make_shared<ComGuiApplication>(IDispatchPtr(vr.pdispVal), 2);
                }
            }

            // If GetScriptingEngine wasn't available, check if sap_gui itself implements OpenConnection
            DISPID test_dispid = -1;
            HRESULT test_hr = get_dispid_via_typeinfo(sap_gui, L"OpenConnection", &test_dispid);
            if (SUCCEEDED(test_hr)) {
                return std::make_shared<ComGuiApplication>(IDispatchPtr(sap_gui), 2);
            }

            sap_gui->Release();
            sap_gui = nullptr;
            // SapGui.ScriptingCtrl.1 (sapfewse.ocx) incremented COM apartment count during
            // CoCreateInstance. Balance it here since creation did not produce an application.
            CoUninitialize();
        }

        spdlog::debug("CoCreateInstance failed (hr={})", hresult_hex(hr));
    }
    return nullptr;
}

}  // namespace

ComGuiApplicationPtr ComGuiApplication::create() {
    // Initialize COM library for STA (Single-Threaded Apartment) mode
    // This is required for SAP GUI scripting API
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != S_FALSE) {  // S_FALSE means already initialized
        throw ComInitializationException("Failed to initialize COM library", hr);
    }

    spdlog::debug("COM library initialized");

    try {
        // Method 1: Try SapROTWr.SapROTWrapper (Official SAP GUI Scripting entrypoint)
        IDispatch* sap_gui = sapgui_rot_entry();

        // Fallback to CoGetObject("SAPGUI")
        if (!sap_gui) {
            spdlog::debug("Attempting to get SAPGUI ROT entry via CoGetObject...");
            hr = CoGetObject(L"SAPGUI", nullptr, IID_IDispatch, (void**)&sap_gui);
        }

        const bool sap_logon_found = sap_gui != nullptr;
        if (sap_gui) {
            spdlog::debug("SAPGUI ROT entry obtained successfully - SAP GUI is running");
            IDispatchPtr engine = scripting_engine_of(sap_gui);
            sap_gui->Release();
            if (engine) return std::make_shared<ComGuiApplication>(engine, true);
        }

        if (!embedded_fallback_allowed()) {
            // The embedded control would host the SAP GUI windows inside this process, where the owner-mode
            // session workers (which attach through the running object table) can never reach them.
            if (sap_logon_found)
                throw ComException("SAP Logon is running, but its scripting engine is not available (is scripting "
                                   "enabled in the SAP GUI options?); this server does not fall back to an embedded "
                                   "SAP GUI.", hr);
            throw SapLogonNotRunningException(
                "SAP Logon (saplogon.exe) is not running on this desktop, and this server does not fall back to an "
                "embedded SAP GUI. Start SAP Logon in the interactive session, then retry.", hr);
        }

        spdlog::debug("Method 1 (GetObject + GetScriptingEngine) failed, trying CreateObject as fallback");

        // Method 2: Try CreateObject as fallback
        // This creates a new SAP GUI instance rather than connecting to running one
        // Only use this if Method 1 fails
        if (auto embedded = create_embedded_application(hr)) return embedded;

        // All methods failed
        std::string error_msg = fmt::format(
            "SAP GUI Scripting API not available (final hr={}). "
            "Possible causes: (1) SAP GUI not running, (2) SAP GUI scripting not enabled in SAP GUI options, "
            "(3) COM registration issue. Ensure SAP GUI is running and has an active transaction window open.",
            hresult_hex(hr));
        throw ComException(error_msg, hr);

    } catch (const ComException&) {
        CoUninitialize();
        throw;
    } catch (const std::exception& e) {
        CoUninitialize();
        throw ComException(std::string("Failed to create ComGuiApplication: ") + e.what());
    }
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

ComGuiConnectionPtr ComGuiApplication::open_connection(const std::string& connection_name, bool sync, bool raise_error) {
    if (!dispatch_) {
        if (raise_error) throw ComException("GuiApplication dispatch is null");
        return nullptr;
    }

    DISPID dispid = -1;
    HRESULT hr = get_dispid_via_typeinfo(dispatch_, L"OpenConnection", &dispid);
    if (FAILED(hr)) {
        _bstr_t name(L"OpenConnection");
        hr = dispatch_->GetIDsOfNames(IID_NULL, (LPOLESTR*)&name, 1, LOCALE_USER_DEFAULT, &dispid);
    }

    if (FAILED(hr)) {
        spdlog::error("OpenConnection method DISPID not found (hr={})", hresult_hex(hr));
        if (raise_error) throw ComException("OpenConnection method not found", hr);
        return nullptr;
    }

    // (Description, Sync, Raise)
    _variant_t result;
    EXCEPINFO excepinfo;
    memset(&excepinfo, 0, sizeof(excepinfo));
    UINT argerr = 0;
    hr = invoke(dispatch_, dispid, DISPATCH_METHOD, {bstr_arg(connection_name), _variant_t(sync), _variant_t(raise_error)},
                &result, &excepinfo, &argerr);

    if (FAILED(hr)) {
        std::string err_detail = "";
        if (excepinfo.bstrDescription) {
            err_detail = com::bstr_to_utf8(excepinfo.bstrDescription);
            SysFreeString(excepinfo.bstrDescription);
        }
        if (excepinfo.bstrSource) SysFreeString(excepinfo.bstrSource);
        if (excepinfo.bstrHelpFile) SysFreeString(excepinfo.bstrHelpFile);

        spdlog::warn("OpenConnection('{}') failed: hr={} {}", connection_name, hresult_hex(hr), err_detail);
        if (raise_error) {
            throw ComException(fmt::format("OpenConnection failed: {}", err_detail.empty() ? fmt::format("hr={}", hresult_hex(hr)) : err_detail), hr);
        }
        return nullptr;
    }

    if (result.vt == VT_DISPATCH && result.pdispVal) {
        spdlog::info("OpenConnection('{}') succeeded", connection_name);
        return std::make_shared<ComGuiConnection>(IDispatchPtr(result.pdispVal, true));
    }

    spdlog::warn("OpenConnection('{}') returned unexpected result type: {}", connection_name, result.vt);
    return nullptr;
}

ComGuiConnectionPtr ComGuiApplication::open_connection_by_connection_string(const std::string& connection_string, bool sync, bool raise_error) {
    if (!dispatch_) {
        if (raise_error) throw ComException("GuiApplication dispatch is null");
        return nullptr;
    }

    DISPID dispid = -1;
    HRESULT hr = get_dispid_via_typeinfo(dispatch_, L"OpenConnectionByConnectionString", &dispid);
    if (FAILED(hr)) {
        _bstr_t name(L"OpenConnectionByConnectionString");
        hr = dispatch_->GetIDsOfNames(IID_NULL, (LPOLESTR*)&name, 1, LOCALE_USER_DEFAULT, &dispid);
    }

    if (FAILED(hr)) {
        spdlog::error("OpenConnectionByConnectionString method DISPID not found (hr={})", hresult_hex(hr));
        if (raise_error) throw ComException("OpenConnectionByConnectionString method not found", hr);
        return nullptr;
    }

    // (ConnectString, Sync, Raise)
    _variant_t result;
    EXCEPINFO excepinfo;
    memset(&excepinfo, 0, sizeof(excepinfo));
    UINT argerr = 0;
    hr = invoke(dispatch_, dispid, DISPATCH_METHOD, {bstr_arg(connection_string), _variant_t(sync), _variant_t(raise_error)},
                &result, &excepinfo, &argerr);

    if (FAILED(hr)) {
        std::string err_detail = "";
        if (excepinfo.bstrDescription) {
            err_detail = com::bstr_to_utf8(excepinfo.bstrDescription);
            SysFreeString(excepinfo.bstrDescription);
        }
        if (excepinfo.bstrSource) SysFreeString(excepinfo.bstrSource);
        if (excepinfo.bstrHelpFile) SysFreeString(excepinfo.bstrHelpFile);

        spdlog::warn("OpenConnectionByConnectionString('{}') failed: hr={} {}", connection_string, hresult_hex(hr), err_detail);
        if (raise_error) {
            throw ComException(fmt::format("OpenConnectionByConnectionString failed: {}", err_detail.empty() ? fmt::format("hr={}", hresult_hex(hr)) : err_detail), hr);
        }
        return nullptr;
    }

    if (result.vt == VT_DISPATCH && result.pdispVal) {
        spdlog::info("OpenConnectionByConnectionString('{}') succeeded", connection_string);
        return std::make_shared<ComGuiConnection>(IDispatchPtr(result.pdispVal, true));
    }

    spdlog::warn("OpenConnectionByConnectionString('{}') returned unexpected result type: {}", connection_string, result.vt);
    return nullptr;
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

