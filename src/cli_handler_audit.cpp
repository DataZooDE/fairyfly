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

} // namespace fairyfly::cli
