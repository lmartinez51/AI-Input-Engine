#include "TSF/TextService.h"

#include "Core/Logging.h"
#include "Providers/ProviderFactory.h"
#include "Translation/TranslationCache.h"
#include "TSF/TsfGuids.h"

namespace aiie::tsf {

namespace {

bool IsNavigationVk(DWORD vk) {
    switch (vk) {
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
        return true;
    default:
        return false;
    }
}

} // namespace

TextService::TextService() = default;
TextService::~TextService() {
    if (ptim_) {
        Deactivate();
    }
}

STDMETHODIMP TextService::QueryInterface(REFIID riid, void** ppvObj) {
    if (ppvObj == nullptr) {
        return E_POINTER;
    }
    *ppvObj = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) ||
        IsEqualIID(riid, IID_ITfTextInputProcessor)) {
        *ppvObj = static_cast<ITfTextInputProcessor*>(this);
    } else if (IsEqualIID(riid, IID_ITfTextInputProcessorEx)) {
        *ppvObj = static_cast<ITfTextInputProcessorEx*>(this);
    } else if (IsEqualIID(riid, IID_ITfThreadMgrEventSink)) {
        *ppvObj = static_cast<ITfThreadMgrEventSink*>(this);
    } else if (IsEqualIID(riid, IID_ITfKeyEventSink)) {
        *ppvObj = static_cast<ITfKeyEventSink*>(this);
    } else if (IsEqualIID(riid, IID_ITfCompositionSink)) {
        *ppvObj = static_cast<ITfCompositionSink*>(this);
    } else {
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) TextService::AddRef() { return ++refCount_; }

STDMETHODIMP_(ULONG) TextService::Release() {
    const ULONG refs = --refCount_;
    if (refs == 0) {
        delete this;
    }
    return refs;
}

STDMETHODIMP TextService::Activate(ITfThreadMgr* ptim, TfClientId tid) {
    return ActivateEx(ptim, tid, 0);
}

STDMETHODIMP TextService::ActivateEx(ITfThreadMgr* ptim, TfClientId tid, DWORD dwFlags) {
    (void)dwFlags;
    if (ptim == nullptr) {
        return E_INVALIDARG;
    }
    if (ptim_) {
        return E_FAIL; // already active
    }
    ptim_ = ptim;
    ptim_->AddRef();
    tid_ = tid;

    // Load configuration (shared with the tray app).
    ConfigStore store;
    if (store.Load()) {
        config_ = store.Get();
    }
    enabled_ = config_.enabled;
    Logger::Instance().SetLevel(config_.logLevel);
    Logger::Instance().SetLogRawText(config_.logRawText);
    wchar_t envDebug[8] = {};
    if (GetEnvironmentVariableW(L"AIIE_TSF_DEBUG", envDebug, 8) > 0 && envDebug[0] == L'1') {
        wchar_t temp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp);
        Logger::Instance().SetLogFile(std::wstring(temp) + L"aiie_tip.log");
        Logger::Instance().SetLevel(LogLevel::Trace);
    } else if (config_.logLevel >= LogLevel::Debug) {
        wchar_t temp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp);
        Logger::Instance().SetLogFile(std::wstring(temp) + L"aiie_tip.log");
    }

    // Wiring: dispatcher + pipeline + engine (owned per activation).
    dispatcher_ = std::make_shared<MessageDispatcher>();
    auto cache = std::make_shared<TranslationCache>(static_cast<size_t>(config_.cacheMaxEntries));
    cache->Configure(config_.cacheEnabled, static_cast<size_t>(config_.cacheMaxEntries));
    auto provider = ProviderFactory::Create(config_);
    pipeline_ = std::make_shared<TranslationPipeline>(provider, cache, config_.requestTimeoutMs);
    statusWindow_ = std::make_unique<StatusWindow>();
    auto backend = std::make_shared<TsfInputBackend>(this);
    engine_ = std::make_shared<InputEngine>(dispatcher_, backend, pipeline_, nullptr);
    engine_->ApplyConfig(config_);

    Logger::Instance().Info(cat::TSF, L"text service activated (tid {:#x}, provider {})", tid_,
                            provider->Name());

    return InitSinks(ptim);
}

