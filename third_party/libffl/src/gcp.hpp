// Access tokens for Google REST APIs (Cloud KMS, Firestore) from the Cloud Run metadata server.
#pragma once
#include <memory>
#include <mutex>
#include <string>

#include "http_client.hpp"

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

// For idempotent reads of Google REST APIs only (never a write: a write whose answer was lost may have been applied).
// One more attempt after a transport failure (HttpTransportError, not a plain Error) or HTTP 429/500/502/503/504, after a jittered 100-300 ms pause, logged as
// a WARNING (op, method, URL path, attempt, error); the retry gets what is left of budget_ms as its timeout and is
// skipped when under a second is left. The second outcome is returned or thrown as is.
HttpResponse http_do_read(const HttpRequest& req, const std::string& op, int budget_ms);

}  // namespace ffy
