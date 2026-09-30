#pragma once
// Read-only collector behind `mcp doctor`'s http.sys checks (elevation, manifest, urlacl, sslcert, certificate,
// firewall, TLS handshake). Works against the setup hosts, so tests use the in-memory fakes. Never elevates and
// never changes anything.

#include "include/config/mcp_doctor.h"
#include "include/setup/setup_hosts.h"

namespace fairyfly::setup {

config::SetupFacts collect_setup_facts(Hosts& hosts, const config::SetupQuery& query);

} // namespace fairyfly::setup
