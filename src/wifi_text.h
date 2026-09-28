#pragma once
#include <stddef.h>
#include <cstdio>
#include <string>
namespace agentlink { namespace wifi_detail {
inline bool DecodeFormValue(char* dst, size_t capacity, const char* src) {
    if (!dst || !capacity || !src) return false;
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    size_t used = 0;
    for (size_t i = 0; src[i]; ++i) {
        char value = src[i] == '+' ? ' ' : src[i];
        if (src[i] == '%') {
            if (!src[i + 1] || !src[i + 2]) return false;
            const int hi = hex(src[i + 1]), lo = hex(src[i + 2]);
            if (hi < 0 || lo < 0) return false;
            value = static_cast<char>((hi << 4) | lo);
            i += 2;
        }
        if (!value || used + 1 >= capacity) return false;
        dst[used++] = value;
    }
    dst[used] = 0;
    return true;
}

// Append a JSON-escaped string (SSIDs can contain quotes/backslashes/control bytes).
inline void AppendJsonEscaped(std::string& out, const char* s) {
    for (const char* p = s; *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
            else out += static_cast<char>(c);
        }
    }
}


} }
