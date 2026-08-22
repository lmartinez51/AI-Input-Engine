#pragma once

#include <string>

namespace aiie {

// Secure persistent storage for API keys using Windows Credential Manager.
// Keys are stored as generic per-user credentials; they are never written to
// config.json or any plaintext file. Credential Manager protects credentials
// with the user's logon (DPAPI under the hood).
//
// Thread-safe: all functions operate on the local user's credential blob.
class CredentialStore {
public:
    // Default target names for API key credentials.
    static constexpr wchar_t kGeminiTarget[] = L"AIInputEngine/GeminiApiKey";
    static constexpr wchar_t kOpenAITarget[] = L"AIInputEngine/OpenAIApiKey";

    // Typed Gemini accessors.
    static bool GetGeminiApiKey(std::wstring& outKey);
    static bool SetGeminiApiKey(const std::wstring& key);
    static bool RemoveGeminiApiKey();
    static bool HasGeminiApiKey();

    // Typed OpenAI accessors.
    static bool GetOpenAIApiKey(std::wstring& outKey);
    static bool SetOpenAIApiKey(const std::wstring& key);
    static bool RemoveOpenAIApiKey();
    static bool HasOpenAIApiKey();

    // Backwards-compatible aliases (defaults to Gemini target).
    static bool GetApiKey(std::wstring& outKey) { return GetGeminiApiKey(outKey); }
    static bool SetApiKey(const std::wstring& key) { return SetGeminiApiKey(key); }
    static bool RemoveApiKey() { return RemoveGeminiApiKey(); }
    static bool HasApiKey() { return HasGeminiApiKey(); }

    // Target-specific operations.
    static bool GetApiKey(const wchar_t* target, std::wstring& outKey);
    static bool SetApiKey(const wchar_t* target, const std::wstring& key);
    static bool RemoveApiKey(const wchar_t* target);
    static bool HasApiKey(const wchar_t* target);
};

} // namespace aiie