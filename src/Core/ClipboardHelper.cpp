#include "Core/ClipboardHelper.h"

#include "Core/Logging.h"

#include <thread>
#include <vector>

namespace aiie {

namespace {

bool OpenClipboardWithRetry(HWND hwndOwner, int maxRetries = 20, int sleepMs = 10) {
    for (int i = 0; i < maxRetries; ++i) {
        if (OpenClipboard(hwndOwner)) {
            return true;
        }
        if (i + 1 < maxRetries) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
        }
    }
    return false;
}

} // namespace

std::vector<INPUT> ClipboardHelper::BuildNormalizedPasteInputs(const ModifierState& modifiers) {
    struct HeldModifier {
        WORD vk;
        WORD scan;
        DWORD flags; // 0 or KEYEVENTF_EXTENDEDKEY
    };

    std::vector<HeldModifier> heldModifiers;
    if (modifiers.leftShift) {
        heldModifiers.push_back({ VK_LSHIFT, static_cast<WORD>(MapVirtualKeyW(VK_LSHIFT, MAPVK_VK_TO_VSC)), 0 });
    }
    if (modifiers.rightShift) {
        heldModifiers.push_back({ VK_RSHIFT, static_cast<WORD>(MapVirtualKeyW(VK_RSHIFT, MAPVK_VK_TO_VSC)), 0 });
    }
    if (modifiers.leftAlt) {
        heldModifiers.push_back({ VK_LMENU, static_cast<WORD>(MapVirtualKeyW(VK_LMENU, MAPVK_VK_TO_VSC)), 0 });
    }
    if (modifiers.rightAlt) {
        heldModifiers.push_back({ VK_RMENU, static_cast<WORD>(MapVirtualKeyW(VK_RMENU, MAPVK_VK_TO_VSC)), KEYEVENTF_EXTENDEDKEY });
    }

    const bool ctrlHeldInitially = (modifiers.leftCtrl || modifiers.rightCtrl);

    std::vector<INPUT> inputs;
    inputs.reserve(heldModifiers.size() * 2 + (ctrlHeldInitially ? 3 : 4));

    // 1. If any Shift or Alt key is held, temporarily lift it so destination receives standard Ctrl+V
    for (const auto& mod : heldModifiers) {
        INPUT in = {};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = mod.vk;
        in.ki.wScan = mod.scan;
        in.ki.dwFlags = mod.flags | KEYEVENTF_KEYUP;
        in.ki.dwExtraInfo = kAiieSyntheticSignature;
        inputs.push_back(in);
    }

    // 2. Synthesize Ctrl+V
    // Only synthesize Ctrl down and up if neither Ctrl key was held initially.
    // If a Ctrl key was already held by the user (left or right Ctrl), it is already down;
    // injecting an extra generic (left) Ctrl down without keyup would corrupt the state
    // when only right Ctrl was physically held.
    if (!ctrlHeldInitially) {
        INPUT in = {};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = VK_CONTROL;
        in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_CONTROL, MAPVK_VK_TO_VSC));
        in.ki.dwExtraInfo = kAiieSyntheticSignature;
        inputs.push_back(in);
    }
    {
        INPUT in = {};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = 'V';
        in.ki.wScan = static_cast<WORD>(MapVirtualKeyW('V', MAPVK_VK_TO_VSC));
        in.ki.dwExtraInfo = kAiieSyntheticSignature;
        inputs.push_back(in);
    }
    {
        INPUT in = {};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = 'V';
        in.ki.wScan = static_cast<WORD>(MapVirtualKeyW('V', MAPVK_VK_TO_VSC));
        in.ki.dwFlags = KEYEVENTF_KEYUP;
        in.ki.dwExtraInfo = kAiieSyntheticSignature;
        inputs.push_back(in);
    }
    if (!ctrlHeldInitially) {
        INPUT in = {};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = VK_CONTROL;
        in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_CONTROL, MAPVK_VK_TO_VSC));
        in.ki.dwFlags = KEYEVENTF_KEYUP;
        in.ki.dwExtraInfo = kAiieSyntheticSignature;
        inputs.push_back(in);
    }

    // 3. Restore the exact physically held modifier keys
    for (const auto& mod : heldModifiers) {
        INPUT in = {};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = mod.vk;
        in.ki.wScan = mod.scan;
        in.ki.dwFlags = mod.flags;
        in.ki.dwExtraInfo = kAiieSyntheticSignature;
        inputs.push_back(in);
    }

    return inputs;
}

