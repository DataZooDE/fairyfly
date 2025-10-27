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

// Element identifier
struct ElementId {
    std::string path;  // e.g., "wnd[0]/usr/btn[3]"

    ElementId() = default;
    explicit ElementId(const std::string& p) : path(p) {}

    bool is_valid() const {
        return !path.empty() && path[0] == 'w';  // Must start with window
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
