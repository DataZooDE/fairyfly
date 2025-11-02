#include "include/com/wrapper.h"
#include "include/com/wrapper_helpers.h"
#include <spdlog/spdlog.h>

namespace fairyfly {
namespace sap {

// ============================================================================
// ComGuiConnection Implementation
// ============================================================================

ComGuiConnection::ComGuiConnection(IDispatchPtr conn) : SapGuiObject(conn) {
    if (!conn) throw ComException("Invalid connection pointer");
}

ComGuiConnectionPtr ComGuiConnection::create(IDispatchPtr conn) {
    return std::make_shared<ComGuiConnection>(conn);
}

// get_id(), get_type(), get_type_as_number() now inherited from SapGuiObject

std::string ComGuiConnection::get_description() const {
    return get_string_property(L"Description");
}

std::string ComGuiConnection::get_connection_string() const {
    return get_string_property(L"ConnectionString");
}

int ComGuiConnection::get_session_count() const {
    // Check if scripting is disabled server-side
    try {
        bool disabled_by_server = get_bool_property(L"DisabledByServer");
        if (disabled_by_server) {
            spdlog::error("SAP GUI Scripting is disabled on the server");
            spdlog::error("Connection: {} ({})", get_description(), get_id());
            spdlog::error("Administrator must set: sapgui/user_scripting = TRUE");
            return 0;
        }
    } catch (const std::exception& e) {
        spdlog::debug("Could not check DisabledByServer property: {}", e.what());
    }

    // Get sessions collection (try Children first, then Sessions)
    auto sessions = get_dispatch_property(L"Children");
    if (!sessions) {
        spdlog::debug("GuiConnection.Children not available, trying Sessions");
        sessions = get_dispatch_property(L"Sessions");
    }

    if (!sessions) {
        spdlog::warn("GuiConnection has no Children/Sessions collection");
        return 0;
    }

    // Get count
    int count = ::fairyfly::sap::get_int_property(sessions, "Count");
    spdlog::debug("GuiConnection session count: {}", count);
    return count;
}

ComGuiSessionPtr ComGuiConnection::get_session(int index) const {
    auto sessions_col = sessions();
    return sessions_col.item(index);
}

SapGuiCollection<ComGuiSession> ComGuiConnection::sessions() const {
    auto sessions_dispatch = get_dispatch_property(L"Children");
    if (!sessions_dispatch) {
        spdlog::debug("GuiConnection.Children not available, trying Sessions");
        sessions_dispatch = get_dispatch_property(L"Sessions");
    }

    if (!sessions_dispatch) {
        spdlog::warn("GuiConnection has no Children/Sessions collection");
        return SapGuiCollection<ComGuiSession>(nullptr);
    }

    return SapGuiCollection<ComGuiSession>(sessions_dispatch);
}

} // namespace sap
} // namespace fairyfly

