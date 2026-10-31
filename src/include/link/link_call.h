#pragma once
// One tool call over the cloud link (D10): what libffl's agent says about it (ffy::link::CallInfo, from the gateway's
// signed assertion) becomes the dispatcher's CallContext. Portable: libffl and the MCP types only.
#include <atomic>
#include <string>

#include "include/mcp/types.h"
#include "link_agent.hpp"  // libffl

namespace fairyfly::link {

/// The assistant acting for this user of this organisation: read-only unless the gateway granted a write for exactly
/// this call, limited to the grant's transaction list with the same rules as a `mcp token create --tcode` token, and
/// cancelled when the gateway cancels the call (timeout, client gone) or the link stops (`stop`). A batch checks
/// ctx.cancelled before each step.
inline mcp::CallContext link_call_context(const ffy::link::CallInfo& info, const std::atomic<bool>& stop) {
    mcp::CallContext ctx;
    ctx.link_request = true;
    ctx.sap_startup_granted = info.sap_startup_granted;
    ctx.request_id = info.request_id;
    ctx.cancelled = [&stop, cancel = info.cancel] { return stop.load() || (cancel && cancel->load()); };
    ctx.principal.name = "link:" + info.tenant + "/" + info.user;
    ctx.principal.read_only = !info.write_granted;
    ctx.principal.tcodes = info.tcodes;  // the gateway's sentinel "-none-" matches no transaction: nothing on a screen
    ctx.principal.authenticated = true;
    return ctx;
}

}  // namespace fairyfly::link
