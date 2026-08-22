// Integration test: drives a real Notepad instance through the hook engine.
//
// Flow (one cycle):
//   1. Clear the document, type a sentence with real key events (SendInput).
//   2. The hook feeds the engine; the terminator triggers an immediate
//      request; the provider returns the translation; the hook commits via
//      SendInput (backspaces + text).
//   3. Poll Notepad's text until the exact translation appears (or fail).
//
// Provider: default is MockProvider (es->en dictionary, "hello world.").
// With --provider gemini, the real GeminiProvider is used through the
// ProviderFactory (key from the stored credential or GEMINI_API_KEY) and the
// expected text is the exact live translation of the Spanish probe sentence.
//
// --repeats N runs N consecutive cycles (varying sentence lengths), which
// exercises the delete/replace pipeline repeatedly and reports an EXACT-match
// verdict per cycle (no fragment matching, no normalization). --gap-ms spaces
// the cycles so the free-tier Gemini quota (20 req/min) is not exhausted.
//
// Requires a US-style keyboard layout and a desktop session. Returns 0 on
// success, 1 on failure, 2 when Notepad cannot be launched.

#include "Composition/InputEngine.h"
#include "Configuration/AppConfig.h"
#include "Configuration/ConfigStore.h"
#include "Core/Dispatcher.h"
#include "Input/HookBackend.h"
#include "Providers/MockProvider.h"
#include "Providers/ProviderFactory.h"
#include "Translation/TranslationCache.h"
#include "Translation/TranslationPipeline.h"

#include <windows.h>
#include <oaidl.h>
#include <uiautomation.h>

#include <chrono>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr wchar_t kExpectedMock[] = L"hello world.";
constexpr wchar_t kProbeText[] = L"Necesito terminar este proyecto antes del viernes.";

// Varying sentence lengths (US-layout typable, no accented characters).
// [0] is the acceptance sentence. Lengths: 50, 5, 13, 24, 26, 35, 68, 6.
const wchar_t* kGeminiSentences[] = {
    L"Necesito terminar este proyecto antes del viernes.",
    L"Hola.",
    L"Tengo hambre.",
    L"Donde esta la biblioteca?",
    L"Manana saldremos temprano.",
    L"El perro corre rapido por el parque.",
    L"Este es un mensaje de prueba muy largo para el sistema de traduccion.",
    L"Adios.",
};

HWND g_edit = nullptr;

// Close any Notepad windows left over from previous test runs (their content
// contains only test markers). Never touches a real user document.
void CleanupTestWindows() {
    const wchar_t* markers[] = {L"hola", L"hello", L"PROBE", L"PROBE2", L"ooooo", L"undo",
                                L".........", nullptr};
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        wchar_t cls[64];
        wchar_t title[256];
        GetClassNameW(hwnd, cls, 64);
        if (wcscmp(cls, L"Notepad") != 0) {
            return TRUE;
        }
        GetWindowTextW(hwnd, title, 256);
        bool isTest = wcsncmp(title, L"Untitled", 8) == 0;
        const wchar_t* const* markers = reinterpret_cast<const wchar_t* const*>(lp);
        for (int i = 0; markers[i]; ++i) {
            if (wcsstr(title, markers[i])) {
                isTest = true;
                break;
            }
        }
        if (isTest) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(markers));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    // Dismiss any "save changes?" prompt.
    keybd_event(VK_ESCAPE, 0, 0, 0);
    keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
}

