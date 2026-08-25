#pragma once

#include "Composition/InputEngine.h"
#include "Configuration/ConfigStore.h"
#include "Core/Dispatcher.h"
#include "Core/StatusWindow.h"
#include "Translation/TranslationPipeline.h"

#include <msctf.h>

#include <functional>
#include <memory>
#include <string>

namespace aiie::tsf {

// The TSF text service. Loaded by the TSF manager into the target process on
// the thread that becomes a "text input" thread (typically the app's UI
// thread). Implements:
//   - ITfTextInputProcessorEx / ITfTextInputProcessor (activation)
//   - ITfThreadMgrEventSink (focus tracking)
//   - ITfKeyEventSink (keystroke interception)
//   - ITfCompositionSink (external composition termination)
//
// The service drives the shared InputEngine; the TsfInputBackend adapts the
// engine to in-process composition operations (StartComposition / SetText /
// TerminateComposition). All TSF calls happen on the service thread.
class TextService : public ITfTextInputProcessorEx, public ITfThreadMgrEventSink,
                    public ITfKeyEventSink, public ITfCompositionSink {
public:
    TextService();
    virtual ~TextService();

    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // ITfTextInputProcessor
    STDMETHODIMP Activate(ITfThreadMgr* ptim, TfClientId tid) override;
    STDMETHODIMP Deactivate() override;
    // ITfTextInputProcessorEx
    STDMETHODIMP ActivateEx(ITfThreadMgr* ptim, TfClientId tid, DWORD dwFlags) override;

    // ITfThreadMgrEventSink
    STDMETHODIMP OnInitDocumentMgr(ITfDocumentMgr* pdim) override;
    STDMETHODIMP OnUninitDocumentMgr(ITfDocumentMgr* pdim) override;
    STDMETHODIMP OnSetFocus(ITfDocumentMgr* pdimFocus, ITfDocumentMgr* pdimPrevFocus) override;
    STDMETHODIMP OnPushContext(ITfContext* pic) override;
    STDMETHODIMP OnPopContext(ITfContext* pic) override;

    // ITfKeyEventSink
    STDMETHODIMP OnSetFocus(BOOL fForeground) override;
    STDMETHODIMP OnTestKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnTestKeyUp(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnKeyUp(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnPreservedKey(ITfContext* pic, REFGUID rguid, BOOL* pfEaten) override;

    // ITfCompositionSink
    STDMETHODIMP OnCompositionTerminated(TfEditCookie ecWrite, ITfComposition* pComposition) override;

    // --- Composition operations (called by TsfInputBackend) ---
    void EnsureComposition();
    void SyncComposition(const std::wstring& text);
    void CommitComposition();

private:
    friend class TsfInputBackend;

    HRESULT InitSinks(ITfThreadMgr* ptim);
    void ClearSinks();
    void UpdateFocusContext(ITfDocumentMgr* pdim);
    HRESULT AdviseKeySinks();
    void UnadviseKeySinks();
    std::wstring CharForVk(WPARAM wParam, LPARAM lParam) const;
    HRESULT SetCompositionText(TfEditCookie ec, const std::wstring& text);
    HRESULT SetCompositionTextFull(TfEditCookie ec, const std::wstring& text);
    HRESULT AppendCompositionText(TfEditCookie ec, const std::wstring& delta);
    HRESULT TruncateCompositionText(TfEditCookie ec, size_t trimCount);
    HRESULT StartComposition(TfEditCookie ec, ITfRange* pRange, ITfCompositionSink* pSink,
                             ITfComposition** ppComposition);
    void TerminateComposition(ITfCompositionView* pView);
    void SetDisplayAttribute(TfEditCookie ec, ITfRange* pRange);
    HRESULT GetSelectionRange(ITfContext* pic, TfEditCookie ec, ITfRange** ppRange);
    HRESULT RunEditSession(ITfContext* pic, DWORD flags, std::function<HRESULT(TfEditCookie)> action);

    ULONG refCount_ = 1;

    ITfThreadMgr* ptim_ = nullptr;
    TfClientId tid_ = 0;
    DWORD threadMgrSinkCookie_ = TF_INVALID_COOKIE;
    ITfContext* focusContext_ = nullptr;
    TfEditCookie ecCookie_ = TF_INVALID_EDIT_COOKIE;

    ITfContextOwnerCompositionServices* compServices_ = nullptr;
    bool managedContext_ = false; // context is backed by an ITextStoreACP
    ITfComposition* composition_ = nullptr;
    TfGuidAtom displayAttrAtom_ = TF_INVALID_GUIDATOM;

    // Engine + pipeline (owned by the service instance).
    std::shared_ptr<MessageDispatcher> dispatcher_;
    std::shared_ptr<TranslationPipeline> pipeline_;
    std::shared_ptr<InputEngine> engine_;
    std::unique_ptr<StatusWindow> statusWindow_;
    AppConfig config_;
    bool enabled_ = true;

    std::wstring compositionText_; // current composition text (UTF-16)
};

// Adapter from the engine to the TSF composition operations.
class TsfInputBackend : public IInputBackend {
public:
    explicit TsfInputBackend(TextService* service) : service_(service) {}

    void CommitReplacement(std::wstring replacement, const std::wstring& expectedSource) override;
    void OnBufferStarted() override;
    void OnBufferChanged(std::wstring_view buffer) override;
    void OnBufferCommitted() override;
    void ShowStatus(std::wstring_view message) override;
    void HideStatus() override;

private:
    TextService* service_;
};

} // namespace aiie::tsf