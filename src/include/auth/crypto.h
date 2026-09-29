#pragma once
// Small crypto helpers for the token store (Windows CNG). Nothing here logs or formats a secret.

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace fairyfly::auth {

/// SHA-256 of `data` as 64 lower-case hex characters (CNG / bcrypt).
std::string sha256_hex(std::string_view data);

/// Constant-time equality (no early exit on the first differing byte; length differences are folded in).
bool constant_time_equal(std::string_view a, std::string_view b) noexcept;

/// Fills `buffer` with `size` cryptographically secure random bytes (BCryptGenRandom). Throws on failure.
void random_bytes(unsigned char* buffer, std::size_t size);

/// Injectable random source (tests use a deterministic one).
using RandomFn = std::function<void(unsigned char*, std::size_t)>;

/// RFC 4648 base64url without padding.
std::string base64url_encode(const unsigned char* data, std::size_t size);

/// Lower-case hex of `size` random bytes from `rng`.
std::string random_hex(const RandomFn& rng, std::size_t bytes);

} // namespace fairyfly::auth