STDMETHODIMP TextService::Deactivate() {
    Logger::Instance().Info(cat::TSF, L"text service deactivating");
    ClearSinks();
    if (engine_) {
        engine_->FlushBuffer();
        engine_.reset();
    }
    statusWindow_.reset();
    pipeline_.reset();
    dispatcher_.reset();
    if (ptim_) {
        ptim_->Release();
        ptim_ = nullptr;
    }
    return S_OK;
}

HRESULT TextService::InitSinks(ITfThreadMgr* ptim) {
    HRESULT hr;

    // Register the display attribute provider category (per activation).
    ITfCategoryMgr* pCategoryMgr = nullptr;
    hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfCategoryMgr,
                          reinterpret_cast<void**>(&pCategoryMgr));
    if (SUCCEEDED(hr) && pCategoryMgr) {
        pCategoryMgr->RegisterCategory(CLSID_AIInputEngineTIP, GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER,
                                       CLSID_AIInputEngineTIP);
        pCategoryMgr->RegisterGUID(GUID_AIInputEngineDisplayAttr, &displayAttrAtom_);
        pCategoryMgr->Release();
    }

    // Thread manager event sink.
    ITfSource* pSource = nullptr;
    hr = ptim->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&pSource));
    if (FAILED(hr) || pSource == nullptr) {
        return E_FAIL;
    }
    hr = pSource->AdviseSink(IID_ITfThreadMgrEventSink,
                             static_cast<ITfThreadMgrEventSink*>(this), &threadMgrSinkCookie_);
    pSource->Release();
    return hr;
}

void TextService::ClearSinks() {
    UnadviseKeySinks();
    if (ptim_ && threadMgrSinkCookie_ != TF_INVALID_COOKIE) {
        ITfSource* pSource = nullptr;
        if (SUCCEEDED(ptim_->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&pSource)))) {
            pSource->UnadviseSink(threadMgrSinkCookie_);
            pSource->Release();
        }
        threadMgrSinkCookie_ = TF_INVALID_COOKIE;
    }
    if (composition_) {
        composition_->Release();
        composition_ = nullptr;
    }
    if (compServices_) {
        compServices_->Release();
        compServices_ = nullptr;
    }
    managedContext_ = false;
    if (focusContext_) {
        focusContext_->Release();
        focusContext_ = nullptr;
    }
}

void TextService::UpdateFocusContext(ITfDocumentMgr* pdim) {
    UnadviseKeySinks();
    if (focusContext_) {
        focusContext_->Release();
        focusContext_ = nullptr;
    }
    if (compServices_) {
        compServices_->Release();
        compServices_ = nullptr;
    }
    managedContext_ = false;
    if (composition_) {
        composition_->Release();
        composition_ = nullptr;
    }
    compositionText_.clear();

    if (pdim == nullptr) {
        return;
    }
    ITfContext* pic = nullptr;
    if (FAILED(pdim->GetTop(&pic)) || pic == nullptr) {
        return;
    }
    focusContext_ = pic;
    // Owner-protocol contexts expose ITfContextOwner (created by the app via
    // CreateContext(tid, IID_ITfContextOwner, ...)); managed contexts are
    // backed by an ITextStoreACP and do not. The context type decides which
    // composition path to use below.
    {
        ITfContextOwner* pOwner = nullptr;
        const bool ownerProtocol =
            SUCCEEDED(pic->QueryInterface(IID_ITfContextOwner, reinterpret_cast<void**>(&pOwner))) &&
            pOwner != nullptr;
        if (pOwner) {
            pOwner->Release();
        }
        managedContext_ = !ownerProtocol;
    }
    if (!managedContext_ &&
        FAILED(pic->QueryInterface(IID_ITfContextOwnerCompositionServices,
                                   reinterpret_cast<void**>(&compServices_)))) {
        compServices_ = nullptr;
    }
    Logger::Instance().Debug(cat::TSF, L"focus context type: {} (compServices {})",
                             managedContext_ ? L"managed" : L"owner",
                             compServices_ ? L"yes" : L"no");
    AdviseKeySinks();
}

HRESULT TextService::AdviseKeySinks() {
    if (ptim_ == nullptr) {
        return E_FAIL;
    }
    ITfKeystrokeMgr* pKeystrokeMgr = nullptr;
    HRESULT hr = ptim_->QueryInterface(IID_ITfKeystrokeMgr, reinterpret_cast<void**>(&pKeystrokeMgr));
    if (FAILED(hr) || pKeystrokeMgr == nullptr) {
        Logger::Instance().Warn(cat::TSF, L"AdviseKeySinks: no ITfKeystrokeMgr (0x{:08X})", hr);
        return E_FAIL;
    }
    hr = pKeystrokeMgr->AdviseKeyEventSink(tid_, static_cast<ITfKeyEventSink*>(this), TRUE);
    Logger::Instance().Debug(cat::TSF, L"AdviseKeyEventSink result 0x{:08X}", hr);
    pKeystrokeMgr->Release();
    return hr;
}

