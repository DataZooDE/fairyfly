#include "jws.hpp"

#include "util.hpp"

namespace ffy::jws {

std::string Token::str(const char* k) const {
    return header.contains(k) && header[k].is_string() ? header[k].get<std::string>() : std::string();
}

Token parse(const std::string& s) {
    auto d1 = s.find('.');
    auto d2 = d1 == std::string::npos ? d1 : s.find('.', d1 + 1);
    if (d2 == std::string::npos || s.find('.', d2 + 1) != std::string::npos) throw Error("not a compact JWS");
    Token t;
    try {
        t.header = nlohmann::json::parse(b64url_decode(s.substr(0, d1)));
        t.payload = b64url_decode(s.substr(d1 + 1, d2 - d1 - 1));
        t.sig = b64url_decode(s.substr(d2 + 1));
    } catch (const nlohmann::json::exception&) {
        throw Error("malformed JWS header");
    }
    if (!t.header.is_object()) throw Error("malformed JWS header");
    t.input = s.substr(0, d2);
    return t;
}

std::string encode(const nlohmann::json& header, const nlohmann::json& claims, const std::function<std::string(const std::string&)>& sign) {
    std::string input = b64url_encode(header.dump()) + "." + b64url_encode(claims.dump());
    return input + "." + b64url_encode(sign(input));
}

}  // namespace ffy::jws
