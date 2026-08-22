#include "mini_test.h"

#include "Core/Utf.h"

using namespace aiie;

TEST(utf16_to_utf8_ascii) {
    CHECK_EQ(utf::ToUtf8(L"hello"), std::string("hello"));
}

TEST(utf16_to_utf8_accented) {
    CHECK_EQ(utf::ToUtf8(L"\u00e1\u00e9\u00ed\u00f3\u00fa\u00f1"),
             std::string("\xC3\xA1\xC3\xA9\xC3\xAD\xC3\xB3\xC3\xBA\xC3\xB1"));
}

TEST(utf16_to_utf8_emoji_surrogate_pair) {
    // U+1F600 😀 → F0 9F 98 80
    const std::string s = utf::ToUtf8(L"\U0001F600");
    CHECK_EQ(s.size(), 4u);
    CHECK_EQ(static_cast<unsigned char>(s[0]), 0xF0);
    CHECK_EQ(static_cast<unsigned char>(s[1]), 0x9F);
    CHECK_EQ(static_cast<unsigned char>(s[2]), 0x98);
    CHECK_EQ(static_cast<unsigned char>(s[3]), 0x80);
}

TEST(utf8_to_utf16_roundtrip) {
    const std::wstring original = L"H\u00e9llo \u4e16\u754c \U0001F600 fin";
    const std::string utf8 = utf::ToUtf8(original);
    const std::wstring back = utf::FromUtf8(utf8);
    CHECK_STREQ(back, original);
}

TEST(utf8_invalid_bytes_become_replacement_char) {
    const std::wstring s = utf::FromUtf8(std::string("\xC3", 1)); // truncated sequence
    CHECK_STREQ(s, L"\uFFFD");
}

TEST(utf16_wellformedness) {
    CHECK(utf::IsWellFormedUtf16(L"normal \u00e1"));
    CHECK(!utf::IsWellFormedUtf16(L"\xD800"));
    CHECK(!utf::IsWellFormedUtf16(L"\xDC00"));
    CHECK(!utf::IsWellFormedUtf16(L"a\xD800\xD800" L"b"));
    CHECK(utf::IsWellFormedUtf16(L"a\xD800\xDC00" L"b"));
}

TEST(utf16_codepoints) {
    CHECK_EQ(utf::CodePointAt(L"ab", 0), static_cast<uint32_t>(L'a'));
    CHECK_EQ(utf::CodePointAt(L"\U0001F600x", 0), 0x1F600u);
    CHECK_EQ(utf::CodePointLength(L"\U0001F600x", 0), 2u);
    CHECK_EQ(utf::CodePointLength(L"ax", 0), 1u);
    CHECK_EQ(utf::CodePointAt(L"", 0), 0u);
}