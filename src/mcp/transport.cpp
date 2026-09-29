#include "include/mcp/transport.h"

#include <iostream>

namespace fairyfly::mcp {

// PHASE 1: real binary-mode stdio (owner: phase 1 worker). Stub uses std::cin/std::cout.
StdioTransport::StdioTransport() {}

bool StdioTransport::read_line(std::string& line) {
    if (!std::getline(std::cin, line)) return false;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return true;
}

void StdioTransport::write_line(std::string_view text) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    std::cout.write(text.data(), static_cast<std::streamsize>(text.size()));
    std::cout.put('\n');
    std::cout.flush();
}

bool StringTransport::read_line(std::string& line) {
    if (!std::getline(in_, line)) return false;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return true;
}

void StringTransport::write_line(std::string_view text) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    out_.write(text.data(), static_cast<std::streamsize>(text.size()));
    out_.put('\n');
    out_.flush();
}

} // namespace fairyfly::mcp