void TextService::UnadviseKeySinks() {
    if (ptim_ == nullptr) {
        return;
    }
    ITfKeystrokeMgr* pKeystrokeMgr = nullptr;
    if (SUCCEEDED(ptim_->QueryInterface(IID_ITfKeystrokeMgr, reinterpret_cast<void**>(&pKeystrokeMgr)))) {
        pKeystrokeMgr->UnadviseKeyEventSink(tid_);
        pKeystrokeMgr->Release();
    }
}

// ---------------------------------------------------------------------------
// ITfThreadMgrEventSink
// ---------------------------------------------------------------------------

STDMETHODIMP TextService::OnInitDocumentMgr(ITfDocumentMgr*) { return S_OK; }
STDMETHODIMP TextService::OnUninitDocumentMgr(ITfDocumentMgr*) { return S_OK; }

STDMETHODIMP TextService::OnSetFocus(ITfDocumentMgr* pdimFocus, ITfDocumentMgr* pdimPrevFocus) {
    (void)pdimPrevFocus;
    ecCookie_ = TF_INVALID_EDIT_COOKIE;
    UpdateFocusContext(pdimFocus);
    Logger::Instance().Debug(cat::TSF, L"focus changed (context {})",
                             focusContext_ ? L"set" : L"none");
    return S_OK;
}

STDMETHODIMP TextService::OnPushContext(ITfContext* pic) {
    (void)pic;
    ecCookie_ = TF_INVALID_EDIT_COOKIE;
    return S_OK;
}

STDMETHODIMP TextService::OnPopContext(ITfContext*) {
    ecCookie_ = TF_INVALID_EDIT_COOKIE;
    return S_OK;
}

// ---------------------------------------------------------------------------
// ITfKeyEventSink
// ---------------------------------------------------------------------------

STDMETHODIMP TextService::OnSetFocus(BOOL) { return S_OK; }

STDMETHODIMP TextService::OnTestKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
    *pfEaten = FALSE;
    if (pic == nullptr || pfEaten == nullptr || !enabled_ || !engine_) {
        return S_OK;
    }
    const DWORD vk = static_cast<DWORD>(wParam);
    if (vk == VK_BACK || vk == VK_RETURN || IsNavigationVk(vk)) {
        // Handled keys when composing; ignored otherwise (decided in OnKeyDown).
        return S_OK;
    }
    if (!CharForVk(wParam, lParam).empty()) {
        *pfEaten = TRUE;
    }
    return S_OK;
}

STDMETHODIMP TextService::OnTestKeyUp(ITfContext*, WPARAM, LPARAM, BOOL* pfEaten) {
    *pfEaten = FALSE;
    return S_OK;
}

STDMETHODIMP TextService::OnKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
    *pfEaten = FALSE;
    if (pic == nullptr || !engine_) {
        return S_FALSE;
    }
    const DWORD vk = static_cast<DWORD>(wParam);

    if (!enabled_) {
        // Engine disabled: behave like a normal keyboard.
        return S_FALSE;
    }

    switch (vk) {
    case VK_RETURN:
        // Commit any live composition, then let the app handle Enter (so the
        // newline format is always correct for the target application).
        if (composition_) {
            engine_->HandleCommitKey();
            CommitComposition();
        }
        return S_OK;
    case VK_BACK:
        if (composition_) {
            engine_->HandleBackspace();
            *pfEaten = TRUE;
            return S_OK;
        }
        return S_FALSE;
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
        // Navigation: commit the composition untouched, let the app move.
        if (composition_) {
            engine_->HandleCommitKey();
            CommitComposition();
        }
        return S_OK;
    default:
        break;
    }

    const std::wstring chars = CharForVk(wParam, lParam);
    Logger::Instance().Debug(cat::TSF, L"key down vk={:#x} enabled={} chars={} eaten={}",
                             vk, enabled_ ? 1 : 0, chars.size(), *pfEaten ? 1 : 0);
    if (chars.empty()) {
        return S_FALSE; // not a printable key from this layout
    }
    *pfEaten = TRUE;
    for (const wchar_t c : chars) {
        if (c == L'.' || c == L'!' || c == L'?') {
            engine_->HandleTerminator(c);
        } else {
            engine_->HandleCharacter(c);
        }
    }
    return S_OK;
}

