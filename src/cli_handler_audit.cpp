#include "include/cli_handler.h"
#include "include/com_automation_engine.h"
#include "include/connection_launcher.h"

namespace fairyfly::cli {

audit::SapFacts CommandHandler::audit_facts() const noexcept {
    audit::SapFacts facts;
    try {
        auto* engine = dynamic_cast<sap::ComAutomationEngine*>(engine_.get());
        if (!engine) return facts;
        auto session = engine->get_session();
        if (!session) return facts;
        facts.system = session->get_system_name();
        facts.client = session->get_client();
        facts.user = session->get_user();
        facts.transaction = session->get_transaction_code();
    } catch (...) {
    }
    return facts;
}

audit::SapFacts CommandHandler::audit_facts_for_connection(std::optional<int> connection) const noexcept {
    try {
        auto* engine = dynamic_cast<sap::ComAutomationEngine*>(engine_.get());
        if (!engine || !conn_mgr_) return {};
        const auto resolved = conn_mgr_->resolve_connection(connection);
        if (resolved.status != ResultT<Connection>::Status::Success) return {};
        return engine->peek_session_facts(resolved.value.session_id, resolved.value.server_session_key);
    } catch (...) {
        return {};
    }
}

CommandHandler::SessionTargetInfo CommandHandler::peek_session_target(const std::string& logon_name,
                                                                     const std::string& session_id,
                                                                     std::optional<int> connection) const noexcept {
    SessionTargetInfo info;
    try {
        auto* engine = dynamic_cast<sap::ComAutomationEngine*>(engine_.get());
        if (!engine || !conn_mgr_) return info;
        if (!logon_name.empty()) {
            info.connection_name = logon_name;
            bool have = false;
            for (const auto& saved : conn_mgr_->list_connections()) {
                if (!sap::ConnectionLauncher::matches_connection_name(logon_name, saved.connection_description)) continue;
                const audit::SapFacts facts = engine->peek_session_facts(saved.session_id, saved.server_session_key);
                if (facts.system.empty()) continue;
                if (have && (facts.system != info.facts.system || facts.client != info.facts.client)) {
                    info.facts = {};  // one entry, different systems: do not guess
                    return info;
                }
                info.facts = facts;
                have = true;
            }
            return info;
        }
        if (!session_id.empty()) {
            info.facts = engine->peek_session_facts(session_id);
            if (const auto saved = conn_mgr_->find_by_session_id(session_id)) info.connection_name = saved->connection_description;
            if (info.connection_name.empty()) info.connection_name = engine->peek_session_connection_description(session_id);
            return info;
        }
        const auto resolved = conn_mgr_->resolve_connection(connection);
        if (resolved.status != ResultT<Connection>::Status::Success) return info;
        info.facts = engine->peek_session_facts(resolved.value.session_id, resolved.value.server_session_key);
        info.connection_name = resolved.value.connection_description;
        if (info.connection_name.empty())
            info.connection_name = engine->peek_session_connection_description(resolved.value.session_id);
    } catch (...) {
    }
    return info;
}

} // namespace fairyfly::cli
