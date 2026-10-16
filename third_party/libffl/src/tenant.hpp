// Tenant scope: the only way to address tenant data in the store and on the bus.
// A scope is built from a validated identifier, so it can be embedded in Firestore paths and Redis keys as is.
#pragma once
#include <string>

#include "util.hpp"

namespace ffy {

class TenantScope {
public:
    // Accepts 1-63 characters of [A-Za-z0-9_-]; throws ffy::Error otherwise.
    static TenantScope parse(const std::string& id) {
        bool ok = !id.empty() && id.size() <= 63;
        for (char c : id) ok = ok && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-');
        if (!ok) throw Error("invalid tenant id");
        return TenantScope(id);
    }
    const std::string& id() const { return id_; }
    bool operator==(const TenantScope& o) const { return id_ == o.id_; }
    bool operator!=(const TenantScope& o) const { return id_ != o.id_; }

private:
    explicit TenantScope(std::string id) : id_(std::move(id)) {}
    std::string id_;
};

}  // namespace ffy
