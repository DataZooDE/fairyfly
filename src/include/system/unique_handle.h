#pragma once
// One owner for a Win32 HANDLE: closed exactly once when the owner ends. Null and INVALID_HANDLE_VALUE (the two
// failure values Win32 uses) are both "no handle".
#include <windows.h>

namespace fairyfly::system {

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) : handle_(handle) {}
    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    ~UniqueHandle() { reset(); }

    HANDLE get() const { return handle_; }
    explicit operator bool() const { return handle_ && handle_ != INVALID_HANDLE_VALUE; }
    /// Gives the handle up without closing it.
    HANDLE release() {
        HANDLE handle = handle_;
        handle_ = nullptr;
        return handle;
    }
    void reset(HANDLE handle = nullptr) {
        if (*this) CloseHandle(handle_);
        handle_ = handle;
    }
    /// For a Win32 out-parameter (CreatePipe, OpenProcessToken): closes what it held and lends its slot.
    HANDLE* out() {
        reset();
        return &handle_;
    }

private:
    HANDLE handle_ = nullptr;
};

}  // namespace fairyfly::system
