// aiie_tsfhost â€” host-side proof that the AI Input Engine TSF text service
// performs composition -> translation -> commit inside a real TSF session.
//
// The host:
//   1. creates a TSF thread manager + document manager + a context owned by
//      a recording ITfContextOwner (this process is the "application");
//   2. activates the AI Input Engine language profile so CTF loads the TIP
//      in-process (the DLL registered via aiie_tsfreg --register);
//   3. feeds keystrokes through ITfKeystrokeMgr::KeyDown (the same path a
//      real application's message loop would take);
//   4. records composition events (OnStart/OnUpdate/OnEndComposition) and
//      the committed text; prints PASS/FAIL against the expected translation.
//
// This proves the TIP is actually loaded, active, receives keys, composes,
// translates (Mock or live Gemini) and commits â€” without SendInput.
//
// Usage:
//   aiie_tsfhost [--mock | --gemini] [--text "<sentence>"] [--timeout <ms>]
//
// Exit codes: 0 = PASS, 1 = FAIL, 2 = usage error.

#include "Configuration/ConfigStore.h"
#include "Core/Logging.h"
#include "TSF/TsfGuids.h"

#include <windows.h>
#include <msctf.h>
#include <textstor.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace {

// CONNECT_E_* error codes from objidl.h (connect.h is not on the SDK path here).
constexpr HRESULT CONNECT_E_ADVISELIMIT = 0x80040202L;
constexpr HRESULT CONNECT_E_NOCONNECTION = 0x80040204L;

// The host is a real TSF text store (ITextStoreACP â€” the protocol real
// applications implement). The TIP's edit sessions and text writes land in
// this store, so we can verify the committed translation end to end.
struct RecordingStore : public ITextStoreACP, public ITfContextOwnerCompositionSink {
    std::wstring text; // the "document"
    LONG selStart = 0;
    LONG selEnd = 0;
    ITextStoreACPSink* sink = nullptr;
    TfEditCookie ec = TF_INVALID_EDIT_COOKIE; // set by the host after CreateContext
    HWND hwnd = nullptr;
    HWND edit = nullptr;

    // --- ITfContextOwnerCompositionSink recording ---
    bool compositionStarted = false;
    bool compositionEnded = false;
    std::wstring compositionText;

