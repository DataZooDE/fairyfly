#pragma once

// RAII wrappers for results of CredReadW / CredEnumerateW. The credential blob
// is scrubbed before the memory is returned to the system.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>

#include <cstddef>
#include <memory>

namespace fairyfly::cred {

inline void scrub_credential(CREDENTIALW* credential) noexcept {
    if (credential && credential->CredentialBlob && credential->CredentialBlobSize > 0) {
        SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
    }
}

struct CredentialDeleter {
    void operator()(CREDENTIALW* credential) const noexcept {
        if (!credential) return;
        scrub_credential(credential);
        CredFree(credential);
    }
};

using CredentialPtr = std::unique_ptr<CREDENTIALW, CredentialDeleter>;

/// Owns the array returned by CredEnumerateW.
class CredentialArray {
public:
    CredentialArray() = default;
    CredentialArray(PCREDENTIALW* items, DWORD count) : items_(items), count_(count) {}
    CredentialArray(const CredentialArray&) = delete;
    CredentialArray& operator=(const CredentialArray&) = delete;
    CredentialArray(CredentialArray&& other) noexcept : items_(other.items_), count_(other.count_) {
        other.items_ = nullptr;
        other.count_ = 0;
    }
    CredentialArray& operator=(CredentialArray&& other) noexcept {
        if (this != &other) {
            reset();
            items_ = other.items_;
            count_ = other.count_;
            other.items_ = nullptr;
            other.count_ = 0;
        }
        return *this;
    }
    ~CredentialArray() { reset(); }

    size_t size() const { return count_; }
    const CREDENTIALW& operator[](size_t index) const { return *items_[index]; }

private:
    void reset() noexcept {
        if (!items_) return;
        for (DWORD i = 0; i < count_; ++i) scrub_credential(items_[i]);
        CredFree(items_);
        items_ = nullptr;
        count_ = 0;
    }

    PCREDENTIALW* items_ = nullptr;
    DWORD count_ = 0;
};

} // namespace fairyfly::cred
