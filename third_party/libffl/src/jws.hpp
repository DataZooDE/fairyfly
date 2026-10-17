// Compact JWS helpers.
#pragma once
#include <functional>
#include <nlohmann/json.hpp>
#include <string>

namespace ffy::jws {

struct Token {
    nlohmann::json header;
    std::string payload;  // decoded bytes
    std::string input;    // "b64(header).b64(payload)" - the signed bytes
    std::string sig;      // decoded
    std::string str(const char* header_key) const;
};

Token parse(const std::string& compact);  // throws ffy::Error; does NOT verify
// sign receives the signing input and returns a raw (r||s) signature.
std::string encode(const nlohmann::json& header, const nlohmann::json& claims, const std::function<std::string(const std::string&)>& sign);

}  // namespace ffy::jws
