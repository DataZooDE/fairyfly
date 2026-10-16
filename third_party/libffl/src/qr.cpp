#include "qr.hpp"

#include <cstdint>

#include "qrcodegen.hpp"

namespace ffy::qr {

Matrix encode(const std::string& text) {
    auto q = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
    Matrix m;
    m.size = q.getSize();
    m.dark.resize(size_t(m.size) * m.size);
    for (int y = 0; y < m.size; y++)
        for (int x = 0; x < m.size; x++) m.dark[size_t(y) * m.size + x] = q.getModule(x, y);
    return m;
}

std::string to_terminal(const Matrix& m) {
    // Light background, dark modules: "█" both dark, "▀" upper dark, "▄" lower dark, " " both light. Inverted so
    // that it scans on dark terminals too (light quiet zone drawn as full blocks).
    std::string out;
    const int b = 2;
    for (int y = -b; y < m.size + b; y += 2) {
        for (int x = -b; x < m.size + b; x++) {
            bool top = !m.at(x, y), bottom = !m.at(x, y + 1);  // light = drawn
            out += top && bottom ? "█" : top ? "▀" : bottom ? "▄" : " ";
        }
        out += "\n";
    }
    return out;
}

std::string to_svg(const Matrix& m, int border) {
    int n = m.size + 2 * border;
    std::string s = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 " + std::to_string(n) + " " + std::to_string(n) +
                    "\" shape-rendering=\"crispEdges\"><rect width=\"100%\" height=\"100%\" fill=\"#fff\"/><path fill=\"#000\" d=\"";
    for (int y = 0; y < m.size; y++)
        for (int x = 0; x < m.size; x++)
            if (m.at(x, y)) s += "M" + std::to_string(x + border) + "," + std::to_string(y + border) + "h1v1h-1z";
    return s + "\"/></svg>\n";
}

namespace {
uint32_t crc32(const std::string& d) {
    uint32_t c = 0xffffffffu;
    for (unsigned char b : d) {
        c ^= b;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}
std::string be32(uint32_t v) { return {char(v >> 24), char(v >> 16), char(v >> 8), char(v)}; }
std::string chunk(const std::string& type, const std::string& data) { return be32(uint32_t(data.size())) + type + data + be32(crc32(type + data)); }
}  // namespace

std::string to_png(const Matrix& m, int scale, int border) {
    int n = (m.size + 2 * border) * scale;
    std::string raw;
    raw.reserve(size_t(n) * (n + 1));
    for (int y = 0; y < n; y++) {
        raw += char(0);  // filter: none
        for (int x = 0; x < n; x++) raw += m.at(x / scale - border, y / scale - border) ? char(0) : char(255);
    }
    // zlib stream with stored (uncompressed) deflate blocks
    std::string z = "\x78\x01";
    for (size_t i = 0; i < raw.size(); i += 65535) {
        size_t len = std::min<size_t>(65535, raw.size() - i);
        z += char(i + len == raw.size() ? 1 : 0);
        z += char(len & 0xff); z += char(len >> 8);
        z += char(~len & 0xff); z += char((~len >> 8) & 0xff);
        z += raw.substr(i, len);
    }
    uint32_t a = 1, b2 = 0;
    for (unsigned char c : raw) { a = (a + c) % 65521; b2 = (b2 + a) % 65521; }
    z += be32((b2 << 16) | a);
    std::string ihdr = be32(uint32_t(n)) + be32(uint32_t(n)) + std::string("\x08\x00\x00\x00\x00", 5);  // 8-bit grayscale
    return std::string("\x89PNG\r\n\x1a\n", 8) + chunk("IHDR", ihdr) + chunk("IDAT", z) + chunk("IEND", "");
}

}  // namespace ffy::qr
