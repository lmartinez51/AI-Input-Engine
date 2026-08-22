#include "TSF/DisplayAttributeInfo.h"
#include "TSF/TsfGuids.h"

namespace aiie::tsf {

namespace {
void SetGuidAtomValue(VARIANT* pvar, TfGuidAtom atom) {
    pvar->vt = VT_I4;
    pvar->lVal = static_cast<LONG>(atom);
}
} // namespace

// ---------------------------------------------------------------------------
// DisplayAttributeInfo
// ---------------------------------------------------------------------------

DisplayAttributeInfo::DisplayAttributeInfo() = default;
DisplayAttributeInfo::~DisplayAttributeInfo() = default;

STDMETHODIMP DisplayAttributeInfo::QueryInterface(REFIID riid, void** ppvObj) {
    if (ppvObj == nullptr) {
        return E_POINTER;
    }
    *ppvObj = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfDisplayAttributeInfo)) {
        *ppvObj = static_cast<ITfDisplayAttributeInfo*>(this);
    } else {
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) DisplayAttributeInfo::AddRef() { return ++refCount_; }

STDMETHODIMP_(ULONG) DisplayAttributeInfo::Release() {
    const ULONG refs = --refCount_;
    if (refs == 0) {
        delete this;
    }
    return refs;
}

STDMETHODIMP DisplayAttributeInfo::GetGUID(GUID* pguid) {
    if (pguid == nullptr) {
        return E_POINTER;
    }
    *pguid = GUID_AIInputEngineDisplayAttr;
    return S_OK;
}

STDMETHODIMP DisplayAttributeInfo::GetDescription(BSTR* pbstrDesc) {
    if (pbstrDesc == nullptr) {
        return E_POINTER;
    }
    *pbstrDesc = SysAllocString(L"AI Input Engine composition");
    return *pbstrDesc ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP DisplayAttributeInfo::GetAttributeInfo(TF_DISPLAYATTRIBUTE* pAttr) {
    if (pAttr == nullptr) {
        return E_POINTER;
    }
    ZeroMemory(pAttr, sizeof(*pAttr));
    // Underline the composition text (like native IMEs); keep default colors.
    pAttr->crText.type = TF_CT_NONE;
    pAttr->crBk.type = TF_CT_NONE;
    pAttr->lsStyle = TF_LS_SOLID;
    pAttr->fBoldLine = FALSE;
    pAttr->crLine.type = TF_CT_COLORREF;
    pAttr->crLine.cr = RGB(0, 120, 200);
    pAttr->bAttr = TF_ATTR_INPUT;
    return S_OK;
}

STDMETHODIMP DisplayAttributeInfo::SetAttributeInfo(const TF_DISPLAYATTRIBUTE*) { return E_NOTIMPL; }
STDMETHODIMP DisplayAttributeInfo::Reset() { return S_OK; }

// ---------------------------------------------------------------------------
// DisplayAttributeProvider
// ---------------------------------------------------------------------------

STDMETHODIMP DisplayAttributeProvider::QueryInterface(REFIID riid, void** ppvObj) {
    if (ppvObj == nullptr) {
        return E_POINTER;
    }
    *ppvObj = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfDisplayAttributeProvider)) {
        *ppvObj = static_cast<ITfDisplayAttributeProvider*>(this);
    } else {
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) DisplayAttributeProvider::AddRef() { return ++refCount_; }

STDMETHODIMP_(ULONG) DisplayAttributeProvider::Release() {
    const ULONG refs = --refCount_;
    if (refs == 0) {
        delete this;
    }
    return refs;
}

STDMETHODIMP DisplayAttributeProvider::EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** ppEnum) {
    if (ppEnum == nullptr) {
        return E_POINTER;
    }
    auto* item = new DisplayAttributeInfo();
    std::vector<ITfDisplayAttributeInfo*> items{item};
    *ppEnum = new ::aiie::tsf::EnumDisplayAttributeInfo(std::move(items));
    return S_OK;
}

STDMETHODIMP DisplayAttributeProvider::GetDisplayAttributeInfo(REFGUID guid,
                                                               ITfDisplayAttributeInfo** ppInfo) {
    if (ppInfo == nullptr) {
        return E_POINTER;
    }
    *ppInfo = nullptr;
    if (!IsEqualGUID(guid, GUID_AIInputEngineDisplayAttr)) {
        return E_INVALIDARG;
    }
    auto* info = new DisplayAttributeInfo();
    if (FAILED(info->QueryInterface(IID_ITfDisplayAttributeInfo, reinterpret_cast<void**>(ppInfo)))) {
        delete info;
        return E_OUTOFMEMORY;
    }
    info->Release(); // QI added a reference
    return S_OK;
}

// ---------------------------------------------------------------------------
// EnumDisplayAttributeInfo
// ---------------------------------------------------------------------------

EnumDisplayAttributeInfo::EnumDisplayAttributeInfo(std::vector<ITfDisplayAttributeInfo*> items)
    : items_(std::move(items)) {}

STDMETHODIMP EnumDisplayAttributeInfo::QueryInterface(REFIID riid, void** ppvObj) {
    if (ppvObj == nullptr) {
        return E_POINTER;
    }
    *ppvObj = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IEnumTfDisplayAttributeInfo)) {
        *ppvObj = static_cast<IEnumTfDisplayAttributeInfo*>(this);
    } else {
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) EnumDisplayAttributeInfo::AddRef() { return ++refCount_; }

STDMETHODIMP_(ULONG) EnumDisplayAttributeInfo::Release() {
    const ULONG refs = --refCount_;
    if (refs == 0) {
        delete this;
    }
    return refs;
}

STDMETHODIMP EnumDisplayAttributeInfo::Clone(IEnumTfDisplayAttributeInfo** ppEnum) {
    if (ppEnum == nullptr) {
        return E_POINTER;
    }
    auto* clone = new EnumDisplayAttributeInfo(items_);
    clone->index_ = index_;
    *ppEnum = clone;
    return S_OK;
}

STDMETHODIMP EnumDisplayAttributeInfo::Next(ULONG ulCount, ITfDisplayAttributeInfo** rgelt,
                                            ULONG* pcFetched) {
    ULONG fetched = 0;
    while (fetched < ulCount && index_ < items_.size()) {
        items_[index_]->AddRef();
        rgelt[fetched++] = items_[index_++];
    }
    if (pcFetched != nullptr) {
        *pcFetched = fetched;
    }
    return fetched == ulCount ? S_OK : S_FALSE;
}

STDMETHODIMP EnumDisplayAttributeInfo::Reset() {
    index_ = 0;
    return S_OK;
}

STDMETHODIMP EnumDisplayAttributeInfo::Skip(ULONG ulCount) {
    index_ = (std::min)(index_ + ulCount, static_cast<ULONG>(items_.size()));
    return S_OK;
}

} // namespace aiie::tsf