#include "include/console_output.h"

namespace fairyfly::console {

bool CodePageGuard::switch_to_utf8() {
    if (!api_.get || !api_.set || !api_.console_attached) return false;
    const unsigned current = api_.get();
    if (current == 0 || !needs_utf8_output(api_.console_attached(), current)) return false;
    if (!api_.set(kUtf8CodePage)) return false;
    previous_ = current;
    return true;
}

bool CodePageGuard::restore() {
    const unsigned previous = previous_.exchange(0);
    if (previous == 0) return false;
    if (api_.get() != kUtf8CodePage) return false;  // changed by someone else since: theirs now
    return api_.set(previous);
}

}  // namespace fairyfly::console

#ifdef _WIN32
#include <windows.h>

namespace fairyfly::console {

namespace {
bool is_console(DWORD which) {
    const HANDLE h = GetStdHandle(which);
    DWORD mode = 0;
    return h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode);
}

CodePageGuard& process_guard() {
    static CodePageGuard guard({[] { return static_cast<unsigned>(GetConsoleOutputCP()); },
                                [](unsigned cp) { return SetConsoleOutputCP(cp) != FALSE; },
                                [] { return is_console(STD_OUTPUT_HANDLE) || is_console(STD_ERROR_HANDLE); }});
    return guard;
}

// last in the handler chain (installed first): reached only when the process is about to end by the default handling
BOOL WINAPI restore_on_ctrl(DWORD) {
    process_guard().restore();
    return FALSE;
}
}  // namespace

void use_utf8_output() {
    if (process_guard().switch_to_utf8()) SetConsoleCtrlHandler(restore_on_ctrl, TRUE);
}

void restore_output_code_page() { process_guard().restore(); }

}  // namespace fairyfly::console
#else
namespace fairyfly::console {
void use_utf8_output() {}
void restore_output_code_page() {}
}  // namespace fairyfly::console
#endif
