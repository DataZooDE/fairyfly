#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace fairyfly {
namespace utils {

/// Encodes binary data to base64 string
/// \param bytes Binary data to encode
/// \return Base64-encoded string
std::string base64_encode(const std::vector<uint8_t>& bytes);

} // namespace utils
} // namespace fairyfly
