#include "include/mcp/transport.h"

#include <cstdio>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace fairyfly::mcp {

StdioTransport::StdioTransport() {
#ifdef _WIN32
    // No CRLF translation and no Ctrl-Z EOF handling: the protocol is byte-exact.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

bool StdioTransport::read_line(std::string& line) {
    if (!std::getline(std::cin, line)) return false;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return true;
}

void StdioTransport::write_line(std::string_view text) {
    // One buffer, one fwrite: a message is never split or interleaved.
    std::string buffer;
    buffer.reserve(text.size() + 1);
    buffer.append(text.data(), text.size());
    buffer.push_back('\n');
    std::lock_guard<std::mutex> lock(write_mutex_);
    std::fwrite(buffer.data(), 1, buffer.size(), stdout);
    std::fflush(stdout);
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
