#pragma once

// Secret handling helpers: buffers that are zeroed when destroyed or moved from.
// Nothing in this header ever logs or formats its contents.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <utility>

namespace fairyfly::cred {

/// Overwrite a string with zeros, then release its storage. The whole capacity is
/// zeroed (not just size()) so a moved-from short string cannot keep inline residue.
inline void scrub_string(std::string& value) noexcept {
    SecureZeroMemory(value.data(), (value.capacity() + 1) * sizeof(char));
    value.clear();
    value.shrink_to_fit();
}

inline void scrub_string(std::wstring& value) noexcept {
    SecureZeroMemory(value.data(), (value.capacity() + 1) * sizeof(wchar_t));
    value.clear();
    value.shrink_to_fit();
}

/// Owns one secret (password). Non-copyable; scrubbed on destruction and after a move.
/// Whichever representation was supplied is authoritative; the other one is a
/// lazily built cache that is scrubbed together with it.
class SecretBuffer {
public:
    SecretBuffer() = default;
    explicit SecretBuffer(std::string&& utf8) : utf8_(std::move(utf8)) { scrub_string(utf8); }
    explicit SecretBuffer(std::wstring&& utf16) : utf16_(std::move(utf16)) { scrub_string(utf16); }

    SecretBuffer(const SecretBuffer&) = delete;
    SecretBuffer& operator=(const SecretBuffer&) = delete;

    SecretBuffer(SecretBuffer&& other) noexcept
        : utf8_(std::move(other.utf8_)), utf16_(std::move(other.utf16_)) {
        other.clear();
    }
    SecretBuffer& operator=(SecretBuffer&& other) noexcept {
        if (this != &other) {
            clear();
            utf8_ = std::move(other.utf8_);
            utf16_ = std::move(other.utf16_);
            other.clear();
        }
        return *this;
    }
    ~SecretBuffer() { clear(); }

    /// UTF-8 view (converted lazily from UTF-16 when needed).
    const std::string& utf8() const {
        if (utf8_.empty() && !utf16_.empty()) utf8_ = to_utf8(utf16_);
        return utf8_;
    }

    /// UTF-16 view (converted lazily from UTF-8 when needed).
    const std::wstring& utf16() const {
        if (utf16_.empty() && !utf8_.empty()) utf16_ = to_utf16(utf8_);
        return utf16_;
    }

    bool empty() const { return utf8_.empty() && utf16_.empty(); }

    void clear() noexcept {
        scrub_string(utf8_);
        scrub_string(utf16_);
    }

private:
    static std::string to_utf8(const std::wstring& in) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, in.data(), static_cast<int>(in.size()), nullptr, 0, nullptr, nullptr);
        if (n <= 0) return {};
        std::string out(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, in.data(), static_cast<int>(in.size()), out.data(), n, nullptr, nullptr);
        return out;
    }
    static std::wstring to_utf16(const std::string& in) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, in.data(), static_cast<int>(in.size()), nullptr, 0);
        if (n <= 0) return {};
        std::wstring out(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, in.data(), static_cast<int>(in.size()), out.data(), n);
        return out;
    }

    mutable std::string utf8_;
    mutable std::wstring utf16_;
};

} // namespace fairyfly::cred
