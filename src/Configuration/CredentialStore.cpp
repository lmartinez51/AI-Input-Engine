#include "Configuration/CredentialStore.h"

#include <windows.h>
#include <wincred.h>

namespace aiie {

namespace {

constexpr DWORD kMaxKeyLen = 1024;

bool ReadTarget(const wchar_t* target, std::wstring& outKey) {
    outKey.clear();
    if (target == nullptr) {
        return false;
    }
    PCREDENTIALW cred = nullptr;
    if (!CredReadW(target, CRED_TYPE_GENERIC, 0, &cred) || cred == nullptr) {
        return false;
    }
    const bool ok = cred->CredentialBlobSize > 0 && cred->CredentialBlob != nullptr &&
                    cred->CredentialBlobSize <= kMaxKeyLen;
    if (ok) {
        outKey.assign(reinterpret_cast<const wchar_t*>(cred->CredentialBlob),
                      cred->CredentialBlobSize / sizeof(wchar_t));
    }
    CredFree(cred);
    return ok && !outKey.empty();
}

bool WriteTarget(const wchar_t* target, const std::wstring& key) {
    if (target == nullptr || key.empty() || key.size() > kMaxKeyLen) {
        return false;
    }
    CREDENTIALW cred{};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<wchar_t*>(target);
    cred.CredentialBlobSize = static_cast<DWORD>(key.size() * sizeof(wchar_t));
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<wchar_t*>(key.data()));
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    cred.UserName = const_cast<wchar_t*>(L"AIInputEngine");
    return CredWriteW(&cred, 0) != FALSE;
}

bool DeleteTarget(const wchar_t* target) {
    if (target == nullptr) {
        return false;
    }
    // Delete succeeds even when no credential exists.
    return CredDeleteW(target, CRED_TYPE_GENERIC, 0) != FALSE ||
           GetLastError() == ERROR_NOT_FOUND;
}

} // namespace

bool CredentialStore::GetGeminiApiKey(std::wstring& outKey) {
    return ReadTarget(kGeminiTarget, outKey);
}

bool CredentialStore::SetGeminiApiKey(const std::wstring& key) {
    return WriteTarget(kGeminiTarget, key);
}

bool CredentialStore::RemoveGeminiApiKey() {
    return DeleteTarget(kGeminiTarget);
}

bool CredentialStore::HasGeminiApiKey() {
    std::wstring dummy;
    return ReadTarget(kGeminiTarget, dummy);
}

bool CredentialStore::GetOpenAIApiKey(std::wstring& outKey) {
    return ReadTarget(kOpenAITarget, outKey);
}

bool CredentialStore::SetOpenAIApiKey(const std::wstring& key) {
    return WriteTarget(kOpenAITarget, key);
}

bool CredentialStore::RemoveOpenAIApiKey() {
    return DeleteTarget(kOpenAITarget);
}

bool CredentialStore::HasOpenAIApiKey() {
    std::wstring dummy;
    return ReadTarget(kOpenAITarget, dummy);
}

bool CredentialStore::GetApiKey(const wchar_t* target, std::wstring& outKey) {
    return ReadTarget(target, outKey);
}

bool CredentialStore::SetApiKey(const wchar_t* target, const std::wstring& key) {
    return WriteTarget(target, key);
}

bool CredentialStore::RemoveApiKey(const wchar_t* target) {
    return DeleteTarget(target);
}

bool CredentialStore::HasApiKey(const wchar_t* target) {
    std::wstring dummy;
    return ReadTarget(target, dummy);
}

} // namespace aiie