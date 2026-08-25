#pragma once

#include <windows.h>

#include <chrono>
#include <string>
#include <string_view>

namespace aiie {

// Clipboard format and sequence validation result.
struct ClipboardCheckResult {
    bool hasText = false;
    bool hasUnsupportedNonText = false;
    DWORD sequenceNumber = 0;
};

class ClipboardHelper {
public:
    // Checks the current clipboard state and format composition.
    // In v1, Smart Paste only proceeds if the clipboard contains text and
    // does not contain complex non-text formats (images, files, OLE streams)
    // that would be corrupted by text replacement.
    static ClipboardCheckResult InspectClipboard();

    // Reads Unicode text from the clipboard and returns the clipboard sequence number.
    static bool ReadUnicodeText(std::wstring& outText, DWORD& outSequence, HWND hwndOwner = nullptr);

    // Sets Unicode text on the clipboard and returns the new sequence number.
    static bool SetUnicodeText(std::wstring_view text, DWORD& outNewSequence, HWND hwndOwner = nullptr);

    // Restores original text to the clipboard IF AND ONLY IF the current clipboard
    // sequence number matches `expectedSequence`. If the user or another app modified
    // the clipboard in the interim, restoration is safely aborted to avoid clobbering.
    static bool RestoreIfSequenceMatches(DWORD expectedSequence, std::wstring_view originalText,
                                         HWND hwndOwner = nullptr);

    // High-level safe paste workflow:
    // 1. Verifies clipboard sequence == origSequence.
    // 2. Installs temporary transformed text.
    // 3. Synthesizes Ctrl+V input event directed at target.
    // 4. Bounded synchronization wait for target application to read clipboard.
    // 5. Restores originalText if sequence remains unmodified.
    static bool ExecuteSafePasteAndRestore(HWND targetHwnd,
                                          std::wstring_view transformedText,
                                          std::wstring_view originalText,
                                          DWORD origSequence,
                                          std::chrono::milliseconds timeout = std::chrono::milliseconds(1500));
};

} // namespace aiie
