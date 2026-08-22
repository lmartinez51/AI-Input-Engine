#pragma once

#include <msctf.h>

#include <vector>

namespace aiie::tsf {

// One display attribute: underline for composition text.
class DisplayAttributeInfo : public ITfDisplayAttributeInfo {
public:
    DisplayAttributeInfo();
    virtual ~DisplayAttributeInfo();

    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP GetGUID(GUID* pguid) override;
    STDMETHODIMP GetDescription(BSTR* pbstrDesc) override;
    STDMETHODIMP GetAttributeInfo(TF_DISPLAYATTRIBUTE* pAttr) override;
    STDMETHODIMP SetAttributeInfo(const TF_DISPLAYATTRIBUTE* pAttr) override;
    STDMETHODIMP Reset() override;

    ULONG refCount_ = 1;
};

// Exposes the display attribute provider to the TSF manager.
class DisplayAttributeProvider : public ITfDisplayAttributeProvider {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** ppEnum) override;
    STDMETHODIMP GetDisplayAttributeInfo(REFGUID guid, ITfDisplayAttributeInfo** ppInfo) override;

    ULONG refCount_ = 1;
};

class EnumDisplayAttributeInfo : public IEnumTfDisplayAttributeInfo {
public:
    explicit EnumDisplayAttributeInfo(std::vector<ITfDisplayAttributeInfo*> items);

    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP Clone(IEnumTfDisplayAttributeInfo** ppEnum) override;
    STDMETHODIMP Next(ULONG ulCount, ITfDisplayAttributeInfo** rgelt, ULONG* pcFetched) override;
    STDMETHODIMP Reset() override;
    STDMETHODIMP Skip(ULONG ulCount) override;

    std::vector<ITfDisplayAttributeInfo*> items_;
    ULONG index_ = 0;
    ULONG refCount_ = 1;
};

} // namespace aiie::tsf