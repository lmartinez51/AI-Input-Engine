// tsfreg — per-user TSF registration / unregistration / verification for the
// AI Input Engine text service. No administrator privileges required.
//
// Usage:
//   aiie_tsfreg.exe --register   register the text service for the current user
//   aiie_tsfreg.exe --unregister remove the text service for the current user
//   aiie_tsfreg.exe --verify     check registration and COM wiring
//   aiie_tsfreg.exe --help       usage

#include <windows.h>
#include <msctf.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kClsidStr[] = L"{2B4E3D5A-7C8F-4A1E-9B3C-6D5F8E2A1B4C}";
constexpr wchar_t kTipKey[] = L"Software\\Microsoft\\CTF\\TIP\\{2B4E3D5A-7C8F-4A1E-9B3C-6D5F8E2A1B4C}";
constexpr wchar_t kCatKey[] = L"Software\\Microsoft\\CTF\\TIP\\{2B4E3D5A-7C8F-4A1E-9B3C-6D5F8E2A1B4C}\\Category\\{24D6F4C1-4F3E-4B6A-9F1D-2E3A4B5C6D7E}";
constexpr wchar_t kLangProfileKey[] = L"Software\\Microsoft\\CTF\\TIP\\{2B4E3D5A-7C8F-4A1E-9B3C-6D5F8E2A1B4C}\\LanguageProfile\\0x00000409\\{6A9F1C3E-2B5D-4C7A-8E9F-1D3A5B6C7D8E}";
constexpr wchar_t kClsidKey[] = L"Software\\Classes\\CLSID\\{2B4E3D5A-7C8F-4A1E-9B3C-6D5F8E2A1B4C}";
constexpr wchar_t kInprocKey[] = L"Software\\Classes\\CLSID\\{2B4E3D5A-7C8F-4A1E-9B3C-6D5F8E2A1B4C}\\InprocServer32";

// The text service DLL must sit next to this tool. Its name is fixed so a
// copy of this tool can never accidentally be registered as the server.
std::wstring GetDllPath() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring dir(buf);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        dir = dir.substr(0, slash + 1);
    }
    return dir + L"AIInputEngineTSF.dll";
}

bool WriteKey(HKEY root, const std::wstring& path, const std::wstring& name,
              DWORD type, const void* data, DWORD size) {
    HKEY hkey = nullptr;
    if (RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hkey,
                        nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const LSTATUS status = RegSetValueExW(hkey, name.c_str(), 0, type,
                                          static_cast<const BYTE*>(data), size);
    RegCloseKey(hkey);
    return status == ERROR_SUCCESS;
}

int Register() {
    const std::wstring dll = GetDllPath();
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wprintf(L"ERROR: AIInputEngineTSF.dll not found next to this tool:\n  %ls\n", dll.c_str());
        std::printf("Build the TSF DLL and copy it (or run this tool from the build output directory).\n");
        return 1;
    }

    // 1) Per-user CLSID registration so CTF can CoCreateInstance the service.
    if (!WriteKey(HKEY_CURRENT_USER, kInprocKey, L"", REG_SZ, dll.c_str(),
                  (static_cast<DWORD>(dll.size()) + 1) * sizeof(wchar_t))) {
        std::printf("ERROR: could not write InprocServer32\n");
        return 1;
    }
    static const wchar_t kThreading[] = L"Apartment";
    WriteKey(HKEY_CURRENT_USER, kInprocKey, L"ThreadingModel", REG_SZ, kThreading,
             sizeof(kThreading));

    // 2) TIP registration under CTF.
    static const wchar_t kName[] = L"AI Input Engine";
    if (!WriteKey(HKEY_CURRENT_USER, kTipKey, L"", REG_SZ, kName, sizeof(kName))) {
        std::printf("ERROR: could not write TIP key\n");
        return 1;
    }
    WriteKey(HKEY_CURRENT_USER, kCatKey, L"", REG_SZ, kClsidStr, sizeof(kClsidStr));
    static const wchar_t kDesc[] = L"AI Input Engine";
    if (!WriteKey(HKEY_CURRENT_USER, kLangProfileKey, L"", REG_SZ, kDesc, sizeof(kDesc))) {
        std::printf("ERROR: could not write language profile\n");
        return 1;
    }
    DWORD enable = 1;
    WriteKey(HKEY_CURRENT_USER, kLangProfileKey, L"Enable", REG_DWORD, &enable, sizeof(enable));

    // 3) Also register through the documented profile API where possible.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) {
        CLSID clsid{};
        CLSID profile{};
        CLSIDFromString(kClsidStr, &clsid);
        CLSIDFromString(L"{6A9F1C3E-2B5D-4C7A-8E9F-1D3A5B6C7D8E}", &profile);
        ITfInputProcessorProfiles* pProfiles = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_ITfInputProcessorProfiles,
                                       reinterpret_cast<void**>(&pProfiles)))) {
            const HRESULT hrReg = pProfiles->Register(clsid);
            if (SUCCEEDED(hrReg)) {
pProfiles->AddLanguageProfile(clsid, 0x0409, profile, kName,
                                      (DWORD)wcslen(kName), nullptr, 0, 0);
                std::printf("Registered via ITfInputProcessorProfiles (0x%08X)\n", hrReg);
            } else {
                std::printf("Note: ITfInputProcessorProfiles::Register returned 0x%08X "
                            "(registry entries were written anyway)\n", hrReg);
            }
            pProfiles->Release();
        }
        CoUninitialize();
    }

    std::printf("AI Input Engine TSF text service registered for the current user.\n");
    std::printf("Sign out and back in (or restart the target applications) for CTF to load it.\n");
    return 0;
}

