#pragma once

#include <windows.h>

#include <string>

namespace aiie {

// A global hotkey combination. Modifiers map 1:1 to MOD_* flags.
struct Hotkey {
    UINT modifiers = 0; // MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN
    UINT vk = 0;        // virtual key code

    bool IsValid() const { return vk != 0; }
    bool operator==(const Hotkey&) const = default;
    bool operator!=(const Hotkey&) const = default;
};

// Parse a hotkey from text like "Ctrl+Alt+Space", "Ctrl+Shift+S", "Alt+I".
// Case-insensitive; the key part may be a letter, a digit, a key name from
// GetKeyNameText-compatible names (Space, Enter, Tab, F1..F24, Back, Del,
// Home, End, PgUp, PgDn, Up, Down, Left, Right, Esc), or a single printable
// character.
bool ParseHotkey(std::wstring_view text, Hotkey& out, std::wstring* error = nullptr);

// Format a hotkey as "Ctrl+Alt+Space".
std::wstring FormatHotkey(const Hotkey& hk);

} // namespace aiie