ClipboardHelper::ModifierState ClipboardHelper::DeduceModifiersFromInputs(const std::vector<INPUT>& inputs) {
    ModifierState m;
    bool hasCtrlDown = false;
    for (const auto& in : inputs) {
        if (in.ki.wVk == VK_CONTROL && (in.ki.dwFlags & KEYEVENTF_KEYUP) == 0) {
            hasCtrlDown = true;
        }
        if ((in.ki.dwFlags & KEYEVENTF_KEYUP) != 0) {
            if (in.ki.wVk == VK_LSHIFT) m.leftShift = true;
            else if (in.ki.wVk == VK_RSHIFT) m.rightShift = true;
            else if (in.ki.wVk == VK_LMENU) m.leftAlt = true;
            else if (in.ki.wVk == VK_RMENU) m.rightAlt = true;
        }
    }
    if (!hasCtrlDown && !inputs.empty()) {
        m.leftCtrl = true;
    }
    return m;
}

std::vector<INPUT> ClipboardHelper::BuildRecoveryInputs(const std::vector<INPUT>& attemptedInputs,
                                                       UINT sentCount,
                                                       const ModifierState& modifiers) {
    std::vector<INPUT> recoveryInputs;
    if (sentCount == 0 || sentCount >= attemptedInputs.size()) {
        return recoveryInputs;
    }

    // Determine current state of relevant keys after dispatching attemptedInputs[0 .. sentCount - 1]
    bool lCtrlDown = modifiers.leftCtrl;
    bool rCtrlDown = modifiers.rightCtrl;
    bool vDown = false;
    bool lShiftDown = modifiers.leftShift;
    bool rShiftDown = modifiers.rightShift;
    bool lAltDown = modifiers.leftAlt;
    bool rAltDown = modifiers.rightAlt;

    for (UINT i = 0; i < sentCount; ++i) {
        const auto& in = attemptedInputs[i];
        const bool isUp = ((in.ki.dwFlags & KEYEVENTF_KEYUP) != 0);
        const bool isExtended = ((in.ki.dwFlags & KEYEVENTF_EXTENDEDKEY) != 0);
        switch (in.ki.wVk) {
            case VK_CONTROL:
                if (isExtended) {
                    rCtrlDown = !isUp;
                } else {
                    lCtrlDown = !isUp;
                }
                break;
            case VK_LCONTROL: lCtrlDown = !isUp; break;
            case VK_RCONTROL: rCtrlDown = !isUp; break;
            case 'V': vDown = !isUp; break;
            case VK_LSHIFT: lShiftDown = !isUp; break;
            case VK_RSHIFT: rShiftDown = !isUp; break;
            case VK_LMENU: lAltDown = !isUp; break;
            case VK_RMENU: rAltDown = !isUp; break;
            default: break;
        }
    }

    // Desired final states:
    // 1. Synthetic keys ('V', synthetic VK_CONTROL) must be UP if not held initially
    if (vDown) {
        INPUT in = {};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = 'V';
        in.ki.wScan = static_cast<WORD>(MapVirtualKeyW('V', MAPVK_VK_TO_VSC));
        in.ki.dwFlags = KEYEVENTF_KEYUP;
        in.ki.dwExtraInfo = kAiieSyntheticSignature;
        recoveryInputs.push_back(in);
    }
    if (!modifiers.leftCtrl && lCtrlDown) {
        INPUT in = {};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = VK_CONTROL;
        in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_CONTROL, MAPVK_VK_TO_VSC));
        in.ki.dwFlags = KEYEVENTF_KEYUP;
        in.ki.dwExtraInfo = kAiieSyntheticSignature;
        recoveryInputs.push_back(in);
    }
    if (!modifiers.rightCtrl && rCtrlDown) {
        INPUT in = {};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = VK_RCONTROL;
        in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_RCONTROL, MAPVK_VK_TO_VSC));
        in.ki.dwFlags = KEYEVENTF_KEYUP | KEYEVENTF_EXTENDEDKEY;
        in.ki.dwExtraInfo = kAiieSyntheticSignature;
        recoveryInputs.push_back(in);
    }

    // 2. Modifiers: if held initially, must end up restored (DOWN)
    if (modifiers.leftCtrl && !lCtrlDown) {
        INPUT in = {};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = VK_LCONTROL;
        in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_LCONTROL, MAPVK_VK_TO_VSC));
        in.ki.dwFlags = 0;
        in.ki.dwExtraInfo = kAiieSyntheticSignature;
        recoveryInputs.push_back(in);
    }
    if (modifiers.rightCtrl && !rCtrlDown) {
        INPUT in = {};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = VK_RCONTROL;
        in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_RCONTROL, MAPVK_VK_TO_VSC));
        in.ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
        in.ki.dwExtraInfo = kAiieSyntheticSignature;
        recoveryInputs.push_back(in);
    }
    if (modifiers.leftShift && !lShiftDown) {
        INPUT in = {};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = VK_LSHIFT;
        in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_LSHIFT, MAPVK_VK_TO_VSC));
        in.ki.dwFlags = 0;
        in.ki.dwExtraInfo = kAiieSyntheticSignature;
        recoveryInputs.push_back(in);
    }
    if (modifiers.rightShift && !rShiftDown) {
        INPUT in = {};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = VK_RSHIFT;
        in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_RSHIFT, MAPVK_VK_TO_VSC));
        in.ki.dwFlags = 0;
        in.ki.dwExtraInfo = kAiieSyntheticSignature;
        recoveryInputs.push_back(in);
    }
    if (modifiers.leftAlt && !lAltDown) {
        INPUT in = {};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = VK_LMENU;
        in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_LMENU, MAPVK_VK_TO_VSC));
        in.ki.dwFlags = 0;
        in.ki.dwExtraInfo = kAiieSyntheticSignature;
        recoveryInputs.push_back(in);
    }
    if (modifiers.rightAlt && !rAltDown) {
        INPUT in = {};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = VK_RMENU;
        in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_RMENU, MAPVK_VK_TO_VSC));
        in.ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
        in.ki.dwExtraInfo = kAiieSyntheticSignature;
        recoveryInputs.push_back(in);
    }

    // If not held initially but currently down, release
    if (!modifiers.leftShift && lShiftDown) {
        INPUT in = {};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = VK_LSHIFT;
        in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_LSHIFT, MAPVK_VK_TO_VSC));
        in.ki.dwFlags = KEYEVENTF_KEYUP;
        in.ki.dwExtraInfo = kAiieSyntheticSignature;
        recoveryInputs.push_back(in);
    }
    if (!modifiers.rightShift && rShiftDown) {
        INPUT in = {};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = VK_RSHIFT;
        in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_RSHIFT, MAPVK_VK_TO_VSC));
        in.ki.dwFlags = KEYEVENTF_KEYUP;
        in.ki.dwExtraInfo = kAiieSyntheticSignature;
        recoveryInputs.push_back(in);
    }
    if (!modifiers.leftAlt && lAltDown) {
        INPUT in = {};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = VK_LMENU;
        in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_LMENU, MAPVK_VK_TO_VSC));
        in.ki.dwFlags = KEYEVENTF_KEYUP;
        in.ki.dwExtraInfo = kAiieSyntheticSignature;
        recoveryInputs.push_back(in);
    }
    if (!modifiers.rightAlt && rAltDown) {
        INPUT in = {};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = VK_RMENU;
        in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_RMENU, MAPVK_VK_TO_VSC));
        in.ki.dwFlags = KEYEVENTF_KEYUP | KEYEVENTF_EXTENDEDKEY;
        in.ki.dwExtraInfo = kAiieSyntheticSignature;
        recoveryInputs.push_back(in);
    }

    return recoveryInputs;
}

