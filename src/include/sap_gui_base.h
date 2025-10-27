#pragma once

#include <string>
#include <memory>
#include <stdexcept>
#include <windows.h>
#include <comdef.h>

namespace fairyfly {
namespace sap {

/// Exception for SAP GUI wrapper errors
class SapGuiException : public std::runtime_error {
private:
    HRESULT hresult_;
    std::string object_type_;
    std::string operation_;

public:
    explicit SapGuiException(
        const std::string& message,
        HRESULT hr = S_OK,
        const std::string& obj_type = "",
        const std::string& op = ""
    ) : std::runtime_error(message),
        hresult_(hr),
        object_type_(obj_type),
        operation_(op) {}

    HRESULT hresult() const noexcept { return hresult_; }
    const std::string& object_type() const noexcept { return object_type_; }
    const std::string& operation() const noexcept { return operation_; }
};

/// Get DISPID for a method/property name using ITypeInfo
/// Required for SAP GUI COM objects - IDispatch::GetIDsOfNames fails
HRESULT get_dispid_via_typeinfo(IDispatch* obj, const wchar_t* name, DISPID* dispid);

/// Base class for all SAP GUI COM object wrappers
/// Provides RAII lifecycle management and common property accessors
class SapGuiObject {
protected:
    IDispatchPtr dispatch_;
    mutable std::string cached_id_;
    mutable std::string cached_type_;
    mutable bool id_cached_ = false;
    mutable bool type_cached_ = false;

    /// Get string property from COM object
    std::string get_string_property(const wchar_t* name) const;

    /// Get integer property from COM object
    int get_int_property(const wchar_t* name) const;

    /// Get boolean property from COM object
    bool get_bool_property(const wchar_t* name) const;

    /// Get IDispatch property from COM object
    IDispatchPtr get_dispatch_property(const wchar_t* name) const;

    /// Set string property on COM object
    void set_string_property(const wchar_t* name, const std::string& value);

    /// Set integer property on COM object
    void set_int_property(const wchar_t* name, int value);

    /// Set boolean property on COM object
    void set_bool_property(const wchar_t* name, bool value);

    /// Invoke method with no parameters, returning IDispatch
    IDispatchPtr invoke_method_dispatch(const wchar_t* method_name) const;

    /// Invoke method with string parameter, no return
    void invoke_method_with_string(const wchar_t* method_name, const std::string& param);

    /// Invoke method with int parameter, no return
    void invoke_method_with_int(const wchar_t* method_name, int param);

    /// Invoke method with no parameters, no return
    void invoke_method_void(const wchar_t* method_name);

public:
    /// Construct from existing IDispatch pointer
    explicit SapGuiObject(IDispatchPtr dispatch);

    /// Virtual destructor for proper cleanup
    virtual ~SapGuiObject() = default;

    /// Get object ID (path like "/app/con[0]/ses[0]")
    virtual std::string get_id() const;

    /// Get object type string (GuiSession, GuiConnection, etc.)
    virtual std::string get_type() const;

    /// Get object type as number
    virtual int get_type_as_number() const;

    /// Get object name
    virtual std::string get_name() const;

    /// Get raw COM IDispatch pointer (for advanced use)
    IDispatch* get_dispatch() const { return dispatch_; }

    /// Check if wrapped object is valid
    bool is_valid() const { return dispatch_ != nullptr; }

    /// Equality comparison by COM object identity
    bool operator==(const SapGuiObject& other) const;
    bool operator!=(const SapGuiObject& other) const { return !(*this == other); }
};

/// Smart pointer type for SapGuiObject
using SapGuiObjectPtr = std::shared_ptr<SapGuiObject>;

/// Collection iterator template for SAP GUI collections
/// Hides IEnumVARIANT complexity behind STL-compatible interface
template<typename T>
class SapGuiCollectionIterator {
private:
    IDispatchPtr collection_;
    IEnumVARIANT* enumerator_ = nullptr;
    int current_index_ = 0;
    bool at_end_ = false;
    std::shared_ptr<T> current_item_;

    void fetch_next() {
        if (!enumerator_ || at_end_) {
            at_end_ = true;
            current_item_.reset();
            return;
        }

        VARIANT item_var;
        VariantInit(&item_var);
        ULONG fetched = 0;
        HRESULT hr = enumerator_->Next(1, &item_var, &fetched);

        if (FAILED(hr) || fetched == 0) {
            at_end_ = true;
            current_item_.reset();
            VariantClear(&item_var);
            return;
        }

        if (item_var.vt == VT_DISPATCH && item_var.pdispVal) {
            IDispatchPtr item_dispatch(item_var.pdispVal, true); // Attach without AddRef
            current_item_ = std::make_shared<T>(item_dispatch);
            current_index_++;
        } else {
            at_end_ = true;
            current_item_.reset();
        }

        VariantClear(&item_var);
    }

public:
    using iterator_category = std::input_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = T*;
    using reference = T&;

    /// Construct end iterator
    SapGuiCollectionIterator() : at_end_(true) {}

