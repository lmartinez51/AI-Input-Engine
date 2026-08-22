#include "mini_test.h"

#include "Core/Hotkey.h"

using namespace aiie;

TEST(hotkey_parse_basic) {
    Hotkey hk;
    CHECK(ParseHotkey(L"Ctrl+Alt+Space", hk));
    CHECK_EQ(hk.modifiers, MOD_CONTROL | MOD_ALT);
    CHECK_EQ(hk.vk, VK_SPACE);
}

TEST(hotkey_parse_letter) {
    Hotkey hk;
    CHECK(ParseHotkey(L"ctrl+shift+s", hk));
    CHECK_EQ(hk.modifiers, MOD_CONTROL | MOD_SHIFT);
    CHECK_EQ(hk.vk, L'S');
}

TEST(hotkey_parse_single_key) {
    Hotkey hk;
    CHECK(ParseHotkey(L"F5", hk));
    CHECK_EQ(hk.modifiers, 0u);
    CHECK_EQ(hk.vk, VK_F5);
    CHECK(ParseHotkey(L"Enter", hk));
    CHECK_EQ(hk.vk, VK_RETURN);
}

TEST(hotkey_parse_invalid) {
    Hotkey hk;
    std::wstring err;
    CHECK(!ParseHotkey(L"", hk, &err));
    CHECK(!ParseHotkey(L"Ctrl+", hk, &err));
    CHECK(!ParseHotkey(L"NotAKey", hk, &err));
    CHECK(!ParseHotkey(L"Ctrl+Alt+Ctrl", hk, &err));
}

TEST(hotkey_format) {
    const Hotkey hk{MOD_CONTROL | MOD_ALT, VK_SPACE};
    const std::wstring s = FormatHotkey(hk);
    CHECK(s.find(L"Ctrl") != std::wstring::npos);
    CHECK(s.find(L"Alt") != std::wstring::npos);
    CHECK(s.find(L"Space") != std::wstring::npos);
}

TEST(hotkey_roundtrip) {
    for (const std::wstring text : {L"Ctrl+Alt+S", L"Alt+F2", L"Ctrl+Shift+Esc", L"Win+Space"}) {
        Hotkey hk;
        CHECK(ParseHotkey(text, hk));
        CHECK(hk.IsValid());
    }
}