#include "include/cli_entry.h"
#include "include/com/utf8.h"
#include "include/mcp/session_worker_main.h"
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>

int wmain(int argc, wchar_t** wargv) {
    std::vector<std::string> utf8_args;
    std::vector<char*> arg_ptrs;
    utf8_args.reserve(static_cast<size_t>(argc));
    arg_ptrs.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i)
        utf8_args.push_back(fairyfly::com::wide_to_utf8(wargv[i]));
    for (auto& arg : utf8_args)
        arg_ptrs.push_back(arg.data());

    // Attach a background process to the interactive desktop for SAP GUI ROT access.
    HDESK hDesk = OpenDesktopA("Default", 0, FALSE, GENERIC_ALL);
    if (hDesk) SetThreadDesktop(hDesk);
    if (argc == 2 && utf8_args[1] == "--mcp-session-worker")
        return fairyfly::mcp::run_session_worker_stdio();
    return fairyfly::cli::run_cli(argc, arg_ptrs.data());
}
#else
int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--mcp-session-worker")
        return fairyfly::mcp::run_session_worker_stdio();
    return fairyfly::cli::run_cli(argc, argv);
}
#endif
