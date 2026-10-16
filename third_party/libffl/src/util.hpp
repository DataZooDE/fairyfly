// Small shared helpers: encodings, randomness, time, strings, URLs.
#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ffy {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::string b64url_encode(std::string_view in);
std::string b64url_decode(std::string_view in);  // throws Error
std::string b64std_encode(std::string_view in);
std::string b64std_decode(std::string_view in);  // throws Error
std::string hex_encode(std::string_view in);

std::string rand_bytes(size_t n);
std::string rand_hex(size_t n);  // n random bytes, hex encoded (2n chars)

int64_t now_unix();
int64_t now_ms();
std::string rfc3339_now();

std::string lower(std::string s);
bool iequals(std::string_view a, std::string_view b);
std::string trim(std::string_view s);
std::vector<std::string> split_csv(std::string_view s);
std::string env_or(const char* name, const std::string& def = {});
std::vector<std::string> env_list(const char* name);
bool contains_ci(const std::vector<std::string>& list, std::string_view v);
std::string domain_of(std::string_view email);

std::string url_encode(std::string_view s);                       // RFC 3986 unreserved kept
std::string url_decode(std::string_view s);                       // %XX and '+'
std::string query_get(std::string_view query, std::string_view name);

struct Url {
    std::string scheme, host, port, target;  // target includes the query
    bool tls() const { return scheme == "https" || scheme == "wss"; }
};
Url parse_url(const std::string& url);  // throws Error

}  // namespace ffy
