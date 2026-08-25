#include "Core/ClipboardHelper.h"

#include "Core/Logging.h"

#include <thread>
#include <vector>

namespace aiie {

namespace {

bool OpenClipboardWithRetry(HWND hwndOwner, int maxRetries = 5, int sleepMs = 10) {
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

    // Step 3: Synthesize hardware Ctrl+V keystroke
    constexpr ULONG_PTR kAiieSyntheticSignature = 0x41494945;
    INPUT inputs[4] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL;
    inputs[0].ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_CONTROL, MAPVK_VK_TO_VSC));
    inputs[0].ki.dwExtraInfo = kAiieSyntheticSignature;

    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = 'V';
    inputs[1].ki.wScan = static_cast<WORD>(MapVirtualKeyW('V', MAPVK_VK_TO_VSC));
    inputs[1].ki.dwExtraInfo = kAiieSyntheticSignature;

    inputs[2].type = INPUT_KEYBOARD;
    inputs[2].ki.wVk = 'V';
    inputs[2].ki.wScan = static_cast<WORD>(MapVirtualKeyW('V', MAPVK_VK_TO_VSC));
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[2].ki.dwExtraInfo = kAiieSyntheticSignature;

    inputs[3].type = INPUT_KEYBOARD;
    inputs[3].ki.wVk = VK_CONTROL;
    inputs[3].ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_CONTROL, MAPVK_VK_TO_VSC));
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[3].ki.dwExtraInfo = kAiieSyntheticSignature;

    const UINT sent = SendInput(4, inputs, sizeof(INPUT));
    if (sent != 4) {
        Logger::Instance().Error(cat::Input, L"Smart Paste: SendInput(Ctrl+V) failed (err {})", GetLastError());
        RestoreIfSequenceMatches(tempSequence, originalText, nullptr);
        return false;
    }

    // Step 4: Bounded synchronization wait.
    // Allow the target application message loop to receive WM_KEYDOWN/WM_PASTE and
    // fetch the clipboard data before we restore the original snapshot.
    const auto tStart = std::chrono::steady_clock::now();
    const auto tDeadline = tStart + timeout;

    // Small initial yield to let the target OS input pipeline dispatch the paste
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Step 5: Restore original clipboard IF sequence still matches temporary sequence
    const bool restored = RestoreIfSequenceMatches(tempSequence, originalText, nullptr);
    Logger::Instance().Info(cat::Input, L"Smart Paste: paste executed and clipboard restore result: {}", restored);

    return true;
}

} // namespace aiie
