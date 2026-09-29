#include "include/cli_handler.h"
#include "include/com_automation_engine.h"

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

} // namespace fairyfly::cli
