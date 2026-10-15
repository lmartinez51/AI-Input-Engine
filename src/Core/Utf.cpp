#include "Core/Utf.h"

#include <windows.h>

namespace aiie::utf {

std::string ToUtf8(std::wstring_view w) {
    if (w.empty()) {
        return {};
    }
    const int len = WideCharToMultiByte(
        CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(std::string_view s) {
    if (s.empty()) {
        return {};
    }
    const int len = MultiByteToWideChar(
        CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
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