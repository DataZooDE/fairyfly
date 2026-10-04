#pragma once

#include <optional>

#include "include/auth/token_store.h"

namespace fairyfly::mcp {

inline bool token_definitively_invalid(const auth::TokenMeta& meta, auth::TimePoint now) {
    return meta.revoked || (meta.expires && now >= *meta.expires);
}

inline bool token_definitively_invalid(const std::optional<auth::TokenMeta>& meta, auth::TimePoint now) {
    // A missing record may be a transient Credential Manager or chunk-read
    // failure. Authentication rejects it, but removing a persistent T-code
    // denial would allow the same token to bypass policy when the read recovers.
    return meta && token_definitively_invalid(*meta, now);
}

} // namespace fairyfly::mcp
