#pragma once

#include <string>
#include <memory>
#include <vector>
#include <map>
#include <chrono>
#include <nlohmann/json.hpp>

namespace fairyfly {

using json = nlohmann::json;

// Core result type for all operations
struct Result {
    enum class Status {
        Success,
        Error,
        NotImplemented
    };

    Status status = Status::NotImplemented;
    json data;
    json error;
    json diagnostics;  // Detailed trace for autonomous debugging
    std::chrono::milliseconds duration{0};

    json to_json() const {
        json response;
        switch (status) {
            case Status::Success:
                response["status"] = "success";
                response["data"] = data;
                break;
            case Status::Error:
                response["status"] = "error";
                response["error"] = error;
                break;
            case Status::NotImplemented:
                response["status"] = "error";
                response["error"]["code"] = "NOT_IMPLEMENTED";
                response["error"]["message"] = "Command not yet implemented";
                response["error"]["suggestions"] = json::array({
                    "This is Phase 1 scaffolding - SAP COM integration coming next"
                });
                break;
        }
        response["metadata"]["duration_ms"] = duration.count();

        // Include diagnostics if available (for autonomous debugging)
        if (!diagnostics.empty()) {
            response["diagnostics"] = diagnostics;
        }

        return response;
    }
};

/// Template version of Result that carries a value
template<typename T>
struct ResultT {
    enum class Status {
        Success,
        Error,
        NotImplemented
    };

    Status status = Status::NotImplemented;
    T value;
    json error;
    json diagnostics;
    std::chrono::milliseconds duration{0};
};

/// Helper to create non-template Result from template ResultT<T> error
template<typename T>
inline Result result_from_error(const ResultT<T>& error_result) {
    Result r;
    if (error_result.status == ResultT<T>::Status::Success) {
        r.status = Result::Status::Success;
    } else if (error_result.status == ResultT<T>::Status::Error) {
        r.status = Result::Status::Error;
    } else {
        r.status = Result::Status::NotImplemented;
    }
    r.error = error_result.error;
    r.diagnostics = error_result.diagnostics;
    r.duration = error_result.duration;
    return r;
}

// Window identifier
// Supported formats:
//   - Explicit window: "wnd[0]" (main window), "wnd[1]" (first modal dialog), etc.
//   - Special selectors:
//     - "@active" - Resolves to current active window (may be modal dialog)
//     - "@main"   - Always resolves to wnd[0] (main window), ignores modal dialogs
struct WindowId {
    std::string id;  // e.g., "wnd[0]", "wnd[1]", "@active", or "@main"

    WindowId() = default;
    explicit WindowId(const std::string& wnd_id) : id(wnd_id) {}

    bool is_valid() const {
        return !id.empty() && (id[0] == 'w' || id[0] == '@' || id[0] == '/');
    }

    bool is_active_selector() const {
        return id == "@active";
    }

    bool is_main_selector() const {
        return id == "@main";
    }

    bool is_special_selector() const {
        return is_active_selector() || is_main_selector();
    }

    // Extract window index from wnd[N] format, returns -1 for special selectors or invalid
    int get_index() const {
        if (is_special_selector()) return -1;
        auto wnd_pos = id.find("wnd[");
        if (wnd_pos != std::string::npos) {
            auto start = wnd_pos + 4;
            auto end = id.find(']', start);
            if (end != std::string::npos) {
                try {
                    return std::stoi(id.substr(start, end - start));
                } catch (...) {
                    return -1;
                }
            }
        }
        return -1;
    }
};

inline bool window_ids_match(const WindowId& requested, const WindowId& active) {
    if (requested.id.rfind("wnd[", 0) != 0)
        return requested.id == active.id;
    const int requested_index = requested.get_index();
    return requested_index >= 0 && requested_index == active.get_index();
}

// Element identifier
// Supported formats:
//   - Explicit: "wnd[0]/usr/btn[3]" - Direct element path with window
//   - @active:  "@active/usr/btn[3]" - Resolved to current active window
//   - @main:    "@main/usr/btn[3]" - Always targets main window (wnd[0])
//   - Canonical: "/app/con[0]/ses[0]/wnd[0]/usr/btn[3]" - Full SAP GUI ID
//
// The @main selector is agent-friendly: it bypasses modal dialogs and always
// targets the main application window, preventing confusion when SAP opens
// error/warning dialogs.
struct ElementId {
    std::string path;  // e.g., "wnd[0]/usr/btn[3]", "@active/usr/btn[3]", or "@main/usr/btn[3]"

    ElementId() = default;
    explicit ElementId(const std::string& p) : path(p) {}

    bool is_valid() const {
        return !path.empty() && (path[0] == 'w' || path[0] == '@' || path[0] == '/');  // Must start with window, special selector, or /app
    }

    // Extract window ID from element path
    WindowId get_window() const {
        if (path.rfind("/app/", 0) == 0) {
            auto wnd_pos = path.find("/wnd[");
            if (wnd_pos != std::string::npos) {
                auto end = path.find(']', wnd_pos + 5);
                if (end != std::string::npos) return WindowId(path.substr(0, end + 1));
            }
        }
        auto slash_pos = path.find('/');
        if (slash_pos == std::string::npos) {
            return WindowId(path);  // Just window ID, no element path
        }
        return WindowId(path.substr(0, slash_pos));
    }

    // Get element path without window prefix (e.g., "usr/btn[3]" from "wnd[0]/usr/btn[3]")
    std::string get_element_path() const {
        if (path.rfind("/app/", 0) == 0) {
            auto wnd_pos = path.find("/wnd[");
            if (wnd_pos != std::string::npos) {
                auto end = path.find(']', wnd_pos + 5);
                if (end != std::string::npos)
                    return end + 1 < path.size() && path[end + 1] == '/' ? path.substr(end + 2) : "";
            }
        }
        auto slash_pos = path.find('/');
        if (slash_pos == std::string::npos) {
            return "";  // Just window ID, no element path
        }
        return path.substr(slash_pos + 1);
    }

    // Replace window part with actual window ID (resolves @active to wnd[N])
    ElementId with_window(const WindowId& window) const {
        auto elem_path = get_element_path();
        if (elem_path.empty()) {
            return ElementId(window.id);
        }
        return ElementId(window.id + "/" + elem_path);
    }
};

// Session identifier
struct SessionId {
    std::string id;

    SessionId() = default;
    explicit SessionId(const std::string& s) : id(s) {}

    bool is_valid() const {
        return !id.empty();
    }
};

// SAP GUI Window info
struct SAPGuiWindow {
    std::string title;           // Window title
    void* hwnd;                  // HWND cast to void* for cross-platform
    int connection_idx;          // Index in GuiApplication.Children
    int session_idx;             // Index in Connection.Children

    SAPGuiWindow() : hwnd(nullptr), connection_idx(-1), session_idx(-1) {}

    SAPGuiWindow(const std::string& t, void* h, int c, int s)
        : title(t), hwnd(h), connection_idx(c), session_idx(s) {}

    bool is_valid() const {
        return hwnd != nullptr && connection_idx >= 0 && session_idx >= 0;
    }

    json to_json() const {
        return json{
            {"title", title},
            {"connection_idx", connection_idx},
            {"session_idx", session_idx}
        };
    }
};

} // namespace fairyfly
