#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace aiie::utf {

// UTF-16 (wchar_t) <-> UTF-8 conversions. Windows APIs are UTF-16; JSON and
// the network layer are UTF-8. All conversions validate where cheap to do so.

std::string ToUtf8(std::wstring_view w);
std::wstring FromUtf8(std::string_view s);

// Length of the UTF-8 encoding of a single UTF-16 code unit pair.
size_t Utf8Size(std::wstring_view w);

// True if the wstring_view contains well-formed UTF-16 (no orphan surrogates).
bool IsWellFormedUtf16(std::wstring_view w);

// Unicode helpers operating on UTF-16 code points.
bool IsHighSurrogate(wchar_t c);
bool IsLowSurrogate(wchar_t c);
uint32_t CodePointAt(std::wstring_view w, size_t offset);   // full code point or L'\0' if invalid
size_t CodePointLength(std::wstring_view w, size_t offset); // 1 or 2 (units)

} // namespace aiie::utf