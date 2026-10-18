// Access tokens for Google REST APIs (Cloud KMS, Firestore) from the Cloud Run metadata server.
#pragma once
#include <memory>
#include <mutex>
#include <string>

namespace ffy {

class GcpToken {
public:
    virtual ~GcpToken() = default;
    virtual std::string token() = 0;
};

// Metadata server of the runtime service account; cached until shortly before expiry.
// base defaults to $FFY_METADATA_BASE or http://metadata.google.internal.
std::shared_ptr<GcpToken> make_metadata_token(std::string base = {});
std::shared_ptr<GcpToken> make_static_token(std::string token);  // development / tests

}  // namespace ffy
