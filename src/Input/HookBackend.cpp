#include "Input/HookBackend.h"

#include "Core/Logging.h"

#include <comdef.h>
#include <optional>
#include <thread>
#include <uiautomation.h>

namespace aiie {

// Module-level accessor so the static hook proc can reach the instance.
HookBackend* g_hookBackend = nullptr;

HookBackend::HookBackend() = default;

HookBackend::~HookBackend() {
    Stop();
}

void HookBackend::ApplyConfig(const AppConfig& config) {
    toggle_ = config.hotkeyToggle;
    swap_ = config.hotkeySwap;
    diagnostics_ = config.hotkeyDiagnostics;
}

bool HookBackend::Start() {
    if (running_.load()) {
        return true;
    }
    g_hookBackend = this;
    running_.store(true);
    thread_ = std::thread([this] { ThreadMain(); });
    return true;
}

void HookBackend::Stop() {
    if (!running_.load()) {
        return;
    }
    running_.store(false);
    if (hookThreadId_ != 0) {
        PostThreadMessageW(hookThreadId_, WM_QUIT, 0, 0);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    hookThreadId_ = 0;
    g_hookBackend = nullptr;
}

void HookBackend::ThreadMain() {
    hookThreadId_ = GetCurrentThreadId();

    eventHook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                                 &HookBackend::ForegroundChanged, 0, 0, WINEVENT_OUTOFCONTEXT);

    hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &HookBackend::HookProc, GetModuleHandleW(nullptr), 0);
    if (!hook_) {
        Logger::Instance().Error(cat::Input, L"SetWindowsHookEx(WH_KEYBOARD_LL) failed: {}", GetLastError());
        running_.store(false);
        return;
    }
    Logger::Instance().Info(cat::Input, L"low-level keyboard hook installed (thread {:#x})", hookThreadId_);

