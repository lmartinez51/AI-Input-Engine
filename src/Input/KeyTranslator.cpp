#include "Input/KeyTranslator.h"

#include "Core/Logging.h"

namespace aiie {

void KeyTranslator::UpdateKeyState(DWORD vkCode, bool down) {
    // Normalize left/right modifier pairs to their generic VK: ToUnicodeEx
    // consults state_[VK_SHIFT] (0x10), not the 0xA0/0xA1 entries that real
    // (and injected) keyboards actually produce. Without this, capitalized
    // characters are dropped from the buffer.
    if (vkCode == VK_LSHIFT || vkCode == VK_RSHIFT) {
        vkCode = VK_SHIFT;
    } else if (vkCode == VK_LCONTROL || vkCode == VK_RCONTROL) {
        vkCode = VK_CONTROL;
    } else if (vkCode == VK_LMENU || vkCode == VK_RMENU) {
        vkCode = VK_MENU;
    }
    if (vkCode < 256) {
        // Preserve the high bit convention used by ToUnicodeEx.
        state_[vkCode] = static_cast<BYTE>(down ? 0x80 : 0x00);
    }
}

HKL KeyTranslator::Layout() const {
    return layout_;
}

void KeyTranslator::UpdateForeground(HWND fg, DWORD threadId) {
    if (!threadId && fg) {
        threadId = GetWindowThreadProcessId(fg, nullptr);
    }
    if (threadId != layoutThread_ || !layout_) {
        layout_ = threadId ? GetKeyboardLayout(threadId) : GetKeyboardLayout(0);
        layoutThread_ = threadId;
    }
}

std::wstring KeyTranslator::Translate(DWORD vkCode, DWORD scanCode, bool extended) {
    if (!layout_) {
        UpdateForeground(GetForegroundWindow());
    }

    wchar_t buf[8] = {};
    UINT flags = 0;
    if (extended) {
        flags |= 1; // KF_EXTENDED
    }
    const int count = ToUnicodeEx(vkCode, scanCode, state_.data(), buf, 8, flags, layout_);
    if (count <= 0) {
        // 0 = no character (dead key or unmapped). Negative = dead key state
        // consumed; the next call returns the composed character.
        return {};
    }
    return std::wstring(buf, buf + count);
}

} // namespace aiie