// aiie_realtsf_test — real-world end-to-end validation against REAL Windows
// TSF, REAL Notepad and the REAL Gemini API (GEMINI_API_KEY environment
// variable). This is NOT the mock host: the TIP is loaded by the Windows TSF
// subsystem (ctfmon) into Notepad's process, receives keystrokes through the
// TSF keystroke manager, translates with the live Gemini API and commits into
// Notepad's native TSF text store.
//
// Keystrokes are injected with SendInput because automation cannot physically
// type. Only the RAW SOURCE-LANGUAGE keystrokes are injected; the English
// translation is produced inside Notepad by the TIP and committed through TSF.
// The harness never injects, pastes or types the English text.
//
// Exit codes: 0 = all four acceptance criteria passed, 1 = any failed,
// 2 = usage/argument error.

#include "Configuration/ConfigStore.h"
#include "TSF/TsfGuids.h"

#include <windows.h>
#include <msctf.h>
#include <oaidl.h>
#include <uiautomation.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr wchar_t kSentence[] = L"Necesito terminar este proyecto antes del viernes.";

// ---------------------------------------------------------------------------
// Tiny helpers
// ---------------------------------------------------------------------------

std::wstring TipLogPath() {
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    return std::wstring(temp) + L"aiie_tip.log";
}

std::string ReadFileUtf8(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Snapshot of the TIP log: captures the file size so callers can detect lines
// appended after a given moment (the TIP is only expected to log while it is
// actually activated inside an application).
size_t TipLogSize() {
    std::ifstream f(TipLogPath(), std::ios::binary | std::ios::ate);
    return static_cast<size_t>(f.tellg());
}

size_t CountAfter(const std::string& content, size_t offset, const char* needle) {
    if (content.size() <= offset) {
        return 0;
    }
    size_t count = 0;
    size_t pos = offset;
    while ((pos = content.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += strlen(needle);
    }
    return count;
}

void SleepMs(DWORD ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// ---------------------------------------------------------------------------
// Notepad handling (Win11 Notepad nests the editor in a CoreWindow)
// ---------------------------------------------------------------------------

void CleanupTestWindows() {
    EnumWindows([](HWND hwnd, LPARAM) -> BOOL {
        wchar_t cls[64];
        wchar_t title[256];
        GetClassNameW(hwnd, cls, 64);
        if (wcscmp(cls, L"Notepad") != 0) {
            return TRUE;
        }
        GetWindowTextW(hwnd, title, 256);
        if (wcsncmp(title, L"Untitled", 8) == 0) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return TRUE;
    }, 0);
    SleepMs(400);
    keybd_event(VK_ESCAPE, 0, 0, 0);
    keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
    SleepMs(300);
}

HWND FindEditRecursive(HWND hwnd) {
    wchar_t cls[64];
    GetClassNameW(hwnd, cls, 64);
    if (wcscmp(cls, L"RichEditD2DPT") == 0 || wcscmp(cls, L"Edit") == 0) {
        return hwnd;
    }
    for (HWND child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        if (HWND found = FindEditRecursive(child)) {
            return found;
        }
    }
    return nullptr;
}

HWND LaunchNotepadEdit() {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(L"C:\\Windows\\System32\\notepad.exe", nullptr, nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &si, &pi)) {
        return nullptr;
    }
    CloseHandle(pi.hThread);

    std::set<HWND> existing;
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        wchar_t cls[64];
        GetClassNameW(hwnd, cls, 64);
        if (wcscmp(cls, L"Notepad") == 0) {
            reinterpret_cast<std::set<HWND>*>(lp)->insert(hwnd);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&existing));

    HWND edit = nullptr;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        struct ScanState {
            const std::set<HWND>* existing;
            HWND* found;
        } state{&existing, &edit};
        EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
            wchar_t cls[64];
            GetClassNameW(hwnd, cls, 64);
            if (wcscmp(cls, L"Notepad") == 0) {
                const auto& state = *reinterpret_cast<ScanState*>(lp);
                if (state.existing->count(hwnd) != 0) {
                    return TRUE;
                }
                if (HWND found = FindEditRecursive(hwnd)) {
                    *state.found = found;
                    return FALSE;
                }
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&state));
        if (edit) {
            break;
        }
        SleepMs(200);
    }
    CloseHandle(pi.hProcess);
    return edit;
}