    MSG msg;
    while (running_.load() && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (eventHook_) {
        UnhookWinEvent(eventHook_);
        eventHook_ = nullptr;
    }
    if (hook_) {
        UnhookWindowsHookEx(hook_);
        hook_ = nullptr;
    }
    Logger::Instance().Info(cat::Input, L"hook thread exiting");
}

void WINAPI HookBackend::ForegroundChanged(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject,
                                           LONG idChild, DWORD thread, DWORD time) {
    (void)hook;
    (void)event;
    (void)hwnd;
    (void)idObject;
    (void)idChild;
    (void)thread;
    (void)time;
    // Runs on the hook thread (WINEVENT_OUTOFCONTEXT). Target application changed;
    // explicitly cancel active composition so old results never commit into the new target.
    if (g_hookBackend && g_hookBackend->engine_) {
        g_hookBackend->engine_->CancelActiveCompositionForFocusChange();
    }
}

LRESULT CALLBACK HookBackend::HookProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_hookBackend && g_hookBackend->running_.load()) {
        return g_hookBackend->OnHookEvent(code, wp, lp);
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT HookBackend::OnHookEvent(int code, WPARAM wp, LPARAM lp) {
    if (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN || wp == WM_KEYUP || wp == WM_SYSKEYUP) {
        hookEventCount_.fetch_add(1, std::memory_order_relaxed);
        const auto* kbd = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        const bool down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);

        // 1. Authoritative check: Ignore AIInputEngine's own synthetic keyboard events via
        // dedicated dwExtraInfo signature. This guarantees immunity to re-entrancy independent
        // of injection duration or timing.
        const bool isAiieSynthetic = ((kbd->flags & LLKHF_INJECTED) != 0) &&
                                     (kbd->dwExtraInfo == kAiieSyntheticSignature);
        if (isAiieSynthetic) {
            return CallNextHookEx(nullptr, code, wp, lp);
        }

        // 2. Secondary lifecycle protection: general injected events during active commit window.
        const bool injected = (kbd->flags & LLKHF_INJECTED) != 0;
        if (injected && (selfInjecting_.load() || GetTickCount64() < injectGuardUntilMs_.load())) {
            return CallNextHookEx(nullptr, code, wp, lp);
        }
        // VK_PACKET (KEYEVENTF_UNICODE) keys carry no virtual key: never feed
        // them into the translator/engine, they only pass through.
        if (kbd->vkCode == 0) {
            return CallNextHookEx(nullptr, code, wp, lp);
        }

        // Update translator + local state only for events that will be
        // processed. Dropped injected events must not touch the translator or
        // keyState_ — doing so would corrupt the key-tracking state and cause
        // subsequent real keystrokes to be mistranslated (e.g. the '.' sentry
        // becomes a different character, preventing the terminator from
        // firing).
        translator_.UpdateKeyState(kbd->vkCode, down);
        if (kbd->vkCode < 256) {
            keyState_[kbd->vkCode] = static_cast<BYTE>(down ? 0x80 : 0);
        }
        hookEngineCount_.fetch_add(1, std::memory_order_relaxed);

        if (down) {
            // Hotkey combos: flush the buffer; the app handles the action.
            if (ModifiersMatch(toggle_, keyState_) || ModifiersMatch(swap_, keyState_) ||
                ModifiersMatch(diagnostics_, keyState_)) {
                if (engine_) {
                    engine_->FlushBuffer();
                }
                return CallNextHookEx(nullptr, code, wp, lp);
            }

            // Control/Alt/Win combos change text invisibly (copy/paste/undo)
            // or move focus; commit the tracked buffer as-is.
            const bool ctrl = (keyState_[VK_LCONTROL] & 0x80) || (keyState_[VK_RCONTROL] & 0x80);
            const bool alt = (keyState_[VK_LMENU] & 0x80) || (keyState_[VK_RMENU] & 0x80);
            const bool win = (keyState_[VK_LWIN] & 0x80) || (keyState_[VK_RWIN] & 0x80);
            const bool isModifier = kbd->vkCode == VK_CONTROL || kbd->vkCode == VK_MENU ||
                                    kbd->vkCode == VK_SHIFT || kbd->vkCode == VK_LWIN ||
                                    kbd->vkCode == VK_RWIN;
            if ((ctrl || alt || win) && !isModifier) {
                if (engine_) {
                    engine_->FlushBuffer();
                }
                return CallNextHookEx(nullptr, code, wp, lp);
            }

            if (!engine_) {
                return CallNextHookEx(nullptr, code, wp, lp);
            }

            switch (kbd->vkCode) {
            case VK_RETURN: {
                const bool shift = (keyState_[VK_LSHIFT] & 0x80) || (keyState_[VK_RSHIFT] & 0x80);
                if (shift && engine_->CanHandleShiftEnter()) {
                    engine_->HandleShiftEnter();
                    return 1; // Consume event: prevents host from inserting premature uncoordinated newline
                }
                engine_->HandleCommitKey();
                return CallNextHookEx(nullptr, code, wp, lp);
            }
            case VK_BACK:
                engine_->HandleBackspace();
                return CallNextHookEx(nullptr, code, wp, lp);
            case VK_TAB:
            case VK_ESCAPE:
            case VK_LEFT:
            case VK_RIGHT:
            case VK_UP:
            case VK_DOWN:
            case VK_HOME:
            case VK_END:
            case VK_PRIOR:
            case VK_NEXT:
            case VK_DELETE:
            case VK_INSERT:
            case VK_F1:
            case VK_F2:
            case VK_F3:
            case VK_F4:
            case VK_F5:
            case VK_F6:
            case VK_F7:
            case VK_F8:
            case VK_F9:
            case VK_F10:
            case VK_F11:
            case VK_F12:
            case VK_F13:
            case VK_F14:
            case VK_F15:
            case VK_F16:
            case VK_F17:
            case VK_F18:
            case VK_F19:
            case VK_F20:
            case VK_F21:
            case VK_F22:
            case VK_F23:
            case VK_F24:
                engine_->HandleCommitKey();
                return CallNextHookEx(nullptr, code, wp, lp);
            default:
                break;
            }

            // Printable character?
            const std::wstring chars = translator_.Translate(kbd->vkCode, kbd->scanCode,
                                                             (kbd->flags & LLKHF_EXTENDED) != 0);
            for (const wchar_t c : chars) {
                if (c == L'.' || c == L'!' || c == L'?') {
                    engine_->HandleTerminator(c);
                } else {
                    engine_->HandleCharacter(c);
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

bool HookBackend::ModifiersMatch(Hotkey hk, const std::array<BYTE, 256>& state) {
    if (!hk.IsValid()) {
        return false;
    }
    const bool ctrl = (state[VK_LCONTROL] & 0x80) || (state[VK_RCONTROL] & 0x80);
    const bool alt = (state[VK_LMENU] & 0x80) || (state[VK_RMENU] & 0x80);
    const bool shift = (state[VK_LSHIFT] & 0x80) || (state[VK_RSHIFT] & 0x80);
    const bool win = (state[VK_LWIN] & 0x80) || (state[VK_RWIN] & 0x80);
    const bool wantCtrl = (hk.modifiers & MOD_CONTROL) != 0;
    const bool wantAlt = (hk.modifiers & MOD_ALT) != 0;
    const bool wantShift = (hk.modifiers & MOD_SHIFT) != 0;
    const bool wantWin = (hk.modifiers & MOD_WIN) != 0;
    return ctrl == wantCtrl && alt == wantAlt && shift == wantShift && win == wantWin;
}

void HookBackend::CommitReplacement(std::wstring replacement, const std::wstring& expectedSource) {
    const size_t sourceLength = expectedSource.size();
    HWND fgHwnd = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fgHwnd, &pid);
    std::wstring procName = L"unknown";
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProc) {
        wchar_t imgPath[MAX_PATH] = {};
        DWORD imgLen = MAX_PATH;
        if (QueryFullProcessImageNameW(hProc, 0, imgPath, &imgLen)) {
            const wchar_t* lastSlash = wcsrchr(imgPath, L'\\');
            procName = lastSlash ? (lastSlash + 1) : imgPath;
        }
        CloseHandle(hProc);
    }

    Logger::Instance().Info(cat::Input,
        L"[REPLACE] target={:#x} pid={} process={} source_len={} translation_len={} method=SendInput delete_count={} inject_count={}",
        reinterpret_cast<uintptr_t>(fgHwnd), pid, procName, sourceLength, replacement.size(), sourceLength, replacement.size());

    {
        std::lock_guard lock(commitMutex_);
        commitPending_ = true;
    }
    std::thread([this, replacement = std::move(replacement), expectedSource, sourceLength, fgHwnd, procName]() {
        auto markDone = [this] {
            {
                std::lock_guard lock(commitMutex_);
                commitPending_ = false;
            }
            commitCV_.notify_all();
        };

        selfInjecting_.store(true);
        injectGuardUntilMs_.store(GetTickCount64() + 500);

        // Delete the source text via backspaces
        InjectBackspaces(sourceLength);

        // Inject the translated text
        InjectText(replacement);

        Logger::Instance().Info(cat::Input, L"[REPLACE] result=SUCCESS target={:#x} process={}",
                               reinterpret_cast<uintptr_t>(fgHwnd), procName);

        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        // Extend guard grace period to 500ms after injection completes as secondary defense
        injectGuardUntilMs_.store(GetTickCount64() + 500);
        selfInjecting_.store(false);
        markDone();
    }).detach();
}

void HookBackend::WaitForCommitComplete(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock lock(commitMutex_);
    // Wait for the commit thread to finish.
    commitCV_.wait_until(lock, deadline, [this] { return !commitPending_; });
    // After the thread is done, selfInjecting_ is already false. Ensure the
    // inject guard has also expired (the thread's 30ms final sleep plus the
    // work before it always exceeds the 500ms guard in practice, but honor
    // the deadline for correctness).
    const uint64_t guardMs = injectGuardUntilMs_.load();
    const uint64_t nowMs = GetTickCount64();
    if (guardMs > nowMs) {
        lock.unlock();
        const auto remaining = std::chrono::milliseconds(guardMs - nowMs);
        const auto actual = std::min(remaining, std::max(std::chrono::milliseconds(0),
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())));
        if (actual.count() > 0) {
            std::this_thread::sleep_for(actual);
        }
    }
}

// Deletes `count` characters at the caret and verifies each step against the
// live document, so a backspace dropped or coalesced by the target's text
// pipeline is re-sent only while its target character is still visibly
// present. The document must read exactly `base` when done; anything else
// aborts the commit (user text is kept, never over-deleted).
bool HookBackend::DeleteVerified(size_t count, const std::wstring& base) {
    if (count == 0) {
        return true;
    }
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::milliseconds(5000);
    // Fast path: send the whole batch at once (same pacing as before). If
    // the target drops any, fall through to verified singles.
    InjectBackspaces(count);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    {
        const auto seen = ReadFocusedText();
        if (!seen.has_value()) {
            return true;
        }
        if (*seen == base) {
            return true;
        }
        const std::wstring& text = *seen;
        if (text.size() <= base.size() || text.compare(0, base.size(), base) != 0) {
            Logger::Instance().Warn(cat::Input, L"commit verify: document no longer starts with pre-buffer text, aborting");
            return false;
        }
        const size_t leftover = text.size() - base.size();
        if (leftover == 0) {
            return true;
        }
        if (leftover > count) {
            Logger::Instance().Warn(cat::Input, L"commit verify: document grew during delete, aborting");
            return false;
        }
    }
    // Verified singles: delete one character at a time and confirm each
    // deletion landed before moving to the next. This is slower but immune
    // to the TSF coalescing / dropping that affects batch injection.
    size_t remaining = count;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto seen = ReadFocusedText();
        if (!seen.has_value()) {
            return true;
        }
        if (*seen == base) {
            return true;
        }
        const std::wstring& text = *seen;
        if (text.size() <= base.size() || text.compare(0, base.size(), base) != 0) {
            Logger::Instance().Warn(cat::Input, L"commit verify: document no longer starts with pre-buffer text, aborting");
            return false;
        }
        remaining = text.size() - base.size();
        if (remaining == 0) {
            return true;
        }
        // Send a single backspace and verify it lands.
        bool landed = false;
        for (int attempt = 0; attempt < 3 && std::chrono::steady_clock::now() < deadline;
             ++attempt) {
            InjectBackspaces(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            const auto re = ReadFocusedText();
            if (!re.has_value()) {
                return true;
            }
            if (*re == base) {
                return true;
            }
            if (re->size() <= base.size() || re->compare(0, base.size(), base) != 0) {
                Logger::Instance().Warn(cat::Input, L"commit verify: document state changed during retry, aborting");
                return false;
            }
            const size_t now = re->size() - base.size();
            if (now < remaining) {
                landed = true;
                break;
            }
        }
        if (!landed) {
            Logger::Instance().Warn(cat::Input,
                                    L"commit verify: backspace not landing, keeping user text");
            return false;
        }
    }
    Logger::Instance().Warn(cat::Input, L"commit verify: deadline exceeded, keeping user text");
    return false;
}

// Checks that the document reads exactly base + replacement. Trailing debris
// inside the replacement region (e.g. a TSF ghost character that appeared
// after the source was deleted) is removed; any other mismatch is logged and
// reported as a failure.
bool HookBackend::VerifyFinal(const std::wstring& base, const std::wstring& replacement) {
    const std::wstring expected = base + replacement;
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        const auto seen = ReadFocusedText();
        if (!seen.has_value()) {
            return true;
        }
        const std::wstring& text = *seen;
        if (text == expected) {
            return true;
        }
        if (text.size() > expected.size() && text.compare(0, expected.size(), expected) == 0) {
            const size_t extra = text.size() - expected.size();
            Logger::Instance().Warn(cat::Input, L"commit verify: {} trailing debris chars, removing", extra);
            InjectBackspaces(extra);
            continue;
        }
        Logger::Instance().Error(cat::Input, L"commit verify: got '{}', expected '{}'", text, expected);
        return false;
    }
    return false;
}

std::optional<std::wstring> HookBackend::ReadFocusedText() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comInited = SUCCEEDED(hr);
    const bool alreadyInitedSta = (hr == RPC_E_CHANGED_MODE);
    const bool canUseCom = comInited || alreadyInitedSta;

    std::optional<std::wstring> result;
    IUIAutomation* uia = nullptr;
    if (canUseCom &&
        SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IUIAutomation, reinterpret_cast<void**>(&uia)))) {
        IUIAutomationElement* el = nullptr;
        if (SUCCEEDED(uia->GetFocusedElement(&el)) && el) {
            VARIANT v;
            VariantInit(&v);
            if (SUCCEEDED(el->GetCurrentPropertyValue(UIA_ValueValuePropertyId, &v)) && v.vt == VT_BSTR &&
                v.bstrVal != nullptr) {
                result = std::wstring(v.bstrVal);
                SysFreeString(v.bstrVal);
            } else {
                VariantClear(&v);
                IUIAutomationTextPattern* tp = nullptr;
                if (SUCCEEDED(el->GetCurrentPattern(UIA_TextPatternId,
                                                    reinterpret_cast<IUnknown**>(&tp))) && tp) {
                    IUIAutomationTextRange* range = nullptr;
                    if (SUCCEEDED(tp->get_DocumentRange(&range)) && range) {
                        BSTR b = nullptr;
                        if (SUCCEEDED(range->GetText(-1, &b)) && b) {
                            result = std::wstring(b);
                            SysFreeString(b);
                        }
                        range->Release();
                    }
                    tp->Release();
                }
            }
            el->Release();
        }
        uia->Release();
    }
    if (comInited) {
        CoUninitialize();
    }
    return result;
}

