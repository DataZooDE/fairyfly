#pragma once

#include "secret_buffer.h"

#include <istream>
#include <string>
#include <string_view>

namespace fairyfly::cred {

enum class PromptSource {
    Console,  ///< read from the console without echo
    Stdin,    ///< read the first line of standard input
};

/// True when standard input is an interactive console.
bool stdin_is_console();

/// Read one secret. Console mode writes the prompt to stderr and disables echo.
SecretBuffer read_secret(std::string_view prompt, PromptSource source);

/// Take the first line of a stream: strips a UTF-8 BOM and trailing CR/LF. Pure, testable.
SecretBuffer read_secret_line(std::istream& input);

} // namespace fairyfly::cred
