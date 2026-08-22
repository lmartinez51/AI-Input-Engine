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

const std::map<std::wstring, UINT>& KeyNames() {
    static const std::map<std::wstring, UINT> names = {
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
    return names;
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
    // Split on '+'.
    std::vector<std::wstring> parts;
    std::wstring current;
    for (const wchar_t c : text) {
        if (c == L'+') {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    parts.push_back(current);

    std::wstring keyPart;
    for (std::wstring& part : parts) {
        // trim
        while (!part.empty() && (part.front() == L' ' || part.front() == L'\t')) part.erase(part.begin());
        while (!part.empty() && (part.back() == L' ' || part.back() == L'\t')) part.pop_back();
        const std::wstring lower = ToLower(part);
        if (lower == L"ctrl" || lower == L"control") {
            out.modifiers |= MOD_CONTROL;
        } else if (lower == L"alt") {
            out.modifiers |= MOD_ALT;
        } else if (lower == L"shift") {
            out.modifiers |= MOD_SHIFT;
        } else if (lower == L"win" || lower == L"cmd" || lower == L"windows") {
            out.modifiers |= MOD_WIN;
        } else if (lower.empty()) {
            if (error) *error = L"empty modifier segment";
            return false;
        } else {
            if (!keyPart.empty()) {
                if (error) *error = L"multiple key segments";
                return false;
            }
            keyPart = lower;
        }
    }

    if (keyPart.empty()) {
        if (error) *error = L"missing key";
        return false;
    }
    if (keyPart.size() == 1) {
        const wchar_t c = keyPart[0];
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
    } else if (keyPart.size() >= 2 && keyPart[0] == L'f' && keyPart[1] >= L'1' && keyPart[1] <= L'9') {
        const int n = _wtoi(keyPart.c_str() + 1);
        if (n >= 1 && n <= 24) {
            out.vk = static_cast<UINT>(VK_F1 + n - 1);
        } else {
            if (error) *error = L"unsupported function key";
            return false;
        }
    } else {
        const auto it = KeyNames().find(keyPart);
        if (it == KeyNames().end()) {
            if (error) *error = L"unknown key '" + std::wstring(keyPart) + L"'";
            return false;
        }
        out.vk = it->second;
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