// DLL module for the AI Input Engine TSF text service.
// Exports: DllGetClassObject, DllCanUnloadNow, DllRegisterServer,
// DllUnregisterServer (per-user registration, no admin required).

#include "Core/Logging.h"
#include "TSF/DisplayAttributeInfo.h"
#include "TSF/TextService.h"
#include "TSF/TsfGuids.h"

#include <windows.h>

namespace {

volatile LONG g_lockCount = 0;
volatile LONG g_objectCount = 0;

// Per-user registration helper (HKCU). No administrator privileges needed.
HRESULT WritePerUserRegistration(bool register_) {
    const std::wstring clsidKey =
        L"Software\\Classes\\CLSID\\{2B4E3D5A-7C8F-4A1E-9B3C-6D5F8E2A1B4C}";
    const std::wstring inprocKey = clsidKey + L"\\InprocServer32";
    const std::wstring tipKey =
        L"Software\\Microsoft\\CTF\\TIP\\{2B4E3D5A-7C8F-4A1E-9B3C-6D5F8E2A1B4C}";
    const std::wstring catKey = tipKey + L"\\Category\\{24D6F4C1-4F3E-4B6A-9F1D-2E3A4B5C6D7E}"; // GUID_TFCAT_TIP
    const std::wstring langProfileKey =
        tipKey + L"\\LanguageProfile\\0x00000409\\{6A9F1C3E-2B5D-4C7A-8E9F-1D3A5B6C7D8E}";

    if (!register_) {
        // Remove only what we own.
        for (const auto& key : {langProfileKey, tipKey, inprocKey, clsidKey}) {
            RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
        }
        return S_OK;
    }

    wchar_t dllPath[MAX_PATH] = {};
    if (GetModuleFileNameW(GetModuleHandleW(nullptr), dllPath, MAX_PATH) == 0) {
        return E_FAIL;
    }

    HKEY hkey = nullptr;
    // InprocServer32 with the DLL path.
    if (RegCreateKeyExW(HKEY_CURRENT_USER, inprocKey.c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &hkey, nullptr) != ERROR_SUCCESS) {
        return E_FAIL;
    }
    RegSetValueExW(hkey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(dllPath),
                   (static_cast<DWORD>(wcslen(dllPath)) + 1) * sizeof(wchar_t));
    const wchar_t kThreading[] = L"Apartment";
    RegSetValueExW(hkey, L"ThreadingModel", 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(kThreading), sizeof(kThreading));
    RegCloseKey(hkey);

    // TIP display name.
    if (RegCreateKeyExW(HKEY_CURRENT_USER, tipKey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                        &hkey, nullptr) == ERROR_SUCCESS) {
        const wchar_t kName[] = L"AI Input Engine";
        RegSetValueExW(hkey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(kName), sizeof(kName));
        RegCloseKey(hkey);
    }
    // TIP category.
    if (RegCreateKeyExW(HKEY_CURRENT_USER, catKey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                        &hkey, nullptr) == ERROR_SUCCESS) {
        const wchar_t kClsid[] = L"{2B4E3D5A-7C8F-4A1E-9B3C-6D5F8E2A1B4C}";
        RegSetValueExW(hkey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(kClsid),
                       sizeof(kClsid));
        RegCloseKey(hkey);
    }
    // Language profile (English).
    if (RegCreateKeyExW(HKEY_CURRENT_USER, langProfileKey.c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &hkey, nullptr) == ERROR_SUCCESS) {
        const wchar_t kDesc[] = L"AI Input Engine";
        RegSetValueExW(hkey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(kDesc), sizeof(kDesc));
        DWORD enable = 1;
        RegSetValueExW(hkey, L"Enable", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&enable),
                       sizeof(enable));
        RegCloseKey(hkey);
    }
    return S_OK;
}

} // namespace

class TextServiceClassFactory : public IClassFactory {
public:
    TextServiceClassFactory() { InterlockedIncrement(&g_objectCount); }
    ~TextServiceClassFactory() { InterlockedDecrement(&g_objectCount); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override {
        if (ppvObj == nullptr) {
            return E_POINTER;
        }
        *ppvObj = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
            *ppvObj = static_cast<IClassFactory*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        InterlockedIncrement(&g_lockCount);
        return 1;
    }

    STDMETHODIMP_(ULONG) Release() override {
        InterlockedDecrement(&g_lockCount);
        return 1;
    }

    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObj) override {
        if (ppvObj == nullptr) {
            return E_POINTER;
        }
        *ppvObj = nullptr;
        if (pUnkOuter != nullptr) {
            return CLASS_E_NOAGGREGATION;
        }
        if (IsEqualIID(riid, IID_ITfTextInputProcessor) ||
            IsEqualIID(riid, IID_ITfTextInputProcessorEx)) {
            auto* service = new aiie::tsf::TextService();
            InterlockedIncrement(&g_objectCount);
            HRESULT hr = service->QueryInterface(riid, ppvObj);
            service->Release();
            return hr;
        }
        if (IsEqualIID(riid, IID_ITfDisplayAttributeProvider)) {
            auto* provider = new aiie::tsf::DisplayAttributeProvider();
            InterlockedIncrement(&g_objectCount);
            HRESULT hr = provider->QueryInterface(riid, ppvObj);
            provider->Release();
            return hr;
        }
        return E_NOINTERFACE;
    }

    STDMETHODIMP LockServer(BOOL fLock) override {
        if (fLock) {
            InterlockedIncrement(&g_lockCount);
        } else {
            InterlockedDecrement(&g_lockCount);
        }
        return S_OK;
    }
};

// Exports are listed in AIInputEngineTSF.def (undecorated names so TSF's
// GetProcAddress works on both x86 and x64).

extern "C" {

HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (ppv == nullptr) {
        return E_POINTER;
    }
    *ppv = nullptr;
    if (!IsEqualCLSID(rclsid, CLSID_AIInputEngineTIP)) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    if (!IsEqualIID(riid, IID_IUnknown) && !IsEqualIID(riid, IID_IClassFactory)) {
        return E_NOINTERFACE;
    }
    auto* factory = new TextServiceClassFactory();
    const HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

HRESULT WINAPI DllCanUnloadNow() {
    return (g_objectCount == 0 && g_lockCount == 0) ? S_OK : S_FALSE;
}

HRESULT WINAPI DllRegisterServer() {
    return WritePerUserRegistration(true);
}

HRESULT WINAPI DllUnregisterServer() {
    return WritePerUserRegistration(false);
}

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        // Logging is configured on activation; keep this minimal.
        DisableThreadLibraryCalls(static_cast<HINSTANCE>(GetModuleHandleW(nullptr)));
    }
    return TRUE;
}