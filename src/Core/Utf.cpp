#include "Core/Utf.h"

namespace aiie::utf {

std::string ToUtf8(std::wstring_view w) {
    std::string out;
    out.reserve(w.size());
    for (size_t i = 0; i < w.size(); ++i) {
        const wchar_t c = w[i];
        uint32_t cp;
        size_t units;
        if (IsHighSurrogate(c) && i + 1 < w.size() && IsLowSurrogate(w[i + 1])) {
            cp = 0x10000 + ((static_cast<uint32_t>(c) - 0xD800) << 10) +
                 (static_cast<uint32_t>(w[i + 1]) - 0xDC00);
            units = 2;
        } else {
            cp = c;
            units = 1;
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        i += units - 1;
    }
    return out;
}

std::wstring FromUtf8(std::string_view s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char b = static_cast<unsigned char>(s[i]);
        uint32_t cp;
        size_t len;
        if (b < 0x80) {
            cp = b;
            len = 1;
        } else if ((b & 0xE0) == 0xC0) {
            cp = b & 0x1F;
            len = 2;
        } else if ((b & 0xF0) == 0xE0) {
            cp = b & 0x0F;
            len = 3;
        } else if ((b & 0xF8) == 0xF0) {
            cp = b & 0x07;
            len = 4;
        } else {
            cp = 0xFFFD;
            len = 1;
        }
        if (i + len > s.size()) {
            cp = 0xFFFD;
            len = 1;
        } else {
            for (size_t k = 1; k < len; ++k) {
                const unsigned char cc = static_cast<unsigned char>(s[i + k]);
                if ((cc & 0xC0) != 0x80) {
                    cp = 0xFFFD;
                    len = 1;
                    break;
                }
                cp = (cp << 6) | (cc & 0x3F);
            }
        }
        if (cp <= 0xFFFF) {
            out.push_back(static_cast<wchar_t>(cp));
        } else {
            cp -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 | (cp >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 | (cp & 0x3FF)));
        }
        i += len;
    }
    return out;
}

size_t Utf8Size(std::wstring_view w) {
    return ToUtf8(w).size();
}

bool IsHighSurrogate(wchar_t c) { return c >= 0xD800 && c <= 0xDBFF; }
bool IsLowSurrogate(wchar_t c) { return c >= 0xDC00 && c <= 0xDFFF; }

bool IsWellFormedUtf16(std::wstring_view w) {
    for (size_t i = 0; i < w.size(); ++i) {
        const wchar_t c = w[i];
        if (IsHighSurrogate(c)) {
            if (i + 1 >= w.size() || !IsLowSurrogate(w[i + 1])) {
                return false;
            }
            ++i;
        } else if (IsLowSurrogate(c)) {
            return false;
        }
    }
    return true;
}

uint32_t CodePointAt(std::wstring_view w, size_t offset) {
    if (offset >= w.size()) {
        return 0;
    }
    const wchar_t c = w[offset];
    if (IsHighSurrogate(c)) {
        if (offset + 1 < w.size() && IsLowSurrogate(w[offset + 1])) {
            return 0x10000 + ((static_cast<uint32_t>(c) - 0xD800) << 10) +
                   (static_cast<uint32_t>(w[offset + 1]) - 0xDC00);
        }
        return 0;
    }
    return static_cast<uint32_t>(c);
}

size_t CodePointLength(std::wstring_view w, size_t offset) {
    if (offset >= w.size()) {
        return 0;
    }
    return (IsHighSurrogate(w[offset]) && offset + 1 < w.size() && IsLowSurrogate(w[offset + 1])) ? 2 : 1;
}

} // namespace aiie::utf