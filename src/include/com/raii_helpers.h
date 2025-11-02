#pragma once

#include <windows.h>
#include <comdef.h>
#include <memory>
#include <cstdio>

namespace fairyfly {
namespace com {

/// RAII wrapper for VARIANT objects
/// Automatically calls VariantInit on construction and VariantClear on destruction
class VariantGuard {
private:
    VARIANT* var_;

    // Non-copyable
    VariantGuard(const VariantGuard&) = delete;
    VariantGuard& operator=(const VariantGuard&) = delete;

public:
    /// Construct and initialize VARIANT
    explicit VariantGuard(VARIANT* v) : var_(v) {
        if (var_) {
            VariantInit(var_);
        }
    }

    /// Construct for standalone VARIANT (stack-allocated)
    VariantGuard() {
        var_ = new VARIANT;
        VariantInit(var_);
    }

    /// Destructor - automatically clears VARIANT
    ~VariantGuard() {
        if (var_) {
            VariantClear(var_);
        }
    }

    /// Get pointer to VARIANT
    VARIANT* get() { return var_; }
    const VARIANT* get() const { return var_; }

    /// Dereference operator
    VARIANT& operator*() { return *var_; }
    const VARIANT& operator*() const { return *var_; }

    /// Arrow operator
    VARIANT* operator->() { return var_; }
    const VARIANT* operator->() const { return var_; }

    /// Release ownership (caller must call VariantClear)
    VARIANT* release() {
        VARIANT* v = var_;
        var_ = nullptr;
        return v;
    }
};

/// RAII wrapper for SafeArray access
/// Automatically calls SafeArrayUnaccessData on destruction
class SafeArrayGuard {
private:
    SAFEARRAY* psa_;
    bool is_locked_;
    BYTE* data_ptr_;

    // Non-copyable
    SafeArrayGuard(const SafeArrayGuard&) = delete;
    SafeArrayGuard& operator=(const SafeArrayGuard&) = delete;

public:
    /// Construct and lock SafeArray
    explicit SafeArrayGuard(SAFEARRAY* psa) : psa_(psa), is_locked_(false), data_ptr_(nullptr) {
        if (psa_) {
            HRESULT hr = SafeArrayAccessData(psa_, reinterpret_cast<void**>(&data_ptr_));
            if (SUCCEEDED(hr)) {
                is_locked_ = true;
            }
        }
    }

    /// Destructor - automatically unlocks SafeArray
    ~SafeArrayGuard() {
        if (psa_ && is_locked_) {
            SafeArrayUnaccessData(psa_);
        }
    }

    /// Check if SafeArray was successfully locked
    bool is_locked() const { return is_locked_; }

    /// Get pointer to SafeArray
    SAFEARRAY* get() { return psa_; }
    const SAFEARRAY* get() const { return psa_; }

    /// Get data pointer (after successful lock)
    BYTE* data() { return data_ptr_; }
    const BYTE* data() const { return data_ptr_; }

    /// Get size of SafeArray (number of elements)
    long size() const {
        if (!psa_ || !is_locked_) {
            return 0;
        }
        return psa_->rgsabound[0].cElements;
    }
};

/// Custom deleter for FILE* pointers
struct FileDeleter {
    void operator()(FILE* f) const {
        if (f) {
            std::fclose(f);
        }
    }
};

/// Type alias for RAII FILE* management
using FileHandlePtr = std::unique_ptr<FILE, FileDeleter>;

/// Helper function to create a temporary file with RAII management
/// Returns nullptr on failure
inline FileHandlePtr create_temp_file() {
#ifdef _WIN32
    std::FILE* tmpfile = nullptr;
    errno_t err_code = tmpfile_s(&tmpfile);
    if (err_code != 0 || !tmpfile) {
        return nullptr;
    }
    return FileHandlePtr(tmpfile);
#else
    std::FILE* tmpfile = std::tmpfile();
    if (!tmpfile) {
        return nullptr;
    }
    return FileHandlePtr(tmpfile);
#endif
}

} // namespace com
} // namespace fairyfly

