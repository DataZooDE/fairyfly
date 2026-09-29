#pragma once
#include <iosfwd>
#include <mutex>
#include "include/mcp/types.h"

namespace fairyfly::mcp {

/// Process stdin/stdout. Reads must not be echoed; nothing else in the process may write to stdout.
class StdioTransport : public Transport {
public:
    StdioTransport();  // PHASE 1: sets stdin/stdout to binary mode (no CRLF translation)
    bool read_line(std::string& line) override;
    void write_line(std::string_view line) override;
private:
    std::mutex write_mutex_;
};

/// In-memory transport for tests (istream in, ostream out).
class StringTransport : public Transport {
public:
    StringTransport(std::istream& in, std::ostream& out) : in_(in), out_(out) {}
    bool read_line(std::string& line) override;
    void write_line(std::string_view line) override;
private:
    std::istream& in_;
    std::ostream& out_;
    std::mutex write_mutex_;
};

} // namespace fairyfly::mcp