    /// Construct begin iterator from collection
    explicit SapGuiCollectionIterator(IDispatchPtr collection)
        : collection_(collection), current_index_(0), at_end_(false) {

        // Get _NewEnum property
        _variant_t enum_var;
        DISPPARAMS no_params = {nullptr, nullptr, 0, 0};
        HRESULT hr = collection_->Invoke(
            DISPID_NEWENUM,
            IID_NULL,
            LOCALE_USER_DEFAULT,
            DISPATCH_PROPERTYGET | DISPATCH_METHOD,
            &no_params,
            &enum_var,
            nullptr,
            nullptr
        );

        if (FAILED(hr) || enum_var.vt != VT_UNKNOWN) {
            at_end_ = true;
            return;
        }

        // QueryInterface for IEnumVARIANT
        hr = enum_var.punkVal->QueryInterface(IID_IEnumVARIANT, (void**)&enumerator_);
        if (FAILED(hr)) {
            at_end_ = true;
            return;
        }

        // Fetch first item
        fetch_next();
    }

    /// Copy constructor
    SapGuiCollectionIterator(const SapGuiCollectionIterator& other)
        : collection_(other.collection_),
          current_index_(other.current_index_),
          at_end_(other.at_end_),
          current_item_(other.current_item_) {

        if (other.enumerator_ && !at_end_) {
            // Clone enumerator
            other.enumerator_->Clone(&enumerator_);
        }
    }

    /// Destructor
    ~SapGuiCollectionIterator() {
        if (enumerator_) {
            enumerator_->Release();
        }
    }

    /// Dereference operator
    T& operator*() const { return *current_item_; }
    T* operator->() const { return current_item_.get(); }

    /// Pre-increment
    SapGuiCollectionIterator& operator++() {
        fetch_next();
        return *this;
    }

    /// Post-increment
    SapGuiCollectionIterator operator++(int) {
        SapGuiCollectionIterator tmp(*this);
        ++(*this);
        return tmp;
    }

    /// Equality comparison
    bool operator==(const SapGuiCollectionIterator& other) const {
        if (at_end_ && other.at_end_) return true;
        if (at_end_ != other.at_end_) return false;
        return current_index_ == other.current_index_;
    }

    bool operator!=(const SapGuiCollectionIterator& other) const {
        return !(*this == other);
    }
};

/// Collection wrapper template for SAP GUI collections
/// Provides STL-compatible container interface with begin()/end()
template<typename T>
class SapGuiCollection {
private:
    IDispatchPtr collection_;
    int cached_count_ = -1;

public:
    using iterator = SapGuiCollectionIterator<T>;
    using const_iterator = SapGuiCollectionIterator<T>;

    /// Construct from IDispatch collection object
    explicit SapGuiCollection(IDispatchPtr collection)
        : collection_(collection) {}

    /// Get collection count
    int count() const {
        if (cached_count_ >= 0) {
            return cached_count_;
        }

        if (!collection_) {
            return 0;
        }

        // Get Count property using ITypeInfo pattern
        DISPID dispid;
        HRESULT hr = get_dispid_via_typeinfo(collection_, L"Count", &dispid);
        if (FAILED(hr)) {
            return 0;
        }

        DISPPARAMS no_params = {nullptr, nullptr, 0, 0};
        _variant_t result;
        hr = collection_->Invoke(
            dispid,
            IID_NULL,
            LOCALE_USER_DEFAULT,
            DISPATCH_PROPERTYGET | DISPATCH_METHOD,
            &no_params,
            &result,
            nullptr,
            nullptr
        );

        if (SUCCEEDED(hr) && (result.vt == VT_I4 || result.vt == VT_I2)) {
            cached_count_ = (int)result;
            return cached_count_;
        }

        return 0;
    }

    /// Get item by enumeration index (not absolute collection index)
    std::shared_ptr<T> item(int index) const {
        if (index < 0 || !collection_) {
            return nullptr;
        }

        // Use IEnumVARIANT to handle sparse indices
        _variant_t enum_var;
        DISPPARAMS no_params = {nullptr, nullptr, 0, 0};
        HRESULT hr = collection_->Invoke(
            DISPID_NEWENUM,
            IID_NULL,
            LOCALE_USER_DEFAULT,
            DISPATCH_PROPERTYGET | DISPATCH_METHOD,
            &no_params,
            &enum_var,
            nullptr,
            nullptr
        );

        if (FAILED(hr) || enum_var.vt != VT_UNKNOWN) {
            return nullptr;
        }

        IEnumVARIANT* enumerator = nullptr;
        hr = enum_var.punkVal->QueryInterface(IID_IEnumVARIANT, (void**)&enumerator);
        if (FAILED(hr)) {
            return nullptr;
        }

        // Skip to requested index
        if (index > 0) {
            hr = enumerator->Skip(index);
            if (FAILED(hr)) {
                enumerator->Release();
                return nullptr;
            }
        }

        // Fetch the item
        VARIANT item_var;
        VariantInit(&item_var);
        ULONG fetched = 0;
        hr = enumerator->Next(1, &item_var, &fetched);
        enumerator->Release();

        if (FAILED(hr) || fetched == 0) {
            VariantClear(&item_var);
            return nullptr;
        }

        if (item_var.vt == VT_DISPATCH && item_var.pdispVal) {
            IDispatchPtr item_dispatch(item_var.pdispVal, true); // Attach without AddRef
            auto result = std::make_shared<T>(item_dispatch);
            VariantClear(&item_var);
            return result;
        }

        VariantClear(&item_var);
        return nullptr;
    }

    /// STL-compatible begin iterator
    iterator begin() { return iterator(collection_); }
    const_iterator begin() const { return const_iterator(collection_); }

    /// STL-compatible end iterator
    iterator end() { return iterator(); }
    const_iterator end() const { return const_iterator(); }

    /// Check if collection is empty
    bool empty() const { return count() == 0; }

    /// Get collection size (alias for count)
    int size() const { return count(); }
};

} // namespace sap
} // namespace fairyfly
