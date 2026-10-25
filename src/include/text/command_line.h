#pragma once
// Windows command-line quoting (the rules CommandLineToArgvW and the MSVC runtime parse by), for char and wchar_t.
// Header-only and dependency-free: the standalone tray launcher (fairyfly_tray.exe, no fairyfly_core) uses it too.
// Not for PowerShell or cmd.exe syntax.
#include <initializer_list>
#include <string>
#include <string_view>

namespace fairyfly::text {

/// Always in double quotes; backslashes before a quote (and at the end) doubled, quotes escaped.
template <class Char>
std::basic_string<Char> quote_argument_always(std::basic_string_view<Char> arg) {
    std::basic_string<Char> out(1, Char('"'));
    std::size_t backslashes = 0;
    for (Char c : arg) {
        if (c == Char('\\')) {
            ++backslashes;
        } else if (c == Char('"')) {
            out.append(backslashes * 2 + 1, Char('\\'));
            out.push_back(c);
            backslashes = 0;
        } else {
            out.append(backslashes, Char('\\'));
            out.push_back(c);
            backslashes = 0;
        }
    }
    out.append(backslashes * 2, Char('\\'));
    out.push_back(Char('"'));
    return out;
}

/// As is when it needs no quotes (not empty, no space, tab, newline, vertical tab or quote), else quoted.
template <class Char>
std::basic_string<Char> quote_argument(std::basic_string_view<Char> arg) {
    static constexpr Char kSpecial[] = {Char(' '), Char('\t'), Char('\n'), Char('\v'), Char('"'), Char(0)};
    if (!arg.empty() && arg.find_first_of(kSpecial) == std::basic_string_view<Char>::npos)
        return std::basic_string<Char>(arg);
    return quote_argument_always<Char>(arg);
}

/// "program arg1 arg2 ...", each part quoted when needed.
template <class Char, class Args>
std::basic_string<Char> join_command_line(std::basic_string_view<Char> program, const Args& args) {
    std::basic_string<Char> out = quote_argument<Char>(program);
    for (const auto& a : args) {
        out.push_back(Char(' '));
        out += quote_argument<Char>(std::basic_string_view<Char>(a));
    }
    return out;
}

template <class Char>
std::basic_string<Char> join_command_line(std::basic_string_view<Char> program, std::initializer_list<const Char*> args) {
    std::basic_string<Char> out = quote_argument<Char>(program);
    for (const Char* a : args) {
        out.push_back(Char(' '));
        out += quote_argument<Char>(std::basic_string_view<Char>(a));
    }
    return out;
}

}  // namespace fairyfly::text
