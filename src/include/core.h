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
struct WindowId {
    std::string id;  // e.g., "wnd[0]", "wnd[1]", or "@active"

    WindowId() = default;
    explicit WindowId(const std::string& wnd_id) : id(wnd_id) {}

    bool is_valid() const {
        return !id.empty() && (id[0] == 'w' || id[0] == '@');
    }

    bool is_active_selector() const {
        return id == "@active";
    }

    // Extract window index from wnd[N] format, returns -1 for @active or invalid
    int get_index() const {
        if (is_active_selector()) return -1;
        if (id.size() < 6) return -1;  // Minimum: "wnd[0]"

        auto start = id.find('[');
        auto end = id.find(']');
        if (start == std::string::npos || end == std::string::npos) return -1;

        try {
            return std::stoi(id.substr(start + 1, end - start - 1));
        } catch (...) {
            return -1;
        }
    }
};

// Element identifier
struct ElementId {
    std::string path;  // e.g., "wnd[0]/usr/btn[3]" or "@active/usr/btn[3]"

    ElementId() = default;
    explicit ElementId(const std::string& p) : path(p) {}

    bool is_valid() const {
        return !path.empty() && (path[0] == 'w' || path[0] == '@');  // Must start with window or @active
    }

    // Extract window ID from element path
    WindowId get_window() const {
        auto slash_pos = path.find('/');
        if (slash_pos == std::string::npos) {
            return WindowId(path);  // Just window ID, no element path
        }
        return WindowId(path.substr(0, slash_pos));
    }

    // Get element path without window prefix (e.g., "usr/btn[3]" from "wnd[0]/usr/btn[3]")
    std::string get_element_path() const {
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
