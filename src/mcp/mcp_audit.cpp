#include "include/mcp/mcp_audit.h"

#include <atomic>
#include <exception>

#include <spdlog/spdlog.h>

namespace fairyfly::mcp {

namespace {
std::atomic<bool> g_failed{false};

void record_result(bool ok) noexcept {
    if (!ok) g_failed.store(true);
}
} // namespace

bool mcp_audit_failed() noexcept { return g_failed.load(); }
void mcp_audit_reset_failure() noexcept { g_failed.store(false); }

AuditHook make_mcp_audit_hook(audit::AuditSink* sink,
                              const std::function<cli::CommandHandler*()>& peek_handler,
                              bool read_only) {
    if (!sink || !sink->enabled()) return [](const McpCallRecord&) {};
    (void)read_only;  // McpCallRecord::read_only is authoritative per call
    return [sink, peek_handler](const McpCallRecord& rec) {
        try {
            audit::AuditRecord out;
            out.ts = std::chrono::system_clock::now();
            out.source = "mcp";
            out.tool = rec.tool;
            out.command = rec.command;
            out.argv = audit::redact_argv(rec.argv);
            out.connection = rec.connection;
            if (peek_handler) {
                if (cli::CommandHandler* handler = peek_handler()) {
                    audit::SapFacts facts = handler->audit_facts();
                    if (facts.any()) out.sap = std::move(facts);
                }
            }
            out.read_only = rec.read_only;
            out.status = rec.status;
            out.error_code = rec.error_code;
            out.exit_code = rec.status == "success" ? 0 : 1;
            out.duration_ms = rec.duration_ms;
            out.client = rec.client;
            out.request_id = rec.request_id;
            out.principal = rec.principal;
            out.remote_addr = rec.remote_addr;
            out.transport = rec.transport;
            out.era = rec.era;
            out.tcode_left_allowlist = rec.tcode_left_allowlist;
            out.facts_pre_ms = rec.facts_pre_ms;
            out.invoke_ms = rec.invoke_ms;
            out.facts_post_ms = rec.facts_post_ms;
            record_result(sink->append(out));
        } catch (const std::exception& e) {
            record_result(false);
            try { spdlog::warn("MCP audit hook failed: {}", e.what()); } catch (...) {}
        } catch (...) {
            record_result(false);
        }
    };
}

bool append_serve_event(audit::AuditSink* sink, const std::string& status, bool read_only) noexcept {
    if (!sink || !sink->enabled()) return true;
    try {
        audit::AuditRecord out;
        out.ts = std::chrono::system_clock::now();
        out.source = "mcp";
        out.command = "mcp";
        out.read_only = read_only;
        out.status = status;
        const bool ok = sink->append(out);
        record_result(ok);
        return ok;
    } catch (...) {
        record_result(false);
        return false;
    }
}

} // namespace fairyfly::mcp