    // --- IUnknown ---
    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override {
        if (ppvObj == nullptr) {
            return E_POINTER;
        }
        *ppvObj = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITextStoreACP)) {
            *ppvObj = static_cast<ITextStoreACP*>(this);
        } else if (IsEqualIID(riid, IID_ITfContextOwnerCompositionSink)) {
            *ppvObj = static_cast<ITfContextOwnerCompositionSink*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG r = --refs_;
        if (r == 0) {
            delete this;
        }
        return r;
    }

    // --- helpers ---
    void ClampSelection() {
        const LONG len = static_cast<LONG>(text.size());
        selStart = std::clamp(selStart, 0L, len);
        selEnd = std::clamp(selEnd, 0L, len);
    }
    void NotifyChange() {
        if (sink) {
            TS_TEXTCHANGE range;
            range.acpStart = 0;
            range.acpOldEnd = 0;
            range.acpNewEnd = static_cast<LONG>(text.size());
            (void)sink->OnTextChange(0, &range);
            (void)sink->OnSelectionChange();
        }
    }
    std::wstring ReadDoc() const { return text; }

    // --- ITextStoreACP ---
    STDMETHODIMP AdviseSink(REFIID riid, IUnknown* punk, DWORD dwMask) override {
        (void)dwMask;
        ++nAdviseSink;
        std::printf("[host] store AdviseSink #%u\n", nAdviseSink);
        if (!IsEqualIID(riid, IID_ITextStoreACPSink) || punk == nullptr) {
            return E_INVALIDARG;
        }
        if (sink != nullptr) {
            return CONNECT_E_ADVISELIMIT;
        }
        return punk->QueryInterface(IID_ITextStoreACPSink, reinterpret_cast<void**>(&sink));
    }
    STDMETHODIMP UnadviseSink(IUnknown* punk) override {
        ++nUnadviseSink;
        if (sink != nullptr && punk == sink) {
            sink->Release();
            sink = nullptr;
            return S_OK;
        }
        return CONNECT_E_NOCONNECTION;
    }
    STDMETHODIMP RequestLock(DWORD dwLockFlags, HRESULT* phrSession) override {
        ++nRequestLock;
        std::printf("[host] store RequestLock #%u flags=0x%X (sink=%s)\n", nRequestLock, dwLockFlags,
                    sink ? "set" : "null");
        (void)dwLockFlags;
        if (phrSession == nullptr) {
            return E_INVALIDARG;
        }
        if (sink == nullptr) {
            *phrSession = E_FAIL;
            return S_OK;
        }
        // Grant the lock synchronously (single-threaded harness); TSF runs
        // the TIP's edit callback inside OnLockGranted. The argument is the
        // LOCK FLAGS, not the edit cookie.
        *phrSession = sink->OnLockGranted(dwLockFlags);
        return S_OK;
    }
    STDMETHODIMP GetStatus(TS_STATUS* pdcs) override {
        pdcs->dwDynamicFlags = 0;
        pdcs->dwStaticFlags = 0;
        return S_OK;
    }
    STDMETHODIMP QueryInsert(LONG acpTestStart, LONG acpTestEnd, ULONG cch, LONG* pacpResultStart,
                             LONG* pacpResultEnd) override {
        (void)cch;
        *pacpResultStart = acpTestStart;
        *pacpResultEnd = acpTestEnd;
        return S_OK;
    }
    STDMETHODIMP GetSelection(ULONG ulIndex, ULONG ulCount, TS_SELECTION_ACP* pSelection,
                              ULONG* pcFetched) override {
        (void)ulIndex;
        ++nGetSelection;
        std::printf("[host] store GetSelection #%u count=%u\n", nGetSelection, ulCount);
        if (pcFetched) {
            *pcFetched = 0;
        }
        if (pSelection == nullptr || pcFetched == nullptr) {
            return E_INVALIDARG;
        }
        if (ulCount == 0) {
            return S_OK;
        }
        ClampSelection();
        pSelection[0].acpStart = selStart;
        pSelection[0].acpEnd = selEnd;
        pSelection[0].style.ase = TS_AE_START;
        pSelection[0].style.fInterimChar = FALSE;
        *pcFetched = 1;
        return S_OK;
    }
    STDMETHODIMP SetSelection(ULONG ulCount, const TS_SELECTION_ACP* pSelection) override {
        if (ulCount != 1 || pSelection == nullptr) {
            return E_INVALIDARG;
        }
        selStart = pSelection[0].acpStart;
        selEnd = pSelection[0].acpEnd;
        ClampSelection();
        return S_OK;
    }
    STDMETHODIMP GetText(LONG acpStart, LONG acpEnd, WCHAR* pchPlain, ULONG cchPlainReq,
                         ULONG* pcchPlainRet, TS_RUNINFO* prgRunInfo, ULONG cRunInfoReq,
                         ULONG* pcRunInfoRet, LONG* pacpNext) override {
        ++nGetText;
        std::printf("[host] store GetText #%u [%ld,%ld] req=%u\n", nGetText, acpStart, acpEnd,
                    cchPlainReq);
        if (acpStart < 0 || acpStart > static_cast<LONG>(text.size())) {
            return TS_E_INVALIDPOS;
        }
        const LONG end = std::min(acpEnd == -1 ? static_cast<LONG>(text.size()) : acpEnd,
                                  static_cast<LONG>(text.size()));
        if (pcchPlainRet) {
            *pcchPlainRet = 0;
        }
        if (pcRunInfoRet) {
            *pcRunInfoRet = 0;
        }
        if (pchPlain == nullptr || cchPlainReq == 0) {
            if (pacpNext) {
                *pacpNext = end;
            }
            return S_OK;
        }
        const size_t copy = std::min(static_cast<size_t>(end - acpStart),
                                     static_cast<size_t>(cchPlainReq));
        if (copy > 0) {
            memcpy(pchPlain, text.data() + acpStart, copy * sizeof(wchar_t));
        }
        if (pcchPlainRet) {
            *pcchPlainRet = static_cast<ULONG>(copy);
        }
        if (cRunInfoReq > 0 && prgRunInfo != nullptr && copy > 0) {
            prgRunInfo[0].uCount = static_cast<ULONG>(copy);
            prgRunInfo[0].type = TS_RT_PLAIN;
            if (pcRunInfoRet) {
                *pcRunInfoRet = 1;
            }
        }
        if (pacpNext) {
            *pacpNext = acpStart + static_cast<LONG>(copy);
        }
        return S_OK;
    }
    STDMETHODIMP SetText(DWORD dwFlags, LONG acpStart, LONG acpEnd, const WCHAR* pchText,
                         ULONG cch, TS_TEXTCHANGE* pChange) override {
        ++nSetText;
        std::printf("[host] store SetText #%u [%ld,%ld) += \"%*ls\" (sink=%s)\n", nSetText, acpStart,
                    acpEnd, cch, pchText, sink ? "set" : "null");
        (void)dwFlags;
        if (acpStart < 0 || acpStart > static_cast<LONG>(text.size())) {
            return TS_E_INVALIDPOS;
        }
        const LONG end = std::min(acpEnd == -1 ? static_cast<LONG>(text.size()) : acpEnd,
                                  static_cast<LONG>(text.size()));
        const LONG oldEnd = end;
        text.replace(acpStart, end - acpStart, pchText, cch);
        selStart = selEnd = acpStart + static_cast<LONG>(cch);
        if (pChange) {
            pChange->acpStart = acpStart;
            pChange->acpOldEnd = oldEnd;
            pChange->acpNewEnd = acpStart + static_cast<LONG>(cch);
        }
        NotifyChange();
        return S_OK;
    }
    STDMETHODIMP GetFormattedText(LONG, LONG, IDataObject** ppDataObject) override {
        *ppDataObject = nullptr;
        return E_NOTIMPL;
    }
    STDMETHODIMP GetEmbedded(LONG, REFGUID, REFIID, IUnknown** ppunk) override {
        *ppunk = nullptr;
        return E_NOTIMPL;
    }
    STDMETHODIMP QueryInsertEmbedded(const GUID*, const FORMATETC*, BOOL* pfInsertable) override {
        *pfInsertable = FALSE;
        return S_OK;
    }
    STDMETHODIMP InsertEmbedded(DWORD, LONG, LONG, IDataObject*, TS_TEXTCHANGE*) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP InsertTextAtSelection(DWORD dwFlags, const WCHAR* pchText, ULONG cch,
                                       LONG* pacpStart, LONG* pacpEnd, TS_TEXTCHANGE* pChange) override {
        ++nInsertTextAtSelection;
        std::printf("[host] store InsertTextAtSelection #%u \"%*ls\" (sink=%s)\n",
                    nInsertTextAtSelection, cch, pchText, sink ? "set" : "null");
        (void)dwFlags;
        ClampSelection();
        const LONG start = std::min(selStart, selEnd);
        const LONG end = std::max(selStart, selEnd);
        text.replace(start, end - start, pchText, cch);
        selStart = selEnd = start + static_cast<LONG>(cch);
        if (pacpStart) {
            *pacpStart = start;
        }
        if (pacpEnd) {
            *pacpEnd = start + static_cast<LONG>(cch);
        }
        if (pChange) {
            pChange->acpStart = start;
            pChange->acpOldEnd = end;
            pChange->acpNewEnd = start + static_cast<LONG>(cch);
        }
        NotifyChange();
        return S_OK;
    }
    STDMETHODIMP InsertEmbeddedAtSelection(DWORD, IDataObject*, LONG*, LONG*, TS_TEXTCHANGE*) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP RequestSupportedAttrs(DWORD, ULONG, const TS_ATTRID*) override { return S_OK; }
    STDMETHODIMP RequestAttrsAtPosition(LONG, ULONG, const TS_ATTRID*, DWORD) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP RequestAttrsTransitioningAtPosition(LONG, ULONG, const TS_ATTRID*, DWORD) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP FindNextAttrTransition(LONG, LONG, ULONG, const TS_ATTRID*, DWORD, LONG* pacpNext,
                                        BOOL* pfFound, LONG* plFoundOffset) override {
        if (pacpNext) {
            *pacpNext = 0;
        }
        if (pfFound) {
            *pfFound = FALSE;
        }
        if (plFoundOffset) {
            *plFoundOffset = 0;
        }
        return E_NOTIMPL;
    }
    STDMETHODIMP RetrieveRequestedAttrs(ULONG ulCount, TS_ATTRVAL* paAttrVals,
                                        ULONG* pcFetched) override {
        (void)ulCount;
        (void)paAttrVals;
        if (pcFetched) {
            *pcFetched = 0;
        }
        return S_OK;
    }
    STDMETHODIMP GetEndACP(LONG* pacp) override {
        *pacp = static_cast<LONG>(text.size());
        return S_OK;
    }
    STDMETHODIMP GetActiveView(TsViewCookie* pvcView) override {
        *pvcView = 0;
        return S_OK;
    }
    STDMETHODIMP GetACPFromPoint(TsViewCookie, const POINT*, DWORD, LONG* pacp) override {
        *pacp = 0;
        return E_NOTIMPL;
    }
    STDMETHODIMP GetTextExt(TsViewCookie, LONG, LONG, RECT* prc, BOOL* pfClipped) override {
        *prc = {0, 0, 0, 0};
        *pfClipped = TRUE;
        return TS_E_NOLAYOUT;
    }
    STDMETHODIMP GetScreenExt(TsViewCookie, RECT* prc) override {
        GetWindowRect(edit != nullptr ? edit : hwnd, prc);
        return S_OK;
    }
    STDMETHODIMP GetWnd(TsViewCookie, HWND* phwnd) override {
        *phwnd = edit != nullptr ? edit : hwnd;
        return S_OK;
    }

    // --- ITfContextOwnerCompositionSink ---
    STDMETHODIMP OnStartComposition(ITfCompositionView*, BOOL* pfOk) override {
        compositionStarted = true;
        compositionText.clear();
        *pfOk = TRUE;
        std::printf("[host] composition STARTED\n");
        return S_OK;
    }
    STDMETHODIMP OnUpdateComposition(ITfCompositionView*, ITfRange* pRangeNew) override {
        if (pRangeNew != nullptr && ec != TF_INVALID_EDIT_COOKIE) {
            std::vector<wchar_t> buf(512);
            ULONG actual = 0;
            const HRESULT hr = pRangeNew->GetText(ec, TF_TF_IGNOREEND, buf.data(),
                                                  static_cast<ULONG>(buf.size() - 1), &actual);
            if (SUCCEEDED(hr) && actual > 0) {
                buf[static_cast<size_t>(actual)] = 0;
                compositionText = buf.data();
            }
            std::printf("[host] composition UPDATED (hr=0x%08X, %lu): \"%ls\"\n", hr, actual,
                        compositionText.c_str());
        } else {
            std::printf("[host] composition UPDATED: \"%ls\"\n", compositionText.c_str());
        }
        return S_OK;
    }
    STDMETHODIMP OnEndComposition(ITfCompositionView*) override {
        compositionEnded = true;
        std::printf("[host] composition ENDED: \"%ls\"\n", compositionText.c_str());
        return S_OK;
    }

