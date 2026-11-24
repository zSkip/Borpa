#include "Base64.hpp"

namespace Borpa { namespace Sim {

static const char k_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const void* data, size_t size)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    std::string    out;
    out.reserve((size + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < size; i += 3) {
        uint32_t v = (uint32_t(p[i]) << 16) | (uint32_t(p[i + 1]) << 8) | p[i + 2];
        out += k_alphabet[(v >> 18) & 63];
        out += k_alphabet[(v >> 12) & 63];
        out += k_alphabet[(v >> 6) & 63];
        out += k_alphabet[v & 63];
    }
    if (i < size) {
        uint32_t v = uint32_t(p[i]) << 16;
        if (i + 1 < size)
            v |= uint32_t(p[i + 1]) << 8;
        out += k_alphabet[(v >> 18) & 63];
        out += k_alphabet[(v >> 12) & 63];
        out += i + 1 < size ? k_alphabet[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

bool base64_decode(const std::string& text, std::vector<uint8_t>& out)
{
    out.clear();
    out.reserve(text.size() / 4 * 3);
    uint32_t acc  = 0;
    int      bits = 0;
    size_t   pad  = 0;
    for (char c : text) {
        int v;
        if (c >= 'A' && c <= 'Z')
            v = c - 'A';
        else if (c >= 'a' && c <= 'z')
            v = c - 'a' + 26;
        else if (c >= '0' && c <= '9')
            v = c - '0' + 52;
        else if (c == '+')
            v = 62;
        else if (c == '/')
            v = 63;
        else if (c == '=') {
            ++pad;
            continue;
        } else
            return false;
        if (pad > 0)
            return false; // data after padding
        acc = (acc << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(uint8_t((acc >> bits) & 0xFF));
        }
    }
    return pad <= 2;
}

}} // namespace Borpa::Sim
