#include "include/mcp/server.h"

namespace fairyfly::mcp {

// PHASE 1: real protocol engine (owner: phase 1 worker). Stub drains the transport and exits.
int McpServer::run() {
    std::string line;
    while (transport_.read_line(line)) {}
    return 0;
}

} // namespace fairyfly::mcp
