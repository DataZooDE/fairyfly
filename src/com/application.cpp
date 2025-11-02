#include "include/com/wrapper.h"
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

ComGuiApplication::ComGuiApplication(IDispatchPtr sap_gui_app) : SapGuiObject(sap_gui_app) {
    if (!sap_gui_app) {
        throw ComException("Invalid SAP GUI application pointer");
    }
    spdlog::debug("ComGuiApplication initialized");
}

ComGuiApplicationPtr ComGuiApplication::create() {
    // Initialize COM library for STA (Single-Threaded Apartment) mode
    // This is required for SAP GUI scripting API
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != S_FALSE) {  // S_FALSE means already initialized
        throw ComException("Failed to initialize COM library", hr);
    }

    spdlog::debug("COM library initialized");

    try {
        IDispatch* sap_gui = nullptr;

        // Method 1: Try to connect to running SAP GUI instance using GetObject (Preferred)
        // According to SAP GUI Scripting API documentation:
        // "Set rotEntry = GetObject("SAPGUI")"
        // "Set application = rotEntry.GetScriptingEngine"
        // We must call GetScriptingEngine property on the ROT entry!
        spdlog::debug("Attempting to get SAPGUI ROT entry...");
        hr = CoGetObject(L"SAPGUI", nullptr, IID_IDispatch, (void**)&sap_gui);

        if (SUCCEEDED(hr) && sap_gui) {
            spdlog::debug("CoGetObject('SAPGUI') succeeded - SAP Logon is running");

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
                    return std::make_shared<ComGuiApplication>(IDispatchPtr(vr.pdispVal));
                }

                // Try METHOD
                VariantClear(&vr);
                HRESULT hr_meth = sap_gui->Invoke(ge_id, IID_NULL, LOCALE_USER_DEFAULT,
                                                  DISPATCH_METHOD, &noargs, &vr, nullptr, nullptr);
                if (SUCCEEDED(hr_meth) && vr.vt == VT_DISPATCH && vr.pdispVal) {
                    spdlog::debug("Got GuiApplication via ROT GetScriptingEngine (METHOD)");
                    sap_gui->Release();
                    return std::make_shared<ComGuiApplication>(IDispatchPtr(vr.pdispVal));
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
                spdlog::info("Created SAP GUI COM object via CoCreateInstance");

                // CRITICAL FIX: SAP GUI uses interface hierarchies. We need to do the IUnknown roundtrip
                // to get the most derived IDispatch interface that exposes all methods.
                // See: https://stackoverflow.com/questions/25965753/idispatch-returns-disp-e-unknownname
                IUnknown* unknown = nullptr;
                HRESULT qi_hr = sap_gui->QueryInterface(IID_IUnknown, (void**)&unknown);
                spdlog::debug("QueryInterface for IID_IUnknown: hr=0x{:08X}", qi_hr);

                if (SUCCEEDED(qi_hr) && unknown) {
                    IDispatch* dispatch_derived = nullptr;
                    qi_hr = unknown->QueryInterface(IID_IDispatch, (void**)&dispatch_derived);
                    spdlog::debug("QueryInterface IUnknown->IDispatch (most derived): hr=0x{:08X}", qi_hr);

                    if (SUCCEEDED(qi_hr) && dispatch_derived) {
                        spdlog::info("Got most derived IDispatch interface via IUnknown roundtrip");
                        sap_gui->Release();
                        sap_gui = dispatch_derived;
                    }
                    unknown->Release();
                }

                // Try alternative approach: Use ITypeInfo instead of GetIDsOfNames directly
                // According to research, GetIDsOfNames may not work but ITypeInfo::GetIDsOfNames can
                ITypeInfo* type_info = nullptr;
                HRESULT ti_hr = sap_gui->GetTypeInfo(0, LOCALE_USER_DEFAULT, &type_info);
                spdlog::debug("GetTypeInfo: hr=0x{:08X}", ti_hr);

                DISPID test_dispid = -1;
                HRESULT test_hr = E_FAIL;

                if (SUCCEEDED(ti_hr) && type_info) {
                    // Try using ITypeInfo::GetIDsOfNames instead of IDispatch::GetIDsOfNames
                    LPOLESTR method_names[1] = { const_cast<LPOLESTR>(L"OpenConnection") };
                    test_hr = type_info->GetIDsOfNames(method_names, 1, &test_dispid);
                    spdlog::debug("ITypeInfo::GetIDsOfNames for 'OpenConnection': hr=0x{:08X}, dispid={}", test_hr, test_dispid);
                    type_info->Release();

                    if (SUCCEEDED(test_hr)) {
                        spdlog::debug("SUCCESS: OpenConnection found via ITypeInfo! GuiApplication is ready");
                        return std::make_shared<ComGuiApplication>(IDispatchPtr(sap_gui));
                    }
                }

                // Fallback: try IDispatch::GetIDsOfNames anyway
                if (FAILED(test_hr)) {
                    _bstr_t test_method("OpenConnection");
                    test_hr = sap_gui->GetIDsOfNames(IID_NULL, (LPOLESTR*)&test_method, 1, LOCALE_USER_DEFAULT, &test_dispid);
                    spdlog::debug("IDispatch::GetIDsOfNames for 'OpenConnection': hr=0x{:08X}, dispid={}", test_hr, test_dispid);

                    if (SUCCEEDED(test_hr)) {
                        spdlog::debug("SUCCESS: OpenConnection found! GuiApplication is ready");
                        return std::make_shared<ComGuiApplication>(IDispatchPtr(sap_gui));
                    }
                }

                spdlog::error("FAILED: OpenConnection not found via ITypeInfo OR IDispatch");
                spdlog::error("This indicates sapfewse.ocx may not be properly registered");
                spdlog::error("Try: regsvr32 \"C:\\Program Files (x86)\\SAP\\FrontEnd\\SAPgui\\sapfewse.ocx\"");
                spdlog::error("Or check: SAP GUI Options → Accessibility & Scripting → Enable scripting");
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
        spdlog::warn("Connections collection exists but reports 0 items - no SAP connections currently open");
        spdlog::warn("To see connections: Open SAP Logon, create a connection, or open a transaction");
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

// ============================================================================
// Mouse Hook Implementation for Window Selection
// ============================================================================

// Global state for mouse hook
static HWND g_clicked_hwnd = nullptr;
static HANDLE g_click_event = nullptr;
static HWND g_last_hwnd = nullptr;
static HCURSOR g_crosshair_cursor = nullptr;

// Debug logging function for hook callback (console won't work in hook context)
static void hook_debug(const char* msg) {
    static FILE* debug_file = nullptr;
    if (!debug_file) {
        fopen_s(&debug_file, "hook_debug.log", "a");
    }
    if (debug_file) {
        fprintf(debug_file, "%s\n", msg);
        fflush(debug_file);
    }
}

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
    char debug_msg[256];
    
    if (nCode >= 0) {
        MSLLHOOKSTRUCT* mouse_data = (MSLLHOOKSTRUCT*)lParam;
        POINT pt = mouse_data->pt;
        HWND current_hwnd = WindowFromPoint(pt);
        
        if (wParam == WM_MOUSEMOVE) {
            sprintf_s(debug_msg, "MouseMove: hwnd=0x%p", reinterpret_cast<void*>(current_hwnd));
            hook_debug(debug_msg);
            
            // Change cursor to crosshair on mouse move
            if (g_crosshair_cursor) {
                SetCursor(g_crosshair_cursor);
            }
            
            // Only update console if window changed
            if (current_hwnd != g_last_hwnd) {
                g_last_hwnd = current_hwnd;
                
                // Check if this is a SAP window
                bool is_sap = is_sap_gui_window(current_hwnd);
                
                sprintf_s(debug_msg, "Window changed: is_sap=%d", is_sap ? 1 : 0);
                hook_debug(debug_msg);
                
                // Print status to console
                std::string status = is_sap ? "[SAP WINDOW DETECTED]" : "[Not a SAP window]";
                std::cout << "\r                                                                 ";  // Clear line
                std::cout << "\r" << status << std::flush;
            }
        }
        else if (wParam == WM_LBUTTONDOWN) {
            sprintf_s(debug_msg, "MouseClick detected!");
            hook_debug(debug_msg);
            
            if (g_click_event) {
                g_clicked_hwnd = current_hwnd;
                hook_debug("Setting click event...");
                std::cout << "\r[CLICK DETECTED!]" << std::endl;
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
    hook_debug("Mouse hook installed successfully");
    spdlog::debug("Mouse hook installed");

    // Show prompt to user
    spdlog::info("Click on SAP GUI window within {} seconds...", timeout_seconds);
    std::cout << "\n>>> Click on SAP GUI window within " << timeout_seconds << " seconds..." << std::endl;
    std::cout << ">>> Hover over SAP windows to see detection status" << std::endl;

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
    
    // Clear the status line
    std::cout << "\r                                 " << std::endl;

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

    // Since SAP COM doesn't expose native HWND directly, we match by window title
    // This is the best we can do without direct HWND access from SAP COM
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
                    spdlog::info("Session {}/{} active window title: '{}'", c, s, active_title);
                    spdlog::info("Clicked window title: '{}'", title);
                    spdlog::info("Titles match or using best match: conn={}, sess={}", c, s);
                    
                    // Return the first matching session
                    return SAPGuiWindow(active_title, target_hwnd, c, s);
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

