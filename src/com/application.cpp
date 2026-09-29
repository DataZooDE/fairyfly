#include "include/com/wrapper.h"
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

ComGuiApplicationPtr ComGuiApplication::create() {
    // Initialize COM library for STA (Single-Threaded Apartment) mode
    // This is required for SAP GUI scripting API
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != S_FALSE) {  // S_FALSE means already initialized
        throw ComInitializationException("Failed to initialize COM library", hr);
    }

    spdlog::debug("COM library initialized");

    try {
        IDispatch* sap_gui = nullptr;

        // Method 1: Try SapROTWr.SapROTWrapper (Official SAP GUI Scripting entrypoint)
        CLSID clsid_rot;
        HRESULT rot_hr = CLSIDFromProgID(L"SapROTWr.SapROTWrapper", &clsid_rot);
        spdlog::debug("SapROTWr CLSID lookup hr=0x{:08X}", static_cast<unsigned int>(rot_hr));
        if (SUCCEEDED(rot_hr)) {
            IDispatch* rot_wrapper = nullptr;
            rot_hr = CoCreateInstance(clsid_rot, nullptr, CLSCTX_INPROC_SERVER, IID_IDispatch, (void**)&rot_wrapper);
            spdlog::debug("SapROTWr creation hr=0x{:08X}", static_cast<unsigned int>(rot_hr));
            if (SUCCEEDED(rot_hr)) {
                OLECHAR* rot_entry_name = (OLECHAR*)L"GetROTEntry";
                DISPID dispid_rot;
                rot_hr = rot_wrapper->GetIDsOfNames(IID_NULL, &rot_entry_name, 1, LOCALE_USER_DEFAULT, &dispid_rot);
                spdlog::debug("SapROTWr GetROTEntry lookup hr=0x{:08X}", static_cast<unsigned int>(rot_hr));
                if (SUCCEEDED(rot_hr)) {
                    VARIANT arg;
                    VariantInit(&arg);
                    arg.vt = VT_BSTR;
                    arg.bstrVal = SysAllocString(L"SAPGUI");
                    DISPPARAMS params{&arg, nullptr, 1, 0};
                    VARIANT res;
                    VariantInit(&res);
                    HRESULT hr_call = rot_wrapper->Invoke(dispid_rot, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD, &params, &res, nullptr, nullptr);
                    spdlog::debug("SapROTWr GetROTEntry invoke hr=0x{:08X}, result type={}",
                                  static_cast<unsigned int>(hr_call), res.vt);
                    VariantClear(&arg);
                    if (SUCCEEDED(hr_call) && res.vt == VT_DISPATCH && res.pdispVal) {
                        sap_gui = res.pdispVal;
                        spdlog::debug("SapROTWr.SapROTWrapper returned SAPGUI ROT entry successfully");
                    }
                    if (res.vt != VT_DISPATCH || !res.pdispVal) VariantClear(&res);
                }
                rot_wrapper->Release();
            }
        }

        // Fallback to CoGetObject("SAPGUI")
        if (!sap_gui) {
            spdlog::debug("Attempting to get SAPGUI ROT entry via CoGetObject...");
            hr = CoGetObject(L"SAPGUI", nullptr, IID_IDispatch, (void**)&sap_gui);
        }

        if (sap_gui) {
            spdlog::debug("SAPGUI ROT entry obtained successfully - SAP GUI is running");

            // CoGetObject("SAPGUI") returns SapGuiAuto (ROT entry). It exposes GetScriptingEngine.
            // Some builds expose it as a PROPERTYGET, others as a METHOD. Try both, PROPERTYGET first.
            _bstr_t ge_name(L"GetScriptingEngine");
            DISPID ge_id{};
            HRESULT ge_hr = get_dispid_via_typeinfo(sap_gui, ge_name, &ge_id);

            if (SUCCEEDED(ge_hr)) {
                DISPPARAMS noargs{nullptr, nullptr, 0, 0};
                _variant_t vr;

                // Try PROPERTYGET
                HRESULT hr_prop = sap_gui->Invoke(ge_id, IID_NULL, LOCALE_USER_DEFAULT,
                                                  DISPATCH_PROPERTYGET, &noargs, &vr, nullptr, nullptr);
                if (SUCCEEDED(hr_prop) && vr.vt == VT_DISPATCH && vr.pdispVal) {
                    spdlog::debug("Got GuiApplication via ROT GetScriptingEngine (PROPERTYGET)");
                    sap_gui->Release();
                    return std::make_shared<ComGuiApplication>(IDispatchPtr(vr.pdispVal), true);
                }

                // Try METHOD
                VariantClear(&vr);
                HRESULT hr_meth = sap_gui->Invoke(ge_id, IID_NULL, LOCALE_USER_DEFAULT,
                                                  DISPATCH_METHOD, &noargs, &vr, nullptr, nullptr);
                if (SUCCEEDED(hr_meth) && vr.vt == VT_DISPATCH && vr.pdispVal) {
                    spdlog::debug("Got GuiApplication via ROT GetScriptingEngine (METHOD)");
                    sap_gui->Release();
                    return std::make_shared<ComGuiApplication>(IDispatchPtr(vr.pdispVal), true);
                }

                spdlog::debug("GetScriptingEngine failed: prop_hr=0x{:08X}, meth_hr=0x{:08X}", hr_prop, hr_meth);
            } else {
                spdlog::debug("GetScriptingEngine name resolution failed (hr=0x{:08X})", ge_hr);
            }

            sap_gui->Release();
            sap_gui = nullptr;
        }

        spdlog::debug("Method 1 (GetObject + GetScriptingEngine) failed, trying CreateObject as fallback");

        // Method 2: Try CreateObject as fallback
        // This creates a new SAP GUI instance rather than connecting to running one
        // Only use this if Method 1 fails
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
                    DISPPARAMS noargs = {nullptr, nullptr, 0, 0};
                    _variant_t vr;
                    HRESULT hr_meth = sap_gui->Invoke(ge_id, IID_NULL, LOCALE_USER_DEFAULT,
                                                      DISPATCH_METHOD | DISPATCH_PROPERTYGET, &noargs, &vr, nullptr, nullptr);
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

            spdlog::debug("CoCreateInstance failed (hr=0x{:08X})", hr);
        }

        // All methods failed
        std::string error_msg = fmt::format(
            "SAP GUI Scripting API not available (final hr=0x{:08X}). "
            "Possible causes: (1) SAP GUI not running, (2) SAP GUI scripting not enabled in SAP GUI options, "
            "(3) COM registration issue. Ensure SAP GUI is running and has an active transaction window open.",
            hr
        );
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
        spdlog::error("OpenConnection method DISPID not found (hr=0x{:08X})", hr);
        if (raise_error) throw ComException("OpenConnection method not found", hr);
        return nullptr;
    }

    // Arguments in reverse order for IDispatch::Invoke:
    // [0] = Raise (VARIANT_BOOL)
    // [1] = Sync (VARIANT_BOOL)
    // [2] = Description (BSTR)
    VARIANT args[3];
    VariantInit(&args[0]);
    VariantInit(&args[1]);
    VariantInit(&args[2]);

    args[0].vt = VT_BOOL;
    args[0].boolVal = raise_error ? VARIANT_TRUE : VARIANT_FALSE;

    args[1].vt = VT_BOOL;
    args[1].boolVal = sync ? VARIANT_TRUE : VARIANT_FALSE;

    args[2].vt = VT_BSTR;
    const auto wide_name = com::utf8_to_wide(connection_name);
    args[2].bstrVal = SysAllocStringLen(wide_name.data(), static_cast<UINT>(wide_name.size()));

    DISPPARAMS params;
    params.rgvarg = args;
    params.rgdispidNamedArgs = nullptr;
    params.cArgs = 3;
    params.cNamedArgs = 0;

    _variant_t result;
    EXCEPINFO excepinfo;
    memset(&excepinfo, 0, sizeof(excepinfo));
    UINT argerr = 0;

    hr = dispatch_->Invoke(
        dispid,
        IID_NULL,
        LOCALE_USER_DEFAULT,
        DISPATCH_METHOD,
        &params,
        &result,
        &excepinfo,
        &argerr
    );

    VariantClear(&args[2]);

    if (FAILED(hr)) {
        std::string err_detail = "";
        if (excepinfo.bstrDescription) {
            err_detail = com::bstr_to_utf8(excepinfo.bstrDescription);
            SysFreeString(excepinfo.bstrDescription);
        }
        if (excepinfo.bstrSource) SysFreeString(excepinfo.bstrSource);
        if (excepinfo.bstrHelpFile) SysFreeString(excepinfo.bstrHelpFile);

        spdlog::warn("OpenConnection('{}') failed: hr=0x{:08X} {}", connection_name, hr, err_detail);
        if (raise_error) {
            throw ComException(fmt::format("OpenConnection failed: {}", err_detail.empty() ? fmt::format("hr=0x{:08X}", hr) : err_detail), hr);
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
        spdlog::error("OpenConnectionByConnectionString method DISPID not found (hr=0x{:08X})", hr);
        if (raise_error) throw ComException("OpenConnectionByConnectionString method not found", hr);
        return nullptr;
    }

    // Arguments in reverse order for IDispatch::Invoke:
    // [0] = Raise (VARIANT_BOOL)
    // [1] = Sync (VARIANT_BOOL)
    // [2] = ConnectString (BSTR)
    VARIANT args[3];
    VariantInit(&args[0]);
    VariantInit(&args[1]);
    VariantInit(&args[2]);

    args[0].vt = VT_BOOL;
    args[0].boolVal = raise_error ? VARIANT_TRUE : VARIANT_FALSE;

    args[1].vt = VT_BOOL;
    args[1].boolVal = sync ? VARIANT_TRUE : VARIANT_FALSE;

    args[2].vt = VT_BSTR;
    const auto wide_connection = com::utf8_to_wide(connection_string);
    args[2].bstrVal = SysAllocStringLen(wide_connection.data(), static_cast<UINT>(wide_connection.size()));

    DISPPARAMS params;
    params.rgvarg = args;
    params.rgdispidNamedArgs = nullptr;
    params.cArgs = 3;
    params.cNamedArgs = 0;

    _variant_t result;
    EXCEPINFO excepinfo;
    memset(&excepinfo, 0, sizeof(excepinfo));
    UINT argerr = 0;

    hr = dispatch_->Invoke(
        dispid,
        IID_NULL,
        LOCALE_USER_DEFAULT,
        DISPATCH_METHOD,
        &params,
        &result,
        &excepinfo,
        &argerr
    );

    VariantClear(&args[2]);

    if (FAILED(hr)) {
        std::string err_detail = "";
        if (excepinfo.bstrDescription) {
            err_detail = com::bstr_to_utf8(excepinfo.bstrDescription);
            SysFreeString(excepinfo.bstrDescription);
        }
        if (excepinfo.bstrSource) SysFreeString(excepinfo.bstrSource);
        if (excepinfo.bstrHelpFile) SysFreeString(excepinfo.bstrHelpFile);

        spdlog::warn("OpenConnectionByConnectionString('{}') failed: hr=0x{:08X} {}", connection_string, hr, err_detail);
        if (raise_error) {
            throw ComException(fmt::format("OpenConnectionByConnectionString failed: {}", err_detail.empty() ? fmt::format("hr=0x{:08X}", hr) : err_detail), hr);
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

    // Convert wide string to regular string for logging
    std::wstring wclass(class_name);
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wclass.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string aclass(size_needed - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wclass.c_str(), -1, &aclass[0], size_needed, nullptr, nullptr);

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
    
    std::wstring wtitle(window_title);
    std::wstring wclass(class_name);
    
    int title_size = WideCharToMultiByte(CP_UTF8, 0, wtitle.c_str(), -1, nullptr, 0, nullptr, nullptr);
    int class_size = WideCharToMultiByte(CP_UTF8, 0, wclass.c_str(), -1, nullptr, 0, nullptr, nullptr);
    
    std::string title(title_size - 1, 0);
    std::string aclass(class_size - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wtitle.c_str(), -1, &title[0], title_size, nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, wclass.c_str(), -1, &aclass[0], class_size, nullptr, nullptr);
    
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

