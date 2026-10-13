#include "Core/Hotkey.h"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <format>
#include <map>
#include <sstream>
#include <vector>

namespace aiie {

namespace {

struct KeyNameEntry {
    std::wstring_view name;
    UINT vk;
};

constexpr KeyNameEntry kKeyNames[] = {
    {L"space", VK_SPACE},   {L"enter", VK_RETURN}, {L"return", VK_RETURN},
    {L"tab", VK_TAB},       {L"esc", VK_ESCAPE},   {L"escape", VK_ESCAPE},
    {L"back", VK_BACK},     {L"backspace", VK_BACK},
    {L"del", VK_DELETE},    {L"delete", VK_DELETE},
    {L"home", VK_HOME},     {L"end", VK_END},      {L"pgup", VK_PRIOR},
    {L"pgdn", VK_NEXT},     {L"up", VK_UP},        {L"down", VK_DOWN},
    {L"left", VK_LEFT},     {L"right", VK_RIGHT},
    {L"ins", VK_INSERT},    {L"insert", VK_INSERT},
    {L"printscreen", VK_SNAPSHOT}, {L"scrolllock", VK_SCROLL},
    {L"capslock", VK_CAPITAL},     {L"numlock", VK_NUMLOCK},
    {L"pause", VK_PAUSE},
};

bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (static_cast<wchar_t>(std::towlower(a[i])) != static_cast<wchar_t>(std::towlower(b[i]))) {
            return false;
        }
    }
    return true;
}

UINT FindKeyByName(std::wstring_view keyPart) {
    for (const auto& entry : kKeyNames) {
        if (EqualsIgnoreCase(keyPart, entry.name)) {
            return entry.vk;
        }
    }
    return 0;
}

std::wstring ToLower(std::wstring_view s) {
    std::wstring out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return out;
}

} // namespace

bool ParseHotkey(std::wstring_view text, Hotkey& out, std::wstring* error) {
    out = {};
    if (text.empty()) {
        if (error) *error = L"empty hotkey";
        return false;
    }

    std::wstring_view keyPart;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t pos = text.find(L'+', start);
        const size_t len = (pos == std::wstring_view::npos) ? (text.size() - start) : (pos - start);
        std::wstring_view part = text.substr(start, len);

        while (!part.empty() && (part.front() == L' ' || part.front() == L'\t')) {
            part.remove_prefix(1);
        }
        while (!part.empty() && (part.back() == L' ' || part.back() == L'\t')) {
            part.remove_suffix(1);
        }

        if (EqualsIgnoreCase(part, L"ctrl") || EqualsIgnoreCase(part, L"control")) {
            out.modifiers |= MOD_CONTROL;
        } else if (EqualsIgnoreCase(part, L"alt")) {
            out.modifiers |= MOD_ALT;
        } else if (EqualsIgnoreCase(part, L"shift")) {
            out.modifiers |= MOD_SHIFT;
        } else if (EqualsIgnoreCase(part, L"win") || EqualsIgnoreCase(part, L"cmd") || EqualsIgnoreCase(part, L"windows")) {
            out.modifiers |= MOD_WIN;
        } else if (part.empty()) {
            if (error) *error = L"empty modifier segment";
            return false;
        } else {
            if (!keyPart.empty()) {
                if (error) *error = L"multiple key segments";
                return false;
            }
            keyPart = part;
        }

        if (pos == std::wstring_view::npos) break;
        start = pos + 1;
    }

    if (keyPart.empty()) {
        if (error) *error = L"missing key";
        return false;
    }
    if (keyPart.size() == 1) {
        const wchar_t c = static_cast<wchar_t>(std::towlower(keyPart[0]));
        if (c >= L'a' && c <= L'z') {
            out.vk = static_cast<UINT>(std::towupper(c));
        } else if (c >= L'0' && c <= L'9') {
            out.vk = static_cast<UINT>(c);
        } else if (c >= L' ' && c < 0x100) {
            out.vk = static_cast<UINT>(c); // printable symbol mapped as VK
        } else {
            if (error) *error = L"unsupported key '" + std::wstring(1, c) + L"'";
            return false;
        }
    } else if (keyPart.size() >= 2 && (keyPart[0] == L'f' || keyPart[0] == L'F') && keyPart[1] >= L'1' && keyPart[1] <= L'9') {
        int n = 0;
        bool valid = true;
        for (size_t i = 1; i < keyPart.size(); ++i) {
            if (keyPart[i] >= L'0' && keyPart[i] <= L'9') {
                n = n * 10 + static_cast<int>(keyPart[i] - L'0');
            } else {
                valid = false;
                break;
            }
        }
        if (valid && n >= 1 && n <= 24) {
            out.vk = static_cast<UINT>(VK_F1 + n - 1);
        } else {
            if (error) *error = L"unsupported function key";
            return false;
        }
    } else {
        const UINT vk = FindKeyByName(keyPart);
        if (vk == 0) {
            if (error) *error = L"unknown key '" + ToLower(keyPart) + L"'";
            return false;
        }
        out.vk = vk;
    }
    return true;
}

std::wstring FormatHotkey(const Hotkey& hk) {
    std::wstring out;
    auto add = [&out](std::wstring_view s) {
        if (!out.empty()) out.append(L"+");
        out.append(s);
    };
    if (hk.modifiers & MOD_CONTROL) add(L"Ctrl");
    if (hk.modifiers & MOD_SHIFT) add(L"Shift");
    if (hk.modifiers & MOD_ALT) add(L"Alt");
    if (hk.modifiers & MOD_WIN) add(L"Win");

    wchar_t name[64] = {};
    const UINT sc = MapVirtualKeyW(hk.vk, MAPVK_VK_TO_VSC);
    const LONG lparam = static_cast<LONG>(sc) << 16;
    const int len = GetKeyNameTextW(lparam, name, 64);
    if (len > 0) {
        add(name);
    } else if (hk.vk >= L'A' && hk.vk <= L'Z') {
        add(std::wstring(1, static_cast<wchar_t>(hk.vk)));
    } else if (hk.vk >= L'0' && hk.vk <= L'9') {
        add(std::wstring(1, static_cast<wchar_t>(hk.vk)));
    } else {
        add(std::format(L"VK_{:#x}", static_cast<unsigned>(hk.vk)));
    }
    return out;
}

} // namespace aiie