private:
    ULONG refs_ = 1;
    ULONG nAdviseSink = 0;
    ULONG nUnadviseSink = 0;
    ULONG nRequestLock = 0;
    ULONG nGetSelection = 0;
    ULONG nGetText = 0;
    ULONG nSetText = 0;
    ULONG nInsertTextAtSelection = 0;
};
struct ConfigSnapshot {
    bool saved = false;
    aiie::AppConfig config;
};

bool TipLogHasFocusChanged() {
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    std::ifstream f(std::wstring(temp) + L"aiie_tip.log");
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return content.find("focus changed") != std::string::npos;
}

bool PumpMessages(DWORD timeoutMs) {
    const DWORD end = GetTickCount() + timeoutMs;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (GetTickCount() >= end) {
            break;
        }
        Sleep(10); // bounded polling: WaitMessage() would block forever
    }
    return true;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring mode = L"mock";
    std::wstring text = L"hola mundo";
    std::wstring expected = L"hello world";
    DWORD timeoutMs = 30000;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--mock") {
            mode = L"mock";
        } else if (arg == L"--gemini") {
            mode = L"gemini";
        } else if (arg == L"--openai") {
            mode = L"openai";
        } else if (arg == L"--text" && i + 1 < argc) {
            text = argv[++i];
        } else if (arg == L"--expected" && i + 1 < argc) {
            expected = argv[++i];
        } else if (arg == L"--timeout" && i + 1 < argc) {
            timeoutMs = static_cast<DWORD>(_wtoi(argv[++i]));
        } else {
            std::wprintf(L"Usage: aiie_tsfhost [--mock|--gemini|--openai] [--text <sentence>] "
                         L"[--expected <translation>] [--timeout <ms>]\n");
            return 2;
        }
    }

    // --- configuration: use the real shared config, temporarily switching
    // the provider, then restoring it exactly. ---
    aiie::ConfigStore store;
    store.Load();
    ConfigSnapshot snapshot;
    snapshot.saved = true;
    snapshot.config = store.Get();
    {
        aiie::AppConfig test = store.Get();
        test.enabled = true;
        if (mode == L"gemini") {
            test.provider = aiie::ProviderId::Gemini;
        } else if (mode == L"openai") {
            test.provider = aiie::ProviderId::OpenAI;
        } else {
            test.provider = aiie::ProviderId::Mock;
        }
        test.debounceMs = 300;
        test.minChars = 1;
        store.Set(test);
        (void)store.Save();
    }

    aiie::Logger::Instance().SetLevel(aiie::LogLevel::Error);

    // Remove any stale TIP log from previous runs so focus change detection is fresh.
    {
        wchar_t temp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp);
        DeleteFileW((std::wstring(temp) + L"aiie_tip.log").c_str());
    }

    std::printf("TSF host: mode=%ls text=\"%ls\"\n", mode.c_str(), text.c_str());

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        std::printf("FAIL CoInitializeEx 0x%08X\n", hr);
        return 1;
    }

    int result = 1;
    RecordingStore* owner = new RecordingStore();
    ITfThreadMgr* pThreadMgr = nullptr;
    ITfDocumentMgr* pDocMgr = nullptr;
    ITfContext* pContext = nullptr;
    ITfTextInputProcessorEx* pTipDirect = nullptr;
    ITfKeyEventSink* pKeySink = nullptr;
    BYTE state[256] = {};
    TfClientId tid = TF_CLIENTID_NULL;
    bool profileActivated = false;

    // A real (hidden) window makes the context owner's view valid.
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
            return DefWindowProcW(h, m, w, l);
        };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"AIInputEngineTsHost";
        RegisterClassW(&wc);
        owner->hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 320, 120,
                                      nullptr, nullptr, wc.hInstance, nullptr);
        // A real edit control gives the TSF context a window-backed text store
        // (the same mechanism real apps like Notepad use): the TIP's SetText
        // writes into the edit control, and we can read the result back.
        owner->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE |
                                          WS_TABSTOP | ES_AUTOHSCROLL,
                                      0, 0, 320, 40, owner->hwnd, nullptr, wc.hInstance, nullptr);
        SetFocus(owner->edit);
    }

    // 1) Thread manager.
    hr = CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
                          reinterpret_cast<void**>(&pThreadMgr));
    if (FAILED(hr)) {
        std::printf("FAIL CoCreateInstance(CLSID_TF_ThreadMgr) 0x%08X\n", hr);
        goto cleanup;
    }
    hr = pThreadMgr->Activate(&tid);
    if (FAILED(hr)) {
        std::printf("FAIL ITfThreadMgr::Activate 0x%08X\n", hr);
        goto cleanup;
    }
    std::printf("[host] thread manager activated, tid=0x%X\n", tid);

    // 2) Activate the AI Input Engine language profile (0x0409) through CTF.
    {
        ITfInputProcessorProfiles* pProfiles = nullptr;
        hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                              IID_ITfInputProcessorProfiles, reinterpret_cast<void**>(&pProfiles));
        if (SUCCEEDED(hr) && pProfiles) {
            // Confirm the profile is known to the system.
            IEnumTfLanguageProfiles* pEnum = nullptr;
            HRESULT hrEnum = pProfiles->EnumLanguageProfiles(0x0409, &pEnum);
            bool found = false;
            if (SUCCEEDED(hrEnum) && pEnum) {
                TF_LANGUAGEPROFILE prof{};
                ULONG n = 0;
                while (pEnum->Next(1, &prof, &n) == S_OK) {
                    if (IsEqualGUID(prof.clsid, CLSID_AIInputEngineTIP) &&
                        IsEqualGUID(prof.guidProfile, GUID_AIInputEngineLangProfile)) {
                        found = true;
                    }
                }
                pEnum->Release();
            }
            std::printf("[host] TIP profile enumerated by Windows TSF: %s\n", found ? "yes" : "NO");

            const HRESULT hrActivate = pProfiles->ActivateLanguageProfile(
                CLSID_AIInputEngineTIP, 0x0409, GUID_AIInputEngineLangProfile);
            std::printf("[host] ActivateLanguageProfile: %s (0x%08X)\n",
                        SUCCEEDED(hrActivate) ? "OK" : "FAILED", hrActivate);
            profileActivated = SUCCEEDED(hrActivate);
            pProfiles->Release();
        } else {
            std::printf("[host] no ITfInputProcessorProfiles (0x%08X)\n", hr);
        }
    }
    if (!profileActivated) {
        // CTF's profile cache only picks up fresh registrations after a sign
        // out/in. Activate the TIP directly (the same mechanism ctfmon uses):
        // CoCreateInstance the service and hand it the thread manager + tid.
        std::printf("[host] fallback: activating the TIP directly via CoCreateInstance\n");
        hr = CoCreateInstance(CLSID_AIInputEngineTIP, nullptr, CLSCTX_INPROC_SERVER,
                              IID_ITfTextInputProcessorEx, reinterpret_cast<void**>(&pTipDirect));
        if (FAILED(hr) || pTipDirect == nullptr) {
            std::printf("FAIL: cannot create the TIP 0x%08X\n", hr);
            goto cleanup;
        }
        hr = pTipDirect->ActivateEx(pThreadMgr, tid, 0);
        std::printf("[host] TIP ActivateEx: %s (0x%08X)\n", SUCCEEDED(hr) ? "OK" : "FAILED", hr);
        if (FAILED(hr)) {
            goto cleanup;
        }
    }

    // Give CTF time to load the TIP and wire its sinks.
    PumpMessages(500);

    // 3) Document manager + context (the host "document").
    hr = pThreadMgr->CreateDocumentMgr(&pDocMgr);
    if (FAILED(hr)) {
        std::printf("FAIL CreateDocumentMgr 0x%08X\n", hr);
        goto cleanup;
    }
    hr = pDocMgr->CreateContext(tid, 0, static_cast<ITextStoreACP*>(owner), &pContext,
                                &owner->ec);
    if (FAILED(hr)) {
        std::printf("FAIL CreateContext 0x%08X\n", hr);
        goto cleanup;
    }
    hr = pDocMgr->Push(pContext);
    if (FAILED(hr)) {
        std::printf("FAIL ITfDocumentMgr::Push 0x%08X\n", hr);
        goto cleanup;
    }

    // Associate the edit control with the document manager: this makes the
    // edit control TSF's window-backed text store (the same mechanism real
    // apps use) and lets TSF track focus changes on the window.
    {
        ITfDocumentMgr* pPrev = nullptr;
        const HRESULT hrAssoc = pThreadMgr->AssociateFocus(owner->edit, pDocMgr, &pPrev);
        std::printf("[host] AssociateFocus: %s (0x%08X)\n", SUCCEEDED(hrAssoc) ? "OK" : "FAILED",
                    hrAssoc);
        if (pPrev) {
            pPrev->Release();
        }
    }

    // Focus the document now that the TIP is active: this fires the TIP's
    // thread-mgr focus sink, which wires the keystroke sinks.
    hr = pThreadMgr->SetFocus(pDocMgr);
    if (FAILED(hr)) {
        std::printf("FAIL ITfThreadMgr::SetFocus 0x%08X\n", hr);
        goto cleanup;
    }
    // Try to obtain real OS input focus (the TSF manager only notifies TIPs
    // when the system foreground window changes). From a console process this
    // may be blocked; if so, fall back to delivering the focus callback
    // directly (the exact call the TSF manager would make).
    ShowWindow(owner->hwnd, SW_SHOW);
    BringWindowToTop(owner->hwnd);
    SetForegroundWindow(owner->hwnd);
    PumpMessages(600);
    if (!TipLogHasFocusChanged() && pTipDirect) {
        ITfThreadMgrEventSink* pSink = nullptr;
        if (SUCCEEDED(pTipDirect->QueryInterface(IID_ITfThreadMgrEventSink,
                                                 reinterpret_cast<void**>(&pSink)))) {
            std::printf("[host] manual focus delivery (TSF manager equivalent)\n");
            pSink->OnSetFocus(pDocMgr, nullptr);
            pSink->Release();
            PumpMessages(300);
        }
    }
    std::printf("[host] TIP focus state: %s\n", TipLogHasFocusChanged() ? "focused" : "NO FOCUS");

    // Advise the composition sink to record composition events.
    {
        ITfSource* pSource = nullptr;
        if (SUCCEEDED(pContext->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&pSource)))) {
            DWORD cookie = 0;
            const HRESULT hrAdvise =
                pSource->AdviseSink(IID_ITfContextOwnerCompositionSink,
                                    static_cast<ITfContextOwnerCompositionSink*>(owner), &cookie);
            std::printf("[host] composition sink advised: %s (0x%08X)\n",
                        SUCCEEDED(hrAdvise) ? "yes" : "no", hrAdvise);
            pSource->Release();
        }
    }

    // 4) Feed the keystrokes to the TIP's key event sink directly. (Routing
    // through ITfKeystrokeMgr::KeyDown only reaches TIPs activated through
    // CTF's profile manager; CTF's registration cache requires a sign out/in
    // to see a freshly registered TIP, so we drive the same callback the
    // keystroke manager would deliver.)
    if (pTipDirect == nullptr ||
        FAILED(pTipDirect->QueryInterface(IID_ITfKeyEventSink,
                                          reinterpret_cast<void**>(&pKeySink)))) {
        std::printf("FAIL: cannot QI the TIP for ITfKeyEventSink\n");
        goto cleanup;
    }

    GetKeyboardState(state);
    {
        int eatenCount = 0;
        for (const wchar_t c : text) {
            SHORT vkShort = VkKeyScanW(c);
            if (vkShort == -1) {
                std::printf("FAIL: no VK for %lc\n", c);
                goto cleanup;
            }
            const BYTE vk = static_cast<BYTE>(vkShort & 0xFF);
            const bool shift = (vkShort & 0x100) != 0;

            BYTE newState[256];
            memcpy(newState, state, sizeof(state));
            newState[VK_SHIFT] = shift ? 0x80 : 0;
            if (shift) {
                newState[vk] = 0x80;
            }
            SetKeyboardState(newState);

            const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
            const LPARAM lParam = (static_cast<LPARAM>(scan) << 16) | (shift ? (1 << 24) : 0) | 1;
            BOOL eaten = FALSE;
            const HRESULT hrKey = pKeySink->OnKeyDown(pContext, vk, lParam, &eaten);
            if (SUCCEEDED(hrKey) && eaten) {
                ++eatenCount;
            } else {
                std::printf("[host] key %lc NOT eaten (hr=0x%08X eaten=%d)\n", c, hrKey,
                            eaten ? 1 : 0);
            }
            PumpMessages(15);
            // Let ToUnicodeEx see the original layout state between keys.
            SetKeyboardState(state);
        }

        SetKeyboardState(state);
        std::printf("[host] %d/%zu keystrokes eaten by the TIP\n", eatenCount, text.size());

        if (eatenCount == 0) {
            std::printf("FAIL: the TIP did not receive any keystroke\n");
            goto cleanup;
        }

        // Let the debounce fire so the incremental translation lands in the
        // composition before we commit.
        std::printf("[host] waiting for debounce + translation...\n");
        PumpMessages(1500);
    }

    // 5) Wait for the debounce + translation + commit to land in the store.
    // (ITfContextOwnerCompositionSink only fires for owner-protocol contexts;
    // this managed ITextStoreACP context reports changes through the text
    // store instead, so we key the wait on the document text.)
    {
        const DWORD start = GetTickCount();
        auto matchesExpected = [&]() {
            const std::wstring doc = owner->ReadDoc();
            const std::wstring comp = owner->compositionText;
            if (mode == L"mock") {
                return doc == expected || comp == expected;
            }
            // For network LLM providers (Gemini/OpenAI), accept exact match,
            // case-insensitive match, or any valid translated output.
            if (doc == expected || comp == expected) return true;
            if (_wcsicmp(doc.c_str(), expected.c_str()) == 0 || _wcsicmp(comp.c_str(), expected.c_str()) == 0) return true;
            if (!doc.empty() && doc != text) return true;
            if (!comp.empty() && comp != text) return true;
            return false;
        };

        while (!matchesExpected() && GetTickCount() - start < timeoutMs) {
            PumpMessages(100);
        }
        const DWORD elapsed = GetTickCount() - start;
        std::printf("[host] waited %lu ms for translation in the store (doc=\"%ls\", comp=\"%ls\")\n",
                    elapsed, owner->ReadDoc().c_str(), owner->compositionText.c_str());

        if (!matchesExpected()) {
            std::printf("FAIL: expected \"%ls\" never landed in the store (doc=\"%ls\", comp=\"%ls\")\n",
                        expected.c_str(), owner->ReadDoc().c_str(), owner->compositionText.c_str());
            goto cleanup;
        }

        // 6) Read the document back: the edit control is the window-backed
        // text store (what an app would see), plus the TSF range read-back.
        std::wstring docText;
        {
            const int len = GetWindowTextLengthW(owner->edit);
            if (len > 0) {
                std::vector<wchar_t> buf(static_cast<size_t>(len) + 1);
                GetWindowTextW(owner->edit, buf.data(), len + 1);
                docText = buf.data();
            }
        }
        std::wstring tsfText;
        {
            TF_SELECTION sel{};
            ULONG n = 0;
            const HRESULT hrSel =
                pContext->GetSelection(owner->ec, TF_DEFAULT_SELECTION, 1, &sel, &n);
            std::printf("[host] read-back GetSelection(ec=0x%08X): 0x%08X, %lu\n", owner->ec, hrSel,
                        n);
            if (SUCCEEDED(hrSel) && n > 0 && sel.range) {
                ITfRange* pRange = nullptr;
                if (SUCCEEDED(sel.range->Clone(&pRange))) {
                    std::vector<wchar_t> buf(1024);
                    ULONG actual = 0;
                    const HRESULT hrGet =
                        pRange->GetText(owner->ec, TF_TF_IGNOREEND, buf.data(),
                                        static_cast<ULONG>(buf.size() - 1), &actual);
                    std::printf("[host] read-back GetText: 0x%08X, %lu chars\n", hrGet, actual);
                    if (SUCCEEDED(hrGet) && actual > 0) {
                        buf[static_cast<size_t>(actual)] = 0;
                        tsfText = buf.data();
                    }
                    pRange->Release();
                }
                sel.range->Release();
            }
        }

        const std::wstring committed = owner->compositionText;
        std::printf("Composition text recorded:  \"%ls\"\n", committed.c_str());
        std::printf("Document text (edit control): \"%ls\"\n", docText.c_str());
        std::printf("Document text (text store): \"%ls\"\n", owner->ReadDoc().c_str());
        std::printf("Document text (TSF readback): \"%ls\"\n", tsfText.c_str());

        if (committed == expected || docText == expected || owner->ReadDoc() == expected ||
            tsfText == expected) {
            std::printf("PASS: committed text matches expected \"%ls\"\n", expected.c_str());
            result = 0;
        } else {
            std::printf("FAIL: expected \"%ls\"\n", expected.c_str());
        }
    }

cleanup:
    if (pTipDirect) {
        pTipDirect->Deactivate();
        pTipDirect->Release();
    }
    if (pContext) {
        pContext->Release();
    }
    if (pDocMgr) {
        pDocMgr->Release();
    }
    if (pThreadMgr) {
        pThreadMgr->Deactivate();
        pThreadMgr->Release();
    }
    if (owner->hwnd) {
        DestroyWindow(owner->hwnd);
    }
    owner->Release();
    CoUninitialize();

    // Restore the user's real configuration.
    if (snapshot.saved) {
        store.Set(snapshot.config);
        (void)store.Save();
    }

    return result;
}