// Recursive search for the edit control (Win11 Notepad nests it under a
// Windows.UI.Core.CoreWindow host; the classic Notepad exposes "Edit").
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
    // Win11 Notepad hands the UI off to a pre-existing process, so the new
    // window may belong to a different pid. Capture existing windows before launching.
    std::set<HWND> existing;
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        wchar_t cls[64];
        GetClassNameW(hwnd, cls, 64);
        if (wcscmp(cls, L"Notepad") == 0) {
            reinterpret_cast<std::set<HWND>*>(lp)->insert(hwnd);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&existing));

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(L"C:\\Windows\\System32\\notepad.exe", nullptr, nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &si, &pi)) {
        return nullptr;
    }
    CloseHandle(pi.hThread);

    HWND edit = nullptr;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        g_edit = nullptr;
        EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
            wchar_t cls[64];
            GetClassNameW(hwnd, cls, 64);
            if (wcscmp(cls, L"Notepad") == 0) {
                const auto& existingSet = *reinterpret_cast<const std::set<HWND>*>(lp);
                if (existingSet.count(hwnd) != 0) {
                    return TRUE; // pre-existing window: skip
                }
                if (HWND found = FindEditRecursive(hwnd)) {
                    g_edit = found;
                    return FALSE;
                }
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&existing));
        edit = g_edit;
        if (edit) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // Fallback: if no brand-new window was tagged, search any Notepad window
    if (!edit) {
        EnumWindows([](HWND hwnd, LPARAM) -> BOOL {
            wchar_t cls[64];
            GetClassNameW(hwnd, cls, 64);
            if (wcscmp(cls, L"Notepad") == 0) {
                if (HWND found = FindEditRecursive(hwnd)) {
                    g_edit = found;
                    return FALSE;
                }
            }
            return TRUE;
        }, 0);
        edit = g_edit;
    }

    CloseHandle(pi.hProcess);
    return edit;
}

// Sends real keyboard input (down + up per character, holding Shift for
// characters that need it, so the document receives the exact sentence).
void SendText(const std::wstring& text) {
    const WORD shiftScan = static_cast<WORD>(MapVirtualKeyW(VK_LSHIFT, MAPVK_VK_TO_VSC));
    for (const wchar_t c : text) {
        const SHORT vkScan = VkKeyScanW(c);
        if (vkScan == -1) {
            continue;
        }
        const BYTE vk = static_cast<BYTE>(vkScan & 0xFF);
        const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        const bool needsShift = (vkScan & 0x0100) != 0;
        std::vector<INPUT> inputs;
        auto add = [&inputs](BYTE vk2, DWORD flags, WORD sc) {
            INPUT in{};
            in.type = INPUT_KEYBOARD;
            in.ki.wVk = vk2;
            in.ki.wScan = sc;
            in.ki.dwFlags = flags;
            inputs.push_back(in);
        };
        if (needsShift) {
            add(VK_LSHIFT, 0, shiftScan);
        }
        add(vk, 0, static_cast<WORD>(scan));
        add(vk, KEYEVENTF_KEYUP, static_cast<WORD>(scan));
        if (needsShift) {
            add(VK_LSHIFT, KEYEVENTF_KEYUP, shiftScan);
        }
        if (!inputs.empty()) {
            SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
        }
    }
}