bool FocusWindow(HWND hwnd) {
    const HWND top = GetAncestor(hwnd, GA_ROOT);
    ShowWindow(top, SW_RESTORE);
    SetWindowPos(top, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    for (int attempt = 0; attempt < 5; ++attempt) {
        keybd_event(VK_MENU, 0, 0, 0);
        keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
        const DWORD fgThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        const DWORD myThread = GetCurrentThreadId();
        AttachThreadInput(fgThread, myThread, TRUE);
        BringWindowToTop(top);
        SetForegroundWindow(top);
        AttachThreadInput(fgThread, myThread, FALSE);
        SleepMs(120);
        if (GetForegroundWindow() == top) {
            break;
        }
    }
    SetWindowPos(top, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SendMessageW(hwnd, WM_SETFOCUS, 0, 0);
    SleepMs(200);
    return GetForegroundWindow() == top;
}

std::wstring ReadEditText(HWND edit) {
    std::wstring text;
    IUIAutomation* uia = nullptr;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IUIAutomation, reinterpret_cast<void**>(&uia)))) {
        return text;
    }
    IUIAutomationElement* element = nullptr;
    if (SUCCEEDED(uia->ElementFromHandle(edit, &element))) {
        IUIAutomationValuePattern* value = nullptr;
        if (SUCCEEDED(element->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&value)))) {
            BSTR bstr = nullptr;
            if (SUCCEEDED(value->get_CurrentValue(&bstr)) && bstr) {
                text = bstr;
                SysFreeString(bstr);
            }
            value->Release();
        }
        element->Release();
    }
    uia->Release();
    return text;
}

// ---------------------------------------------------------------------------
// Input switching / activation of the AI Input Engine TIP for Notepad
// ---------------------------------------------------------------------------

void PressKey(BYTE vk, bool down) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(in));
}

