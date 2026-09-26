#pragma once

#include <windows.h>

#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "Input/HookBackend.h"

namespace aiie {

// Clipboard format and sequence validation result.
struct ClipboardCheckResult {
    bool hasText = false;
    bool hasUnsupportedNonText = false;
    DWORD sequenceNumber = 0;
};

class ClipboardHelper {
public:
    struct ModifierState {
        bool leftShift = false;
        bool rightShift = false;
        bool leftAlt = false;
        bool rightAlt = false;
        bool leftCtrl = false;
        bool rightCtrl = false;
    };

    // Builds the normalized synthetic keyboard input sequence for Ctrl+V injection:
    // 1. Temporarily releases any held Shift or Alt modifiers (with extended-key flag for right Alt).
    // 2. Synthesizes Ctrl+V keydown and keyup with synthetic signature.
    // 3. Restores any originally held Shift or Alt modifiers with matching extended-key flags.
    static std::vector<INPUT> BuildNormalizedPasteInputs(const ModifierState& modifiers);

    // Deduces modifier state from the temporary release prefix of an input sequence.
    static ModifierState DeduceModifiersFromInputs(const std::vector<INPUT>& inputs);

    // Computes best-effort recovery inputs to return keyboard state to the desired final state:
    // synthetic Ctrl/V keys released (UP) and physically held modifiers restored (DOWN),
    // if SendInput partially fails after dispatching `sentCount` inputs.
    static std::vector<INPUT> BuildRecoveryInputs(const std::vector<INPUT>& attemptedInputs,
                                                 UINT sentCount,
                                                 const ModifierState& modifiers);
    static std::vector<INPUT> BuildRecoveryInputs(const std::vector<INPUT>& attemptedInputs,
                                                 UINT sentCount);

    // Queries physical modifier key state from OS and dispatches the normalized paste sequence.
    // Performs best-effort keyboard state recovery if partial dispatch occurs.
    // Accepts an optional custom sender function for unit testing and failure verification.
    using InputSender = std::function<UINT(UINT nInputs, LPINPUT pInputs, int cbSize)>;
    static bool DispatchNormalizedPasteKeystroke(InputSender customSender = nullptr);

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