std::wstring ReadEditText(HWND edit) {
    // RichEditD2DPT (Win11 Notepad) does not respond to GetWindowTextW, and
    // clipboard-based reads depend on focus. Use UI Automation's ValuePattern
    // instead: reads the control regardless of focus.
    std::wstring text;
    IUIAutomation* uia = nullptr;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IUIAutomation, reinterpret_cast<void**>(&uia)))) {
        return text;
    }
    IUIAutomationElement* element = nullptr;
    if (SUCCEEDED(uia->ElementFromHandle(edit, &element))) {
        IUIAutomationValuePattern* value = nullptr;
        if (SUCCEEDED(element->GetCurrentPatternAs(UIA_ValuePatternId,
                                                   IID_PPV_ARGS(&value)))) {
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

// Clears whatever the target document may contain (leftover debris from a
// previous run that failed to close). Ctrl+A + Backspace via real keys; the
// hook also flushes the engine buffer as-is on the Ctrl+A combination.
// Caller must ensure WaitForCommitComplete() has been called first, so the
// injected keystrokes are not swallowed by the self-injection guard.
void ClearEdit(HWND edit) {
    if (edit) {
        RECT rc{};
        GetWindowRect(edit, &rc);
        const int midX = (rc.left + rc.right) / 2;
        const int midY = (rc.top + rc.bottom) / 2;
        SetCursorPos(midX, midY);
        mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        IUIAutomation* uia = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_IUIAutomation, reinterpret_cast<void**>(&uia)))) {
            IUIAutomationElement* element = nullptr;
            if (SUCCEEDED(uia->ElementFromHandle(edit, &element))) {
                IUIAutomationValuePattern* value = nullptr;
                if (SUCCEEDED(element->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&value)))) {
                    BSTR empty = SysAllocString(L"");
                    (void)value->SetValue(empty);
                    SysFreeString(empty);
                    value->Release();
                }
                element->Release();
            }
            uia->Release();
        }
    }

    auto press = [](BYTE vk, bool down) {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = vk;
        in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
        SendInput(1, &in, sizeof(in));
    };
    press(VK_CONTROL, true);
    press(L'A', true);
    press(L'A', false);
    press(VK_CONTROL, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    press(VK_BACK, true);
    press(VK_BACK, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

bool FocusWindow(HWND hwnd) {
    const HWND top = GetAncestor(hwnd, GA_ROOT);
    if (!top) {
        return false;
    }
    ShowWindow(top, SW_RESTORE);
    SetWindowPos(top, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);

    LockSetForegroundWindow(LSFW_UNLOCK);
    AllowSetForegroundWindow(ASFW_ANY);

    SwitchToThisWindow(top, TRUE);

    RECT rc{};
    GetWindowRect(hwnd, &rc);
    const int midX = (rc.left + rc.right) / 2;
    const int midY = (rc.top + rc.bottom) / 2;
    SetCursorPos(midX, midY);
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);

    const DWORD topThread = GetWindowThreadProcessId(top, nullptr);
    const DWORD myThread = GetCurrentThreadId();

    for (int attempt = 0; attempt < 20; ++attempt) {
        keybd_event(VK_MENU, 0, 0, 0);
        keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);

        const HWND curFg = GetForegroundWindow();
        const DWORD fgThread = curFg ? GetWindowThreadProcessId(curFg, nullptr) : 0;

        if (fgThread != 0 && fgThread != myThread) {
            AttachThreadInput(myThread, fgThread, TRUE);
        }
        if (topThread != 0 && topThread != myThread) {
            AttachThreadInput(myThread, topThread, TRUE);
        }

        BringWindowToTop(top);
        SetForegroundWindow(top);
        SetActiveWindow(top);
        SetFocus(hwnd);

        IUIAutomation* uia = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_IUIAutomation, reinterpret_cast<void**>(&uia))) && uia) {
            IUIAutomationElement* el = nullptr;
            if (SUCCEEDED(uia->ElementFromHandle(hwnd, &el)) && el) {
                (void)el->SetFocus();
                el->Release();
            }
            uia->Release();
        }

        if (fgThread != 0 && fgThread != myThread) {
            AttachThreadInput(myThread, fgThread, FALSE);
        }
        if (topThread != 0 && topThread != myThread) {
            AttachThreadInput(myThread, topThread, FALSE);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const HWND fg = GetForegroundWindow();
        if (fg == top || GetAncestor(fg, GA_ROOT) == top) {
            break;
        }
    }
    SetWindowPos(top, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

    keybd_event(VK_ESCAPE, 0, 0, 0);
    keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);

    SendMessageW(hwnd, WM_SETFOCUS, 0, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const HWND finalFg = GetForegroundWindow();
    return finalFg == top || GetAncestor(finalFg, GA_ROOT) == top || finalFg == nullptr;
}

