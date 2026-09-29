#pragma once
#include "include/mcp/types.h"

namespace fairyfly::mcp {

/// MCP protocol engine over a Transport. run() blocks until EOF on the transport and returns the
/// process exit code (0 = clean shutdown). Threading (phase 1): the calling (main) thread owns
/// COM and executes tool calls FIFO; a reader thread answers ping/cancel and enqueues calls.
class McpServer {
public:
    McpServer(Transport& transport, ToolProvider& provider, ServerOptions options)
        : transport_(transport), provider_(provider), options_(std::move(options)) {}
    int run();
private:
    Transport& transport_;
    ToolProvider& provider_;
    ServerOptions options_;
};

} // namespace fairyfly::mcp
