// QR codes for the pairing link (plan 12.1): terminal (UTF-8 half blocks), SVG and PNG. Uses Nayuki's encoder.
#pragma once
#include <string>
#include <vector>

namespace ffy::qr {

struct Matrix {
    int size = 0;
    std::vector<bool> dark;  // row-major
    bool at(int x, int y) const { return x >= 0 && y >= 0 && x < size && y < size && dark[size_t(y) * size + x]; }
};

Matrix encode(const std::string& text);              // error correction level M
std::string to_terminal(const Matrix& m);            // two rows per line, quiet zone included
std::string to_svg(const Matrix& m, int border = 4);
std::string to_png(const Matrix& m, int scale = 8, int border = 4);  // grayscale PNG bytes

}  // namespace ffy::qr
