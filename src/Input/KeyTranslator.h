#pragma once

#include <windows.h>

#include <array>
#include <string>
#include <vector>

namespace aiie {

// Converts raw keyboard hook events into characters using the foreground
// application's keyboard layout. Maintains its own 256-entry key state array
// (updated from hook events) so that shift/ctrl/alt/capslock state is always
// accurate, and calls ToUnicodeEx on the hook thread so dead-key state stays
// in sync with the keys the application receives.
class KeyTranslator {
public:
    // Feed every keydown/keyup observed by the hook.
    void UpdateKeyState(DWORD vkCode, bool down);

    // Translate a keydown into characters (may be empty). Call in key order.
    std::wstring Translate(DWORD vkCode, DWORD scanCode, bool extended);

    // Current keyboard layout (of the foreground window if resolvable).
    HKL Layout() const;

    // Updates cached layout for foreground window/thread.
    void UpdateForeground(HWND fg, DWORD threadId = 0);

private:
    std::array<BYTE, 256> state_{};
    HKL layout_ = nullptr;
    DWORD layoutThread_ = 0;
};

} // namespace aiie