// Cycles the active input method (Win+Space, with the legacy Ctrl+Shift
// fallback) until the TIP's log shows it received focus inside Notepad.
bool CycleToTip(size_t logBaseline) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    int cycle = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const std::string content = ReadFileUtf8(TipLogPath());
        if (CountAfter(content, logBaseline, "focus changed") > 0) {
            return true;
        }
        ++cycle;
        // Win+Space is the default input-switch hotkey on Win10/11.
        PressKey(VK_LWIN, true);
        PressKey(VK_SPACE, true);
        PressKey(VK_SPACE, false);
        PressKey(VK_LWIN, false);
        SleepMs(700);
        // Fallback for the legacy "Ctrl+Shift" input-switch setting.
        if (cycle % 4 == 0) {
            PressKey(VK_CONTROL, true);
            PressKey(VK_SHIFT, true);
            PressKey(VK_SHIFT, false);
            PressKey(VK_CONTROL, false);
            SleepMs(500);
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Keystroke injection (raw source-language characters only)
// ---------------------------------------------------------------------------

std::wstring InjectCharacters(const std::wstring& text, int delayMs) {
    std::wstring resolved;
    const bool capsLock = (GetKeyState(VK_CAPITAL) & 1) != 0;
    for (const wchar_t c : text) {
        const SHORT vkScan = VkKeyScanW(c);
        if (vkScan == -1) {
            continue;
        }
        const BYTE vk = static_cast<BYTE>(vkScan & 0xFF);
        const bool needsShift = ((vkScan & 0x100) != 0) != capsLock;
        if (needsShift) {
            PressKey(VK_SHIFT, true);
        }
        PressKey(vk, true);
        PressKey(vk, false);
        if (needsShift) {
            PressKey(VK_SHIFT, false);
        }
        resolved.push_back(c);
        SleepMs(static_cast<DWORD>(delayMs));
    }
    return resolved;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::wstring sentence = kSentence;
    DWORD timeoutMs = 120000;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--text" && i + 1 < argc) {
            sentence = argv[++i];
        } else if (arg == L"--timeout" && i + 1 < argc) {
            timeoutMs = static_cast<DWORD>(_wtoi(argv[++i]));
        } else {
            std::wprintf(L"Usage: aiie_realtsf_test [--text <sentence>] [--timeout <ms>]\n");
            return 2;
        }
    }

    std::printf("== AI Input Engine: real Windows TSF + Notepad + Gemini validation ==\n\n");

    // --- Gemini API key availability (never printed) ---
    const DWORD keyLen = GetEnvironmentVariableW(L"GEMINI_API_KEY", nullptr, 0);
    const bool hasKey = keyLen > 0;
    std::printf("[env] GEMINI_API_KEY: %s\n", hasKey ? "present" : "MISSING");
    if (!hasKey) {
        std::printf("FAIL: GEMINI_API_KEY environment variable not set\n");
        return 1;
    }

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        std::printf("FAIL: CoInitializeEx\n");
        return 1;
    }

    // --- configuration: temporarily force Gemini + es->en, restore afterwards ---
    // Written only AFTER the early-exit checks above: every path from here on
    // reaches the single cleanup block at the end, which restores the config.
    aiie::ConfigStore store;
    store.Load();
    const aiie::AppConfig savedConfig = store.Get();
    {
        aiie::AppConfig test = savedConfig;
        test.enabled = true;
        test.provider = aiie::ProviderId::Gemini;
        test.sourceLanguage = L"es";
        test.targetLanguage = L"en";
        test.debounceMs = 300;
        test.minChars = 1;
        store.Set(test);
        (void)store.Save();
    }

    bool tsfReal = false;
    bool notepadReal = false;
    bool geminiLive = false;
    bool e2e = false;
    bool keyboardInjection = false;
    std::wstring finalText;
    std::string tipLog;

    // --- 1) REAL Windows TSF: is the TIP known to the Windows TSF subsystem? ---
    {
        CLSID clsid = CLSID_AIInputEngineTIP;
        ITfInputProcessorProfiles* pProfiles = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                      CLSCTX_INPROC_SERVER, IID_ITfInputProcessorProfiles,
                                      reinterpret_cast<void**>(&pProfiles));
        if (SUCCEEDED(hr) && pProfiles) {
            IEnumTfLanguageProfiles* pEnum = nullptr;
            const HRESULT hrEnum = pProfiles->EnumLanguageProfiles(0x0409, &pEnum);
            bool found = false;
            if (SUCCEEDED(hrEnum) && pEnum) {
                TF_LANGUAGEPROFILE prof{};
                ULONG n = 0;
                while (pEnum->Next(1, &prof, &n) == S_OK) {
                    if (IsEqualCLSID(prof.clsid, clsid)) {
                        found = true;
                    }
                }
                pEnum->Release();
            }
            std::printf("[TSF] TIP profile enumerated by Windows TSF (lang 0x0409): %s\n",
                        found ? "yes" : "NO");
            if (!found && pEnum) {
                // Diagnostic: does enumeration work at all for this language?
                std::vector<std::wstring> others;
                TF_LANGUAGEPROFILE prof{};
                ULONG n = 0;
                HRESULT hrNext = pEnum->Reset();
                while (SUCCEEDED(hrNext) && (hrNext = pEnum->Next(1, &prof, &n)) == S_OK) {
                    wchar_t clsidStr[64] = {};
                    StringFromGUID2(prof.clsid, clsidStr, 64);
                    others.push_back(clsidStr);
                }
                std::printf("[TSF] other profiles enumerated for 0x0409: %zu\n", others.size());
                for (const std::wstring& s : others) {
                    std::printf("      %ls\n", s.c_str());
                }
            }
            const HRESULT hrActivate =
                pProfiles->ActivateLanguageProfile(clsid, 0x0409, GUID_AIInputEngineLangProfile);
            std::printf("[TSF] ActivateLanguageProfile: %s (0x%08X)\n",
                        SUCCEEDED(hrActivate) ? "OK" : "FAILED", hrActivate);
            tsfReal = found && SUCCEEDED(hrActivate);
            pProfiles->Release();
        } else {
            std::printf("[TSF] no ITfInputProcessorProfiles (0x%08X)\n", hr);
        }
    }

    if (!tsfReal) {
        std::printf("\nREAL Windows TSF does not know the AI Input Engine TIP yet.\n"
                    "This normally means the TIP was registered after the current logon\n"
                    "session started; the Text Services Framework (ctfmon) caches the TIP\n"
                    "list at sign-in. A sign-out/sign-in or reboot refreshes the cache.\n\n");
    }

    // --- 2) REAL Notepad ---
    CleanupTestWindows();
    HWND edit = LaunchNotepadEdit();
    if (edit) {
        notepadReal = true;
        std::printf("[notepad] launched, edit control found: %p\n", static_cast<void*>(edit));
    } else {
        std::printf("[notepad] FAIL: could not launch/find Notepad edit control\n");
    }

    if (tsfReal && notepadReal) {
        if (!FocusWindow(edit)) {
            std::printf("[notepad] FAIL: could not focus Notepad\n");
        } else {
            // --- 3) activate the TIP inside Notepad via the real input switch ---
            const size_t baseline = TipLogSize();
            std::printf("[input] cycling input methods to activate AI Input Engine...\n");
            const bool activated = CycleToTip(baseline);
            std::printf("[input] TIP active in Notepad: %s\n", activated ? "yes" : "no");
            if (!activated) {
                tipLog = ReadFileUtf8(TipLogPath());
            } else {
                const std::string content = ReadFileUtf8(TipLogPath());
                tipLog = content;
                // --- 4) type the Spanish sentence: RAW KEYSTROKES ONLY ---
                std::printf("[input] injecting raw source-language keystrokes (%zu chars)...\n",
                            sentence.size());
                const std::wstring injected = InjectCharacters(sentence, 30);
                keyboardInjection = true;
                std::printf("[input] injected characters: \"%ls\" (%zu chars)\n", injected.c_str(),
                            injected.size());
                std::printf("[input] English translation injected via keyboard: NO (never sent)\n");

                // --- 5) wait for debounce/terminator + Gemini + TSF commit ---
                const DWORD start = GetTickCount();
                while (GetTickCount() - start < timeoutMs) {
                    SleepMs(100);
                    finalText = ReadEditText(edit);
                    tipLog = ReadFileUtf8(TipLogPath());
                    if (finalText.find(L"Friday") != std::wstring::npos &&
                        finalText.find(L"project") != std::wstring::npos) {
                        break;
                    }
                    if (tipLog.find("committed replacement") != std::string::npos &&
                        finalText.size() >= sentence.size()) {
                        break;
                    }
                }

                const DWORD elapsed = GetTickCount() - start;
                std::printf("[e2e] waited %lu ms for the translation to land\n", elapsed);

                // --- 6) evaluate ---
                const std::wstring lower = finalText;
                const bool hasFriday = lower.find(L"Friday") != std::wstring::npos;
                const bool hasProject = lower.find(L"project") != std::wstring::npos;
                const bool hasVerb = lower.find(L"finish") != std::wstring::npos ||
                                     lower.find(L"complete") != std::wstring::npos ||
                                     lower.find(L"done") != std::wstring::npos;

                geminiLive = tipLog.find("provider gemini") != std::string::npos &&
                             tipLog.find("committed replacement") != std::string::npos;
                e2e = hasFriday && hasProject && hasVerb;

                std::printf("\n-- Gemini/TSF log evidence --\n");
                const std::string log = tipLog;
                for (const char* needle :
                     {"provider gemini", "request #", "committed replacement", "composition committed",
                      "AdviseKeyEventSink result", "focus changed"}) {
                    size_t pos = 0;
                    std::size_t shown = 0;
                    while ((pos = log.find(needle, pos)) != std::string::npos && shown < 3) {
                        const size_t lineStart = log.rfind('\n', pos) + 1;
                        const size_t lineEnd = log.find('\n', pos);
                        std::printf("  %s\n", log.substr(lineStart, lineEnd - lineStart).c_str());
                        pos += strlen(needle);
                        ++shown;
                    }
                }
                std::printf("-- end log evidence --\n");

                std::printf("\nNotepad final text: \"%ls\"\n", finalText.c_str());
                std::printf("Translation keywords present: Friday=%s project=%s verb=%s\n",
                            hasFriday ? "yes" : "no", hasProject ? "yes" : "no",
                            hasVerb ? "yes" : "no");
                std::printf("TIP log shows a live Gemini request + committed replacement: %s\n",
                            geminiLive ? "yes" : "no");
            }
        }
    }

    // --- cleanup ---
    if (edit) {
        const HWND top = GetAncestor(edit, GA_ROOT);
        SendMessageW(top, WM_CLOSE, 0, 0);
        SleepMs(400);
        keybd_event(VK_ESCAPE, 0, 0, 0);
        keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
    }
    CoUninitialize();
    store.Set(savedConfig);
    (void)store.Save();

    // --- final evidence report ---
    std::printf("\n===== FINAL EVIDENCE =====\n");
    std::printf("TSF real Windows: %s\n", tsfReal ? "PASS" : "FAIL");
    std::printf("Notepad real application: %s\n", notepadReal ? "PASS" : "FAIL");
    std::printf("Gemini live API request: %s\n", geminiLive ? "PASS" : "FAIL");
    std::printf("Gemini + TSF + Notepad end-to-end: %s\n", e2e ? "PASS" : "FAIL");
    std::printf("Keyboard injection involved: %s\n", keyboardInjection ? "YES" : "NO");
    if (keyboardInjection) {
        std::printf("  (only the %zu raw Spanish keystrokes were injected; the English\n"
                    "   translation was produced and committed by the TIP via native TSF)\n",
                    sentence.size());
    }
    return (tsfReal && notepadReal && geminiLive && e2e) ? 0 : 1;
}