std::vector<INPUT> ClipboardHelper::BuildRecoveryInputs(const std::vector<INPUT>& attemptedInputs,
                                                       UINT sentCount) {
    return BuildRecoveryInputs(attemptedInputs, sentCount, DeduceModifiersFromInputs(attemptedInputs));
}

bool ClipboardHelper::DispatchNormalizedPasteKeystroke(InputSender customSender) {
    ModifierState modifiers;
    modifiers.leftShift = ((GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0);
    modifiers.rightShift = ((GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0);
    modifiers.leftAlt = ((GetAsyncKeyState(VK_LMENU) & 0x8000) != 0);
    modifiers.rightAlt = ((GetAsyncKeyState(VK_RMENU) & 0x8000) != 0);
    modifiers.leftCtrl = ((GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0);
    modifiers.rightCtrl = ((GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0);
    if (!modifiers.leftCtrl && !modifiers.rightCtrl && ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0)) {
        modifiers.leftCtrl = true;
    }

    std::vector<INPUT> inputs = BuildNormalizedPasteInputs(modifiers);
    const UINT count = static_cast<UINT>(inputs.size());
    if (count == 0) {
        return false;
    }

    UINT sent = 0;
    if (customSender) {
        sent = customSender(count, inputs.data(), sizeof(INPUT));
    } else {
        sent = SendInput(count, inputs.data(), sizeof(INPUT));
    }

    if (sent < count) {
        if (sent > 0) {
            std::vector<INPUT> recovery = BuildRecoveryInputs(inputs, sent, modifiers);
            if (!recovery.empty()) {
                if (customSender) {
                    customSender(static_cast<UINT>(recovery.size()), recovery.data(), sizeof(INPUT));
                } else {
                    SendInput(static_cast<UINT>(recovery.size()), recovery.data(), sizeof(INPUT));
                }
            }
        }
        return false;
    }

    return true;
}

ClipboardCheckResult ClipboardHelper::InspectClipboard() {
    ClipboardCheckResult result;
    result.sequenceNumber = GetClipboardSequenceNumber();

    if (!OpenClipboardWithRetry(nullptr)) {
        return result;
    }

    UINT format = 0;
    while ((format = EnumClipboardFormats(format)) != 0) {
        if (format == CF_UNICODETEXT || format == CF_TEXT || format == CF_OEMTEXT) {
            result.hasText = true;
        } else if (format == CF_BITMAP || format == CF_DIB || format == CF_DIBV5 ||
                   format == CF_HDROP || format == CF_ENHMETAFILE || format == CF_WAVE ||
                   format == CF_RIFF) {
            result.hasUnsupportedNonText = true;
        }
    }

    CloseClipboard();
    return result;
}

bool ClipboardHelper::ReadUnicodeText(std::wstring& outText, DWORD& outSequence, HWND hwndOwner) {
    outSequence = GetClipboardSequenceNumber();

    if (!OpenClipboardWithRetry(hwndOwner)) {
        Logger::Instance().Warn(cat::Input, L"ReadUnicodeText: failed to open clipboard (err {})", GetLastError());
        return false;
    }

    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (!hData) {
        CloseClipboard();
        return false;
    }

    const wchar_t* pText = static_cast<const wchar_t*>(GlobalLock(hData));
    if (!pText) {
        CloseClipboard();
        return false;
    }

    outText = pText;
    GlobalUnlock(hData);
    CloseClipboard();
    return true;
}

bool ClipboardHelper::SetUnicodeText(std::wstring_view text, DWORD& outNewSequence, HWND hwndOwner) {
    if (!OpenClipboardWithRetry(hwndOwner)) {
        Logger::Instance().Warn(cat::Input, L"SetUnicodeText: failed to open clipboard (err {})", GetLastError());
        return false;
    }

    if (!EmptyClipboard()) {
        Logger::Instance().Warn(cat::Input, L"SetUnicodeText: failed to empty clipboard (err {})", GetLastError());
        CloseClipboard();
        return false;
    }

    const size_t bytesNeeded = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytesNeeded);
    if (!hMem) {
        CloseClipboard();
        return false;
    }

    wchar_t* pDest = static_cast<wchar_t*>(GlobalLock(hMem));
    if (!pDest) {
        GlobalFree(hMem);
        CloseClipboard();
        return false;
    }

    memcpy(pDest, text.data(), text.size() * sizeof(wchar_t));
    pDest[text.size()] = L'\0';
    GlobalUnlock(hMem);

    if (!SetClipboardData(CF_UNICODETEXT, hMem)) {
        GlobalFree(hMem);
        CloseClipboard();
        return false;
    }

    CloseClipboard();
    outNewSequence = GetClipboardSequenceNumber();
    return true;
}

bool ClipboardHelper::RestoreIfSequenceMatches(DWORD expectedSequence, std::wstring_view originalText,
                                              HWND hwndOwner) {
    const DWORD currentSeq = GetClipboardSequenceNumber();
    if (currentSeq != expectedSequence) {
        Logger::Instance().Info(cat::Input,
                               L"Clipboard sequence changed externally (expected {}, current {}); skipping restore",
                               expectedSequence, currentSeq);
        return false;
    }

    DWORD restoredSeq = 0;
    return SetUnicodeText(originalText, restoredSeq, hwndOwner);
}

bool ClipboardHelper::ExecuteSafePasteAndRestore(HWND targetHwnd,
                                                std::wstring_view transformedText,
                                                std::wstring_view originalText,
                                                DWORD origSequence,
                                                std::chrono::milliseconds timeout) {
    const DWORD currentSeq = GetClipboardSequenceNumber();
    if (currentSeq != origSequence) {
        Logger::Instance().Warn(cat::Input,
                                L"Smart Paste aborted: clipboard sequence changed prior to paste (expected {}, current {})",
                                origSequence, currentSeq);
        return false;
    }

    // Fast-path: If transformedText == originalText, clipboard already contains the exact text.
    // No temporary clipboard overwrite or sequence mutation needed.
    if (transformedText == originalText) {
        if (targetHwnd && targetHwnd != GetDesktopWindow()) {
            const HWND currentFg = GetForegroundWindow();
            if (currentFg && currentFg != targetHwnd) {
                Logger::Instance().Warn(cat::Input, L"Smart Paste: target window lost foreground before paste");
                return false;
            }
        }

        if (!DispatchNormalizedPasteKeystroke()) {
            Logger::Instance().Error(cat::Input, L"Smart Paste: verbatim SendInput(Ctrl+V) failed (err {})", GetLastError());
            return false;
        }

        Logger::Instance().Info(cat::Input, L"Smart Paste: verbatim paste executed without clipboard mutation");
        return true;
    }

    // Step 1: Install temporary transformed text
    DWORD tempSequence = 0;
    if (!SetUnicodeText(transformedText, tempSequence, nullptr)) {
        Logger::Instance().Error(cat::Input, L"Smart Paste: failed to set temporary clipboard text");
        return false;
    }

    // Step 2: Ensure target window is still foreground
    if (targetHwnd && targetHwnd != GetDesktopWindow()) {
        const HWND currentFg = GetForegroundWindow();
        if (currentFg && currentFg != targetHwnd) {
            Logger::Instance().Warn(cat::Input, L"Smart Paste: target window lost foreground before paste");
            // Restore original clipboard immediately
            RestoreIfSequenceMatches(tempSequence, originalText, nullptr);
            return false;
        }
    }

    // Step 3: Synthesize hardware Ctrl+V keystroke with normalized modifiers
    if (!DispatchNormalizedPasteKeystroke()) {
        Logger::Instance().Error(cat::Input, L"Smart Paste: SendInput(Ctrl+V) failed (err {})", GetLastError());
        RestoreIfSequenceMatches(tempSequence, originalText, nullptr);
        return false;
    }

    // Step 4: Adaptive synchronization wait.
    // Allow the target application message loop to receive WM_KEYDOWN/WM_PASTE and
    // fetch the clipboard data before we restore the original snapshot.
    const auto tStart = std::chrono::steady_clock::now();
    const auto maxWait = std::min(timeout, std::chrono::milliseconds(50));
    const auto tDeadline = tStart + maxWait;

    // Adaptive micro-polling yield loop to allow OS input queue dispatch while checking for clipboard sequence changes.
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    while (std::chrono::steady_clock::now() < tDeadline) {
        if (GetClipboardSequenceNumber() != tempSequence) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Step 5: Restore original clipboard IF sequence still matches temporary sequence
    const bool restored = RestoreIfSequenceMatches(tempSequence, originalText, nullptr);
    Logger::Instance().Info(cat::Input, L"Smart Paste: paste executed and clipboard restore result: {}", restored);

    return true;
}

} // namespace aiie
