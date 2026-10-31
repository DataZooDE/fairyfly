#include "include/console_output.h"

#ifdef _WIN32
#include <windows.h>

#include <atomic>

namespace fairyfly::console {

namespace {
std::atomic<unsigned> previous_code_page{0};  // 0: not switched

bool is_console(DWORD which) {
    const HANDLE h = GetStdHandle(which);
    DWORD mode = 0;
    return h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode);
}
}  // namespace

void use_utf8_output() {
    const UINT current = GetConsoleOutputCP();
    const bool attached = is_console(STD_OUTPUT_HANDLE) || is_console(STD_ERROR_HANDLE);
    if (!needs_utf8_output(attached, current) || current == 0) return;
    if (SetConsoleOutputCP(CP_UTF8)) previous_code_page = current;
}

void restore_output_code_page() {
    const unsigned previous = previous_code_page.exchange(0);
    if (previous != 0) SetConsoleOutputCP(previous);
}

}  // namespace fairyfly::console
#else
namespace fairyfly::console {
void use_utf8_output() {}
void restore_output_code_page() {}
}  // namespace fairyfly::console
#endif
