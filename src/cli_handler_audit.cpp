#include "include/cli_handler.h"
#include "include/com_automation_engine.h"
#include "include/connection_launcher.h"
#include "include/session_facts.h"

namespace fairyfly::cli {

audit::SapFacts CommandHandler::audit_facts() const noexcept {
    audit::SapFacts facts;
    try {
        auto* engine = dynamic_cast<sap::ComAutomationEngine*>(engine_.get());
        if (!engine) return facts;
        auto session = engine->get_session();
        if (!session) return facts;
        // The audit record of every call reads these: never wait long for a busy session (2 s), unknown facts are fine.
        facts = sap::read_facts_bounded(
            [&] { return session->is_busy(); },
            [&] {
                audit::SapFacts read;
                read.system = session->get_system_name();
                read.client = session->get_client();
                read.user = session->get_user();
                read.transaction = session->get_transaction_code();
                read.program = session->get_program();
                read.screen_number = session->get_screen_number();
                return read;
            },
            sap::FactsBudget{std::chrono::milliseconds(2000), std::chrono::milliseconds(100)});
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
        audit::SapFacts facts = engine->peek_session_facts(resolved.value.session_id, resolved.value.server_session_key);
        if (!facts.any()) return facts;
        facts.connection_id = resolved.value.id;
        if (!resolved.value.session_id.empty())
            facts.session_identity = resolved.value.session_id + "|" + resolved.value.server_session_key + "|" +
                                     resolved.value.cache_generation;
        return facts;
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
        if (!engine || !conn_mgr_) { info.ambiguous = true; return info; }
        if (!logon_name.empty()) {
            info.connection_name = logon_name;
            bool have = false;
            for (const auto& saved : conn_mgr_->list_connections()) {
                if (!sap::ConnectionLauncher::matches_connection_name(logon_name, saved.connection_description)) continue;
                const audit::SapFacts facts = engine->peek_session_facts(saved.session_id, saved.server_session_key);
                if (facts.system.empty()) continue;
                if (have && (facts.system != info.facts.system || facts.client != info.facts.client)) {
                    info.facts = {};  // one entry, different systems: do not guess
                    info.ambiguous = true;
                    return info;
                }
                info.facts = facts;
                have = true;
            }
            return info;
        }
        if (!session_id.empty()) {
            // The name comes from the LIVE session's own connection (SAP GUI scripting), never from a saved record:
            // a saved record keyed by a reused session id could name another connection. Not determinable = empty.
            info.facts = engine->peek_session_facts(session_id);
            info.connection_name = engine->peek_session_connection_description(session_id);
            return info;
        }
        const auto resolved = conn_mgr_->resolve_connection(connection);
        if (resolved.status != ResultT<Connection>::Status::Success) return info;
        info.facts = engine->peek_session_facts(resolved.value.session_id, resolved.value.server_session_key);
        // The saved record must match the live session (session id AND server session key), otherwise a reused id could
        // let a stale record vouch for another connection: not validated = no name (the caller denies, fail closed).
        // The name is then the live connection's description only; when that is empty the name stays empty (the caller
        // denies with CONNECTION_DENIED) and the saved record's description is never used as a fallback.
        if (!engine->validate_session(resolved.value.session_id, resolved.value.server_session_key)) return info;
        info.connection_name = engine->peek_session_connection_description(resolved.value.session_id);
    } catch (...) {
        info.ambiguous = true;  // a launch is denied when the open sessions of the entry could not be inspected
    }
    return info;
}

} // namespace fairyfly::cli
