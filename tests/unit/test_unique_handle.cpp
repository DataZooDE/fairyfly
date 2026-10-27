// UniqueHandle: one owner for a Win32 HANDLE (review 2026-10-25: three local copies and hand-closed handles).
#include <catch2/catch_test_macros.hpp>

#include <utility>

#include "include/system/unique_handle.h"

using fairyfly::system::UniqueHandle;

namespace {
bool open(HANDLE h) {
    DWORD flags = 0;
    return GetHandleInformation(h, &flags) != FALSE;
}
}  // namespace

TEST_CASE("unique handle: closes what it owns, once, and moves", "[system][handle]") {
    HANDLE raw = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    REQUIRE(raw);
    {
        UniqueHandle owner(raw);
        CHECK(owner);
        CHECK(owner.get() == raw);
        UniqueHandle moved = std::move(owner);
        CHECK_FALSE(owner);
        CHECK(moved.get() == raw);
        CHECK(open(raw));
    }
    CHECK_FALSE(open(raw));   // closed when the last owner ended

    HANDLE kept = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    {
        UniqueHandle owner(kept);
        CHECK(owner.release() == kept);   // the caller owns it again
    }
    CHECK(open(kept));
    CloseHandle(kept);
}

TEST_CASE("unique handle: null and INVALID_HANDLE_VALUE are empty; out() fills an out-parameter", "[system][handle]") {
    CHECK_FALSE(UniqueHandle());
    CHECK_FALSE(UniqueHandle(INVALID_HANDLE_VALUE));   // CreateFileW's failure: never closed
    UniqueHandle read, write;
    REQUIRE(CreatePipe(read.out(), write.out(), nullptr, 0));
    CHECK(read);
    CHECK(write);
    HANDLE first = read.get();
    read.reset();
    CHECK_FALSE(read);
    CHECK_FALSE(open(first));
}