STDMETHODIMP TextService::OnKeyUp(ITfContext*, WPARAM, LPARAM, BOOL* pfEaten) {
    *pfEaten = FALSE;
    return S_OK;
}

STDMETHODIMP TextService::OnPreservedKey(ITfContext*, REFGUID, BOOL* pfEaten) {
    *pfEaten = FALSE;
    return S_OK;
}

std::wstring TextService::CharForVk(WPARAM wParam, LPARAM lParam) const {
    // In-process: the service thread receives the key events in order, so
    // GetKeyboardState + ToUnicodeEx resolve the layout, shift state and
    // dead-key composition correctly.
    BYTE state[256] = {};
    if (!GetKeyboardState(state)) {
        return {};
    }
    const DWORD scan = (static_cast<DWORD>(lParam) >> 16) & 0xFF;
    wchar_t buf[8] = {};
    const int count = ToUnicodeEx(static_cast<UINT>(wParam), scan, state, buf, 8,
                                  (lParam & (1 << 24)) ? 1u : 0u, GetKeyboardLayout(0));
    if (count <= 0) {
        // Fall back to a static ASCII VK -> character table. The thread's
        // keyboard layout while a text service is the active input profile is
        // not guaranteed to map VK codes the way ToUnicodeEx expects (the
        // service may own the thread layout), so never drop a printable key.
        const BYTE vk = static_cast<BYTE>(wParam);
        if (vk >= '0' && vk <= '9') {
            return std::wstring(1, static_cast<wchar_t>(vk));
        }
        if (vk >= 'A' && vk <= 'Z') {
            const bool caps = (GetKeyState(VK_CAPITAL) & 1) != 0;
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            wchar_t c = static_cast<wchar_t>(vk);
            if (caps != shift) {
                c += L'a' - L'A';
            }
            return std::wstring(1, c);
        }
        switch (vk) {
        case VK_SPACE:
            return L" ";
        case VK_OEM_PERIOD:
            return L".";
        case VK_OEM_COMMA:
            return L",";
        case VK_OEM_MINUS:
            return L"-";
        default:
            return {};
        }
    }
    // Filter control characters.
    std::wstring out;
    for (int i = 0; i < count; ++i) {
        if (buf[i] >= 0x20 || buf[i] == L'\t') {
            out.push_back(buf[i]);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// ITfCompositionSink
// ---------------------------------------------------------------------------

STDMETHODIMP TextService::OnCompositionTerminated(TfEditCookie, ITfComposition* pComposition) {
    if (composition_ == pComposition) {
        Logger::Instance().Debug(cat::TSF, L"composition terminated externally");
        composition_ = nullptr;
        compositionText_.clear();
        if (engine_) {
            engine_->FlushBuffer();
        }
    }
    return S_OK;
}

// ---------------------------------------------------------------------------
// Composition operations
// ---------------------------------------------------------------------------

// One-shot edit session adapter: runs a lambda with the valid edit cookie.
class EditSession : public ITfEditSession {
public:
    using Action = std::function<HRESULT(TfEditCookie)>;
    explicit EditSession(Action action) : action_(std::move(action)) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override {
        if (ppvObj == nullptr) {
            return E_POINTER;
        }
        *ppvObj = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
            *ppvObj = static_cast<ITfEditSession*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refCount_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG refs = --refCount_;
        if (refs == 0) {
            delete this;
        }
        return refs;
    }
    STDMETHODIMP DoEditSession(TfEditCookie ec) override { return action_(ec); }

private:
    Action action_;
    ULONG refCount_ = 1;
};

HRESULT TextService::RunEditSession(ITfContext* pic, DWORD flags,
                                    std::function<HRESULT(TfEditCookie)> action) {
    if (pic == nullptr) {
        return E_FAIL;
    }
    auto* session = new EditSession(std::move(action));
    HRESULT hrEdit = E_FAIL;
    const HRESULT hr = pic->RequestEditSession(tid_, session, flags, &hrEdit);
    session->Release();
    return FAILED(hr) ? hr : hrEdit;
}

HRESULT TextService::GetSelectionRange(ITfContext* pic, TfEditCookie ec, ITfRange** ppRange) {
    *ppRange = nullptr;
    TF_SELECTION selection{};
    ULONG fetched = 0;
    const HRESULT hr = pic->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
    if (SUCCEEDED(hr) && fetched > 0 && selection.range != nullptr) {
        *ppRange = selection.range;
        return S_OK;
    }
    return hr;
}

void TextService::EnsureComposition() {
    if (composition_ || focusContext_ == nullptr) {
        return;
    }
    ITfContext* pic = focusContext_;
    pic->AddRef();
    (void)RunEditSession(pic, TF_ES_READWRITE | TF_ES_SYNC, [this, pic](TfEditCookie ec) -> HRESULT {
        ITfRange* pRange = nullptr;
        HRESULT hr = GetSelectionRange(pic, ec, &pRange);
        if (FAILED(hr) || pRange == nullptr) {
            Logger::Instance().Debug(cat::TSF,
                                     L"EnsureComposition: GetSelectionRange hr=0x{:08X}",
                                     FAILED(hr) ? hr : 0x80004005L);
            return hr;
        }
        pRange->Collapse(ec, TF_ANCHOR_START);
        ITfComposition* pComposition = nullptr;
        hr = StartComposition(ec, pRange, static_cast<ITfCompositionSink*>(this), &pComposition);
        Logger::Instance().Debug(cat::TSF, L"EnsureComposition: StartComposition hr=0x{:08X}",
                                 hr);
        pRange->Release();
        if (SUCCEEDED(hr) && pComposition != nullptr) {
            composition_ = pComposition;
            compositionText_.clear();
            Logger::Instance().Trace(cat::TSF, L"composition started");
        }
        return hr;
    });
    pic->Release();
}

HRESULT TextService::StartComposition(TfEditCookie ec, ITfRange* pRange,
                                      ITfCompositionSink* pSink, ITfComposition** ppComposition) {
    if (compServices_) {
        return compServices_->StartComposition(ec, pRange, pSink, ppComposition);
    }
    // Managed (text-store) contexts: compose through ITfContextComposition.
    ITfContextComposition* pContextComposition = nullptr;
    HRESULT hr = focusContext_->QueryInterface(IID_ITfContextComposition,
                                               reinterpret_cast<void**>(&pContextComposition));
    if (FAILED(hr) || pContextComposition == nullptr) {
        return hr;
    }
    hr = pContextComposition->StartComposition(ec, pRange, pSink, ppComposition);
    pContextComposition->Release();
    return hr;
}

void TextService::TerminateComposition(ITfCompositionView* pView) {
    if (compServices_) {
        compServices_->TerminateComposition(pView);
        return;
    }
    // Managed (text-store) context: the composition is ended through the
    // TIP's own composition object, inside an edit session.
    ITfContext* pic = focusContext_;
    if (pic == nullptr || composition_ == nullptr) {
        return;
    }
    pic->AddRef();
    (void)RunEditSession(pic, TF_ES_READWRITE | TF_ES_SYNC, [this](TfEditCookie ec) -> HRESULT {
        return composition_ ? composition_->EndComposition(ec) : S_OK;
    });
    pic->Release();
}

void TextService::SyncComposition(const std::wstring& text) {
    if (composition_ == nullptr) {
        EnsureComposition();
        if (composition_ == nullptr) {
            return; // app does not support composition; text stays uncomposed
        }
    }
    if (text.empty()) {
        CommitComposition();
        return;
    }
    if (compositionText_ == text) {
        return;
    }
    ITfContext* pic = focusContext_;
    if (pic == nullptr) {
        return;
    }
    pic->AddRef();
    const bool ok = SUCCEEDED(RunEditSession(pic, TF_ES_READWRITE | TF_ES_SYNC,
                                             [this, text](TfEditCookie ec) -> HRESULT {
        const HRESULT hr = SetCompositionText(ec, text);
        if (SUCCEEDED(hr)) {
            compositionText_ = text;
        }
        return hr;
    }));
    pic->Release();
    if (!ok) {
        Logger::Instance().Warn(cat::TSF, L"edit session for composition text failed");
    }
}

void TextService::CommitComposition() {
    if (composition_ == nullptr) {
        return;
    }
    // For context-owner compositions, terminating commits the text (the
    // composed text was already inserted into the document by SetText); for
    // managed contexts the composition ends through ITfComposition::End.
    {
        ITfCompositionView* pView = nullptr;
        if (SUCCEEDED(composition_->QueryInterface(IID_ITfCompositionView,
                                                   reinterpret_cast<void**>(&pView)))) {
            TerminateComposition(pView);
            pView->Release();
        }
    }
    composition_ = nullptr;
    compositionText_.clear();
    Logger::Instance().Trace(cat::TSF, L"composition committed");
}

HRESULT TextService::SetCompositionTextFull(TfEditCookie ec, const std::wstring& text) {
    if (composition_ == nullptr || focusContext_ == nullptr) {
        return E_FAIL;
    }
    ITfRange* pRange = nullptr;
    HRESULT hr = composition_->GetRange(&pRange);
    if (FAILED(hr) || pRange == nullptr) {
        return hr;
    }
    if (compServices_) {
        // Owner-created contexts (ITfContextOwner protocol): the owner's
        // document is not the TSF range store, so range SetText is a no-op.
        // Replace the composition content by collapsing to its start and
        // inserting the new text through the insert-at-selection interface.
        ITfRange* pInserted = nullptr;
        hr = pRange->Collapse(ec, TF_ANCHOR_START);
        if (SUCCEEDED(hr)) {
            TF_SELECTION sel{};
            sel.range = pRange;
            sel.style.ase = TF_AE_START;
            sel.style.fInterimChar = FALSE;
            hr = focusContext_->SetSelection(ec, 1, &sel);
        }
        ITfInsertAtSelection* pInsert = nullptr;
        if (SUCCEEDED(hr)) {
            hr = focusContext_->QueryInterface(IID_ITfInsertAtSelection,
                                               reinterpret_cast<void**>(&pInsert));
        }
        if (SUCCEEDED(hr) && pInsert) {
            hr = pInsert->InsertTextAtSelection(ec, 0, text.data(), static_cast<LONG>(text.size()),
                                                &pInserted);
            pInsert->Release();
        }
        if (SUCCEEDED(hr) && pInserted) {
            SetDisplayAttribute(ec, pInserted);
            pInserted->Release();
        }
        pRange->Release();
        return hr;
    }
    // Managed context: Replace the whole composition range with the new text.
    hr = pRange->SetText(ec, 0, text.data(), static_cast<LONG>(text.size()));
    Logger::Instance().Trace(cat::TSF, L"SetText on composition range: 0x{:08X} ({})", hr,
                             text.size());
    if (SUCCEEDED(hr)) {
        SetDisplayAttribute(ec, pRange);
    }
    pRange->Release();
    return hr;
}

HRESULT TextService::AppendCompositionText(TfEditCookie ec, const std::wstring& delta) {
    if (composition_ == nullptr || focusContext_ == nullptr || delta.empty()) {
        return E_FAIL;
    }
    ITfRange* pRange = nullptr;
    HRESULT hr = composition_->GetRange(&pRange);
    if (FAILED(hr) || pRange == nullptr) {
        return hr;
    }
    if (compServices_) {
        // Owner-protocol context: collapse to END and insert delta
        ITfRange* pInserted = nullptr;
        hr = pRange->Collapse(ec, TF_ANCHOR_END);
        if (SUCCEEDED(hr)) {
            TF_SELECTION sel{};
            sel.range = pRange;
            sel.style.ase = TF_AE_END;
            sel.style.fInterimChar = FALSE;
            hr = focusContext_->SetSelection(ec, 1, &sel);
        }
        ITfInsertAtSelection* pInsert = nullptr;
        if (SUCCEEDED(hr)) {
            hr = focusContext_->QueryInterface(IID_ITfInsertAtSelection,
                                               reinterpret_cast<void**>(&pInsert));
        }
        if (SUCCEEDED(hr) && pInsert) {
            hr = pInsert->InsertTextAtSelection(ec, 0, delta.data(), static_cast<LONG>(delta.size()),
                                                &pInserted);
            pInsert->Release();
        }
        if (SUCCEEDED(hr) && pInserted) {
            SetDisplayAttribute(ec, pInserted);
            pInserted->Release();
        }
        pRange->Release();
        return hr;
    }

    // Managed context: collapse to END, insert delta, and shift composition end
    hr = pRange->Collapse(ec, TF_ANCHOR_END);
    if (SUCCEEDED(hr)) {
        hr = pRange->SetText(ec, 0, delta.data(), static_cast<LONG>(delta.size()));
    }
    if (SUCCEEDED(hr)) {
        composition_->ShiftEnd(ec, pRange);
        SetDisplayAttribute(ec, pRange);
    }
    pRange->Release();
    return hr;
}

HRESULT TextService::TruncateCompositionText(TfEditCookie ec, size_t trimCount) {
    if (composition_ == nullptr || focusContext_ == nullptr || trimCount == 0) {
        return E_FAIL;
    }
    ITfRange* pRange = nullptr;
    HRESULT hr = composition_->GetRange(&pRange);
    if (FAILED(hr) || pRange == nullptr) {
        return hr;
    }
    if (compServices_) {
        // Owner-protocol context: fall back to full replacement of remaining text
        pRange->Release();
        return E_NOTIMPL;
    }

    // Managed context: collapse to END, shift start backwards by trimCount, and delete tail
    hr = pRange->Collapse(ec, TF_ANCHOR_END);
    if (SUCCEEDED(hr)) {
        LONG shifted = 0;
        hr = pRange->ShiftStart(ec, -static_cast<LONG>(trimCount), &shifted, nullptr);
        if (SUCCEEDED(hr) && shifted != 0) {
            hr = pRange->SetText(ec, 0, nullptr, 0);
            if (SUCCEEDED(hr)) {
                composition_->ShiftEnd(ec, pRange);
            }
        }
    }
    pRange->Release();
    return hr;
}

HRESULT TextService::SetCompositionText(TfEditCookie ec, const std::wstring& text) {
    if (composition_ == nullptr || focusContext_ == nullptr) {
        return E_FAIL;
    }
    if (text == compositionText_) {
        return S_OK;
    }

    const std::wstring& oldText = compositionText_;

    // Case 1: Incremental Append (text strictly extends oldText)
    if (!oldText.empty() && text.size() > oldText.size() &&
        text.compare(0, oldText.size(), oldText) == 0) {
        const std::wstring delta = text.substr(oldText.size());
        const HRESULT hr = AppendCompositionText(ec, delta);
        if (SUCCEEDED(hr)) {
            return S_OK;
        }
        // Fall back to full replacement if incremental append fails
    }
    // Case 2: Incremental Truncation / Backspace (text is a strict prefix of oldText)
    else if (!text.empty() && text.size() < oldText.size() &&
             oldText.compare(0, text.size(), text) == 0) {
        const size_t trimCount = oldText.size() - text.size();
        const HRESULT hr = TruncateCompositionText(ec, trimCount);
        if (SUCCEEDED(hr)) {
            return S_OK;
        }
        // Fall back to full replacement if incremental truncation fails
    }

    // Case 3: Initial insertion, final LLM replacement, or non-prefix transformation
    return SetCompositionTextFull(ec, text);
}

void TextService::SetDisplayAttribute(TfEditCookie ec, ITfRange* pRange) {
    if (displayAttrAtom_ == TF_INVALID_GUIDATOM || focusContext_ == nullptr) {
        return;
    }
    ITfProperty* pProperty = nullptr;
    if (FAILED(focusContext_->GetProperty(GUID_PROP_ATTRIBUTE, &pProperty)) || pProperty == nullptr) {
        return;
    }
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_I4;
    var.lVal = static_cast<LONG>(displayAttrAtom_);
    pProperty->SetValue(ec, pRange, &var);
    pProperty->Release();
}

// ---------------------------------------------------------------------------
// TsfInputBackend
// ---------------------------------------------------------------------------

void TsfInputBackend::CommitReplacement(std::wstring replacement, const std::wstring&) {
    service_->SyncComposition(replacement);
}

void TsfInputBackend::OnBufferStarted() {
    service_->EnsureComposition();
}

void TsfInputBackend::OnBufferChanged(std::wstring_view buffer) {
    service_->SyncComposition(std::wstring(buffer));
}

void TsfInputBackend::OnBufferCommitted() {
    service_->CommitComposition();
}

void TsfInputBackend::ShowStatus(std::wstring_view message) {
    if (service_->statusWindow_) {
        service_->statusWindow_->Show(message);
    }
}

void TsfInputBackend::HideStatus() {
    if (service_->statusWindow_) {
        service_->statusWindow_->Hide();
    }
}

} // namespace aiie::tsf