// Wraps a provider and captures the exact text of the last successful result,
// so the test can compare the final document against the precise translation
// the engine committed (the live translation is unknown in advance).
// Also resolves expected text from TranslationCache on cache hits.
class CaptureProvider : public aiie::ITranslationProvider {
public:
    CaptureProvider(std::shared_ptr<aiie::ITranslationProvider> inner,
                    std::shared_ptr<aiie::TranslationCache> cache)
        : inner_(std::move(inner)), cache_(std::move(cache)) {}

    std::wstring_view Name() const override { return inner_->Name(); }
    std::wstring_view DisplayName() const override { return inner_->DisplayName(); }
    bool RequiresNetwork() const override { return inner_->RequiresNetwork(); }
    aiie::ProviderStatus Status() const override { return inner_->Status(); }
    void CancelAll() override { inner_->CancelAll(); }

    void Translate(aiie::TranslationRequest request, aiie::TranslationCallback callback) override {
        inner_->Translate(std::move(request),
                          [this, callback = std::move(callback)](aiie::TranslationResult result) {
            if (result.ok && !result.text.empty()) {
                std::lock_guard lock(mutex_);
                lastText_ = result.text;
                haveLast_ = true;
            }
            callback(std::move(result));
        });
    }

    void Reset() {
        std::lock_guard lock(mutex_);
        haveLast_ = false;
        lastText_.clear();
    }

    bool HaveLast(const aiie::TranslationRequest* req = nullptr) const {
        std::lock_guard lock(mutex_);
        if (haveLast_) {
            return true;
        }
        if (req && cache_) {
            return cache_->Lookup(*req) != nullptr;
        }
        return false;
    }

    std::wstring LastText(const aiie::TranslationRequest* req = nullptr) const {
        std::lock_guard lock(mutex_);
        if (haveLast_) {
            return lastText_;
        }
        if (req && cache_) {
            if (const std::wstring* cached = cache_->Lookup(*req)) {
                return *cached;
            }
        }
        return {};
    }

private:
    std::shared_ptr<aiie::ITranslationProvider> inner_;
    std::shared_ptr<aiie::TranslationCache> cache_;
    mutable std::mutex mutex_;
    bool haveLast_ = false;
    std::wstring lastText_;
};

// Classifies an exact-match failure for the report: where the stray/missing
// characters are and whether they relate to the source sentence.
std::wstring ClassifyMismatch(const std::wstring& expected, const std::wstring& actual,
                              const std::wstring& sentence) {
    if (actual == expected) {
        return L"EXACT";
    }
    std::wstring out;
    if (actual.size() > expected.size() && actual.compare(0, expected.size(), expected) == 0) {
        out += L"extra trailing '";
        out += actual.substr(expected.size());
        out += L"'";
        return out;
    }
    if (actual.size() > expected.size() && actual.compare(actual.size() - expected.size(),
                                                          expected.size(), expected) == 0) {
        const std::wstring lead = actual.substr(0, actual.size() - expected.size());
        out += L"leading stray '";
        out += lead;
        out += L"'";
        if (sentence.size() >= lead.size() &&
            sentence.compare(sentence.size() - lead.size(), lead.size(), lead) == 0) {
            out += L" (suffix of source sentence)";
        } else if (sentence.compare(0, lead.size(), lead) == 0) {
            out += L" (prefix of source sentence)";
        } else if (sentence.find(lead) != std::wstring::npos) {
            out += L" (substring of source sentence)";
        }
        return out;
    }
    size_t pos = 0;
    while (pos < expected.size() && pos < actual.size() && expected[pos] == actual[pos]) {
        ++pos;
    }
    out += L"mismatch at pos ";
    out += std::to_wstring(pos);
    out += L" (expected len ";
    out += std::to_wstring(expected.size());
    out += L", actual len ";
    out += std::to_wstring(actual.size());
    out += L")";
    return out;
}

struct CycleReport {
    std::wstring sentence;
    std::wstring expected;
    std::wstring actual;
    std::wstring verdict;
    bool ok = false;
};

} // namespace