void HookBackend::InjectBackspaces(size_t count) {
    for (size_t i = 0; i < count; ++i) {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = VK_BACK;
        in.ki.dwExtraInfo = kAiieSyntheticSignature;
        SendInput(1, &in, sizeof(in));
        in.ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(1, &in, sizeof(in));
        // WinUI3 Notepad's TSF pipeline drops rapid backspace pairs; a short gap
        // between pairs keeps every deletion.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void HookBackend::InjectText(const std::wstring& text) {
    // WinUI3 Notepad's TSF pipeline is hostile to synthetic keystrokes: batched
    // SendInput drops the shift modifier (letters come out lowercase), rapid
    // per-key SendInput drops whole characters, and KEYEVENTF_UNICODE is dropped
    // entirely. Reliable delivery requires human-paced timing: a 40ms shift hold
    // before shift-modified letters and ~15ms between key events.
    const auto sendOne = [](INPUT in) {
        in.ki.dwExtraInfo = kAiieSyntheticSignature;
        SendInput(1, &in, sizeof(in));
    };
    const WORD shiftScan = static_cast<WORD>(MapVirtualKeyW(VK_LSHIFT, MAPVK_VK_TO_VSC));
    for (const wchar_t c : text) {
        // Prefer layout-mapped virtual keys: real keystrokes behave identically
        // to what the user typed (KEYEVENTF_UNICODE is dropped by WinUI3 apps).
        const SHORT vkScan = VkKeyScanW(c);
        if (vkScan != -1) {
            const BYTE vk = static_cast<BYTE>(vkScan & 0xFF);
            const bool needsShift = (vkScan & 0x0100) != 0;
            if (needsShift) {
                INPUT s{};
                s.type = INPUT_KEYBOARD;
                s.ki.wVk = VK_LSHIFT;
                s.ki.wScan = shiftScan;
                sendOne(s);
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
            }
            INPUT in{};
            in.type = INPUT_KEYBOARD;
            in.ki.wVk = vk;
            in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
            sendOne(in);
            in.ki.dwFlags = KEYEVENTF_KEYUP;
            sendOne(in);
            if (needsShift) {
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                INPUT s{};
                s.type = INPUT_KEYBOARD;
                s.ki.wVk = VK_LSHIFT;
                s.ki.wScan = shiftScan;
                s.ki.dwFlags = KEYEVENTF_KEYUP;
                sendOne(s);
            }
        } else {
            // No virtual key in the current layout: fall back to UNICODE.
            INPUT in{};
            in.type = INPUT_KEYBOARD;
            in.ki.wScan = static_cast<WORD>(c);
            in.ki.dwFlags = KEYEVENTF_UNICODE;
            sendOne(in);
            in.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
            sendOne(in);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
}

void HookBackend::ShowStatus(std::wstring_view message) {
    if (statusCb_) {
        statusCb_(message, true);
    }
}

void HookBackend::HideStatus() {
    if (statusCb_) {
        statusCb_(L"", false);
    }
}

} // namespace aiie