int Unregister() {
    for (const wchar_t* key : {kLangProfileKey, kTipKey, kInprocKey, kClsidKey}) {
        RegDeleteTreeW(HKEY_CURRENT_USER, key);
    }
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) {
        CLSID clsid{};
        CLSIDFromString(kClsidStr, &clsid);
        ITfInputProcessorProfiles* pProfiles = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_ITfInputProcessorProfiles,
                                       reinterpret_cast<void**>(&pProfiles)))) {
            pProfiles->Unregister(clsid);
            pProfiles->Release();
        }
        CoUninitialize();
    }
    std::printf("AI Input Engine TSF text service unregistered.\n");
    return 0;
}

int Verify() {
    bool ok = true;

    HKEY hkey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kInprocKey, 0, KEY_READ, &hkey) == ERROR_SUCCESS) {
        wchar_t dll[MAX_PATH] = {};
        DWORD size = sizeof(dll);
        const LSTATUS st = RegQueryValueExW(hkey, nullptr, nullptr, nullptr,
                                            reinterpret_cast<BYTE*>(dll), &size);
        RegCloseKey(hkey);
        if (st == ERROR_SUCCESS) {
            std::printf("InprocServer32: %ls\n", dll);
            ok = ok && GetFileAttributesW(dll) != INVALID_FILE_ATTRIBUTES;
        } else {
            std::printf("InprocServer32: MISSING\n");
            ok = false;
        }
    } else {
        std::printf("InprocServer32: NOT REGISTERED\n");
        ok = false;
    }

    if (RegOpenKeyExW(HKEY_CURRENT_USER, kLangProfileKey, 0, KEY_READ, &hkey) == ERROR_SUCCESS) {
        RegCloseKey(hkey);
        std::printf("Language profile: registered\n");
    } else {
        std::printf("Language profile: NOT REGISTERED\n");
        ok = false;
    }

    // COM wiring: load the DLL and create the class factory.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) {
        CLSID clsid{};
        CLSIDFromString(kClsidStr, &clsid);
        IClassFactory* pFactory = nullptr;
        const HRESULT hrCo = CoGetClassObject(clsid, CLSCTX_INPROC_SERVER, nullptr,
                                              IID_IClassFactory, reinterpret_cast<void**>(&pFactory));
        if (SUCCEEDED(hrCo) && pFactory) {
            std::printf("CoGetClassObject: OK\n");
            ITfTextInputProcessor* pTip = nullptr;
            const HRESULT hrCreate = pFactory->CreateInstance(nullptr, IID_ITfTextInputProcessor,
                                                              reinterpret_cast<void**>(&pTip));
            if (SUCCEEDED(hrCreate) && pTip) {
                std::printf("CreateInstance(ITfTextInputProcessor): OK\n");
                pTip->Release();
            } else {
                std::printf("CreateInstance(ITfTextInputProcessor): FAILED 0x%08X\n", hrCreate);
                ok = false;
            }
            pFactory->Release();
        } else {
            std::printf("CoGetClassObject: FAILED 0x%08X\n", hrCo);
            ok = false;
        }
        CoUninitialize();
    } else {
        std::printf("CoInitializeEx: FAILED 0x%08X\n", hr);
        ok = false;
    }

    std::printf(ok ? "Verification PASSED.\n" : "Verification FAILED.\n");
    return ok ? 0 : 1;
}

void PrintUsage() {
    std::printf(
        "aiie_tsfreg — AI Input Engine TSF registration tool\n\n"
        "Usage: aiie_tsfreg.exe [--register | --unregister | --verify | --help]\n\n"
        "  --register     Register the TSF text service for the current user (HKCU,\n"
        "                 no administrator privileges required).\n"
        "  --unregister   Remove the TSF text service registration.\n"
        "  --verify       Check registration and that the DLL creates the TIP.\n"
        "  --help         Show this help.\n\n"
        "After registering, sign out and back in (or restart target applications)\n"
        "for the Text Services Framework to load the new text service.\n");
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const std::wstring cmd = argc > 1 ? argv[1] : L"--help";
    if (cmd == L"--register") {
        return Register();
    }
    if (cmd == L"--unregister") {
        return Unregister();
    }
    if (cmd == L"--verify") {
        return Verify();
    }
    PrintUsage();
    return 0;
}