int wmain(int argc, wchar_t** argv) {
    aiie::ProviderId selectedProvider = aiie::ProviderId::Mock;
    aiie::TransformOperation selectedOp = aiie::TransformOperation::Translate;
    std::wstring sourceLang = L"es";
    std::wstring targetLang = L"en";
    aiie::TranslationProfile selectedProfile = aiie::TranslationProfile::Neutral;
    int repeats = 1;
    int gapMs = 10000;
    std::wstring model;
    std::vector<std::wstring> sentences;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--provider" && i + 1 < argc) {
            const std::wstring provStr = argv[++i];
            if (provStr == L"gemini") {
                selectedProvider = aiie::ProviderId::Gemini;
            } else if (provStr == L"openai") {
                selectedProvider = aiie::ProviderId::OpenAI;
            } else {
                selectedProvider = aiie::ProviderId::Mock;
            }
        } else if (arg == L"--operation" && i + 1 < argc) {
            const std::wstring opStr = argv[++i];
            if (opStr == L"correct_and_rewrite" || opStr == L"rewrite") {
                selectedOp = aiie::TransformOperation::CorrectAndRewrite;
            } else {
                selectedOp = aiie::TransformOperation::Translate;
            }
        } else if (arg == L"--source" && i + 1 < argc) {
            sourceLang = argv[++i];
        } else if (arg == L"--target" && i + 1 < argc) {
            targetLang = argv[++i];
        } else if (arg == L"--profile" && i + 1 < argc) {
            const std::wstring profStr = argv[++i];
            aiie::TryParseProfile(profStr, selectedProfile);
        } else if (arg == L"--repeats" && i + 1 < argc) {
            repeats = _wtoi(argv[++i]);
            if (repeats < 1) {
                repeats = 1;
            }
        } else if (arg == L"--gap-ms" && i + 1 < argc) {
            gapMs = _wtoi(argv[++i]);
            if (gapMs < 0) {
                gapMs = 0;
            }
        } else if (arg == L"--model" && i + 1 < argc) {
            model = argv[++i];
        } else if (arg == L"--sentences" && i + 1 < argc) {
            std::wstring all = argv[++i];
            size_t start = 0;
            while (true) {
                const size_t sep = all.find(L';', start);
                sentences.push_back(all.substr(start, sep == std::wstring::npos ? std::wstring::npos : sep - start));
                if (sep == std::wstring::npos) {
                    break;
                }
                start = sep + 1;
            }
        } else {
            std::wprintf(L"Usage: aiie_integration_test [--provider mock|gemini|openai] [--operation translate|correct_and_rewrite] "
                         L"[--source <code>] [--target <code>] [--profile <name>] [--repeats N] [--gap-ms M] [--model <id>] [--sentences s1;s2;...]\n");
            return 2;
        }
    }
    if (sentences.empty()) {
        for (const wchar_t* s : kGeminiSentences) {
            sentences.push_back(s);
        }
    }

    std::printf("[ RUN ] notepad_hook_end_to_end (%ls, %d cycles, %zu sentences)\n",
                aiie::ProviderName(selectedProvider).data(), repeats, sentences.size());

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        std::printf("[FAIL ] notepad_hook_end_to_end: CoInitializeEx failed\n");
        return 1;
    }

    CleanupTestWindows();

    HWND edit = LaunchNotepadEdit();
    if (!edit) {
        std::printf("[FAIL ] notepad_hook_end_to_end: notepad edit control not found\n");
        return 2;
    }
    std::printf("  edit control found: %p\n", static_cast<void*>(edit));

    // Real stack, mirroring TrayApp wiring (immediate dispatcher: the test
    // thread cannot pump messages while polling).
    auto logger = std::shared_ptr<aiie::Logger>(&aiie::Logger::Instance(), [](aiie::Logger*) {});
    auto cache = std::make_shared<aiie::TranslationCache>(200);
    aiie::AppConfig cfg;
    cfg.enabled = true;
    cfg.operation = selectedOp;
    cfg.sourceLanguage = sourceLang;
    cfg.targetLanguage = (selectedOp == aiie::TransformOperation::CorrectAndRewrite) ? sourceLang : targetLang;
    cfg.profile = selectedProfile;
    cfg.provider = selectedProvider;
    if (!model.empty()) {
        cfg.model = model;
    }
    cfg.debounceMs = 300;
    cfg.minChars = 2;
    cfg.maxBufferChars = 400;
    auto rawProvider = aiie::ProviderFactory::Create(cfg);
    if (!rawProvider) {
        std::printf("[FAIL ] notepad_hook_end_to_end: no provider created\n");
        return 1;
    }
    auto provider = std::make_shared<CaptureProvider>(std::move(rawProvider), cache);
    std::printf("  provider: %ls\n", provider->DisplayName().data());
    auto pipeline = std::make_shared<aiie::TranslationPipeline>(provider, cache, 40000);
    auto dispatcher = std::make_shared<aiie::ImmediateDispatcher>();
    auto backend = std::make_shared<aiie::HookBackend>();
    auto engine = std::make_shared<aiie::InputEngine>(dispatcher, backend, pipeline, logger);
    backend->SetEngine(engine.get());

    engine->ApplyConfig(cfg);
    backend->ApplyConfig(cfg);

    if (!backend->Start()) {
        std::printf("[FAIL ] notepad_hook_end_to_end: hook failed to start\n");
        return 1;
    }

    // Give the hook thread a moment to install.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    if (!FocusWindow(edit)) {
        wchar_t cls[64];
        GetClassNameW(GetForegroundWindow(), cls, 64);
        std::printf("[FAIL ] notepad_hook_end_to_end: could not focus notepad (foreground: %ls)\n",
                    cls);
        backend->Stop();
        return 1;
    }

    const bool isNetworkProvider = (selectedProvider == aiie::ProviderId::Gemini ||
                                    selectedProvider == aiie::ProviderId::OpenAI);

    std::vector<CycleReport> reports;
    reports.reserve(static_cast<size_t>(repeats));
    int passed = 0;
    int consecutive = 0;
    int bestConsecutive = 0;

    for (int cycle = 0; cycle < repeats; ++cycle) {
        const std::wstring sentence = isNetworkProvider
                                          ? sentences[static_cast<size_t>(cycle) % sentences.size()]
                                          : std::wstring(L"hola mundo.");

        aiie::TranslationRequest req;
        req.text = sentence;
        req.sourceLanguage = cfg.sourceLanguage;
        req.targetLanguage = cfg.targetLanguage;
        req.mode = cfg.mode;

        // Wait for the previous cycle's commit thread (if any) to finish and
        // for the inject guard to expire, so our SendInput keystrokes are not
        // ignored by the hook's self-injection guard.
        backend->WaitForCommitComplete();

        provider->Reset();
        ClearEdit(edit);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // Diagnostic: verify the document was actually cleared.
        const std::wstring docAfterClear = ReadEditText(edit);
        std::printf("  [cycle %02d/%02d] doc after clear: \"%ls\" (%zu chars)\n", cycle + 1,
                    repeats, docAfterClear.c_str(), docAfterClear.size());

        std::printf("  [cycle %02d/%02d] typing \"%ls\" (%zu chars)...\n", cycle + 1, repeats,
                    sentence.c_str(), sentence.size());
        const uint64_t eventsBefore = backend->hookEventCount_.load();
        const uint64_t engineBefore = backend->hookEngineCount_.load();
        for (const wchar_t c : sentence) {
            SendText(std::wstring(1, c));
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        const aiie::EngineStatus status = engine->Status();
        const std::wstring docAfterType = ReadEditText(edit);
        const uint64_t eventsAfter = backend->hookEventCount_.load();
        const uint64_t engineAfter = backend->hookEngineCount_.load();
        std::printf("  [cycle %02d/%02d] engine: state=%d buffer=\"%ls\" message=\"%ls\"\n",
                    cycle + 1, repeats, static_cast<int>(status.state), engine->Buffer().c_str(),
                    status.lastMessage.c_str());
        std::printf("  [cycle %02d/%02d] doc after type: \"%ls\" (%zu chars)\n", cycle + 1,
                    repeats, docAfterType.c_str(), docAfterType.size());
        std::printf("  [cycle %02d/%02d] hook events: %llu total, %llu to engine\n", cycle + 1, repeats,
                    static_cast<unsigned long long>(eventsAfter - eventsBefore),
                    static_cast<unsigned long long>(engineAfter - engineBefore));
        std::printf("  [cycle %02d/%02d] provider haveLast=%d\n", cycle + 1, repeats,
                    provider->HaveLast(&req) ? 1 : 0);

        // Poll until the exact translation appears in the document. Gemini is
        // slow (quota throttling can stretch a request well beyond the
        // nominal latency), so grant a generous deadline.
        std::wstring finalText;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(isNetworkProvider ? 150 : 10);
        bool ok = false;
        while (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            finalText = ReadEditText(edit);
            if (provider->HaveLast(&req) && finalText == provider->LastText(&req)) {
                ok = true;
                break;
            }
            // Translation failed (e.g. quota): the engine keeps the user's
            // text and nothing more will change — stop polling.
            if (engine->Status().state == aiie::EngineState::Error) {
                break;
            }
        }

        CycleReport report;
        report.sentence = sentence;
        report.expected = provider->HaveLast(&req) ? provider->LastText(&req) : std::wstring();
        report.actual = finalText;
        report.ok = ok && !report.expected.empty() && report.actual == report.expected;
        report.verdict = report.ok ? L"EXACT"
                                   : ClassifyMismatch(report.expected, report.actual, sentence);
        reports.push_back(report);

        if (report.ok) {
            ++passed;
            ++consecutive;
            if (consecutive > bestConsecutive) {
                bestConsecutive = consecutive;
            }
        } else {
            consecutive = 0;
        }

        const aiie::EngineStatus endStatus = engine->Status();
        std::printf("  [cycle %02d/%02d] %ls -> \"%ls\"  [%ls]\n", cycle + 1, repeats,
                    sentence.c_str(), finalText.c_str(), report.verdict.c_str());
        if (!report.ok) {
            std::printf("                 expected exactly: \"%ls\"\n", report.expected.c_str());
            std::printf("                 end engine state=%d message=\"%ls\"\n",
                         static_cast<int>(endStatus.state), endStatus.lastMessage.c_str());
        }

        if (isNetworkProvider && cycle + 1 < repeats && gapMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(gapMs));
        } else if (cycle + 1 < repeats) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        }
    }

    backend->Stop();

    // Clean up: close the window we used (dismiss any save prompt with Esc).
    const HWND top = GetAncestor(edit, GA_ROOT);
    SendMessageW(top, WM_CLOSE, 0, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    keybd_event(VK_ESCAPE, 0, 0, 0);
    keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);

    CoUninitialize();

    std::printf("[SUMMARY] %d/%d cycles exact, best consecutive run: %d\n", passed, repeats,
                bestConsecutive);
    for (size_t i = 0; i < reports.size(); ++i) {
        if (!reports[i].ok) {
            std::printf("[FAILED ] cycle %02zu: len=%zu %ls -> %ls\n", i + 1,
                        reports[i].sentence.size(), reports[i].sentence.c_str(),
                        reports[i].verdict.c_str());
        }
    }

    if (passed == repeats) {
        std::printf("[  OK ] notepad_hook_end_to_end: all %d cycles exact\n", repeats);
        return 0;
    }
    std::printf("[FAIL ] notepad_hook_end_to_end: %d of %d cycles not exact\n",
                repeats - passed, repeats);
    return 1;
}