#include "Configuration/ConfigStore.h"

#include "Configuration/CredentialStore.h"
#include "Configuration/ModelCapabilities.h"
#include "Core/Json.h"
#include "Core/Utf.h"

#include <windows.h>
#include <shlobj.h>

#include <fstream>
#include <sstream>

namespace aiie {

ConfigStore::ConfigStore(std::filesystem::path path) : path_(std::move(path)) {}

std::filesystem::path ConfigStore::DefaultPath() {
    wchar_t buf[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, buf))) {
        std::filesystem::path p(buf);
        return p / L"AIInputEngine" / L"config.json";
    }
    return std::filesystem::temp_directory_path() / L"AIInputEngine" / L"config.json";
}

std::filesystem::path ConfigStore::DefaultLogDir() {
    wchar_t buf[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, buf))) {
        std::filesystem::path p(buf);
        return p / L"AIInputEngine" / L"logs";
    }
    return std::filesystem::temp_directory_path() / L"AIInputEngine" / L"logs";
}

bool ConfigStore::Load() {
    std::wstring text;
    {
        std::ifstream f(path_, std::ios::binary);
        if (!f) {
            return false; // no config yet: defaults are used
        }
        std::stringstream ss;
        ss << f.rdbuf();
        text = utf::FromUtf8(ss.str());
    } // close the file before any Save() below may replace it
    try {
        const json::Value root = json::Parse(text);
        // Legacy plaintext key migration: versions before 0.2 stored the
        // Gemini API key in config.json. If one is present, move it into
        // Windows Credential Manager exactly once and drop it from the file.
        if (const json::Value* legacy = root.find(L"geminiApiKey")) {
            const std::wstring& key = legacy->as_string();
            if (!key.empty() && !CredentialStore::HasGeminiApiKey()) {
                (void)CredentialStore::SetGeminiApiKey(key);
            }
            json::Value clean = root;
            clean.erase(L"geminiApiKey");
            config_ = FromJson(clean);
            Save(); // rewrite without the plaintext key
            return true;
        }
        config_ = FromJson(root);
        return true;
    } catch (const json::JsonError&) {
        return false;
    }
}

bool ConfigStore::Save() const {
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    if (ec) {
        return false;
    }
    const std::wstring text = json::Serialize(ToJson(config_), 2);
    const std::string utf8 = utf::ToUtf8(text);

    const std::filesystem::path tmp = path_.wstring() + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            return false;
        }
        f.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
        f.flush();
        if (!f) {
            return false;
        }
    }
    // Replacing an existing file can transiently fail (AV scanners briefly
    // lock freshly written files). Retry a few times before giving up.
    constexpr int kMaxAttempts = 5;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (MoveFileExW(tmp.c_str(), path_.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return true;
        }
        if (attempt + 1 < kMaxAttempts) {
            Sleep(50);
        }
    }
    return false;
}

AppConfig ConfigStore::FromJson(const json::Value& root) {
    AppConfig c;
    c.enabled = root.get_bool(L"enabled", c.enabled);
    // Operation resolution (Translate vs CorrectAndRewrite)
    std::wstring opName = root.get_string(L"operation", L"");
    if (!opName.empty()) {
        TransformOperation op;
        if (TryParseOperation(opName, op)) {
            c.operation = op;
        }
    }

    c.sourceLanguage = root.get_string(L"sourceLanguage", c.sourceLanguage);
    c.targetLanguage = root.get_string(L"targetLanguage", c.targetLanguage);

    // Translation profile resolution with fallback to legacy 'mode'
    std::wstring profileName = root.get_string(L"profile", L"");
    if (!profileName.empty()) {
        TranslationProfile prof;
        if (TryParseProfile(profileName, prof)) {
            c.profile = prof;
        }
    }
    std::wstring modeName = root.get_string(L"mode", L"");
    if (!modeName.empty()) {
        TransformMode m;
        if (TryParseMode(modeName, m)) {
            c.mode = m;
            if (profileName.empty()) {
                TranslationProfile prof;
                if (TryParseProfile(modeName, prof)) {
                    c.profile = prof;
                }
            }
        }
    }
    c.customProfileInstructions = root.get_string(L"customProfileInstructions", c.customProfileInstructions);

    std::wstring providerName = root.get_string(L"provider", L"");
    if (!providerName.empty()) {
        ProviderId p;
        if (TryParseProvider(providerName, p)) {
            c.provider = p;
        }
    }

    // Generic model and endpoint fields.
    c.model = root.get_string(L"model", L"");
    c.endpoint = root.get_string(L"endpoint", L"");

    // Backward-compatible migration from legacy fields:
    if (c.model.empty()) {
        c.model = root.get_string(L"geminiModel", L"");
    }
    if (c.endpoint.empty()) {
        c.endpoint = root.get_string(L"geminiEndpoint", L"");
    }

    // Fall back to provider-appropriate defaults if empty:
    if (c.model.empty()) {
        c.model = DefaultModelForProvider(c.provider);
    }
    if (c.endpoint.empty()) {
        c.endpoint = DefaultEndpointForProvider(c.provider);
    }

    // Model parameters
    if (const json::Value* rEffort = root.find(L"reasoningEffort")) {
        if (rEffort->is_string()) {
            c.modelParams.reasoningEffort = rEffort->as_string();
        }
    }
    if (root.find(L"temperature")) {
        c.modelParams.temperature = root.get_number(L"temperature");
    }
    if (root.find(L"topP")) {
        c.modelParams.topP = root.get_number(L"topP");
    }
    if (root.find(L"topK")) {
        c.modelParams.topK = static_cast<int>(root.get_int(L"topK"));
    }
    if (root.find(L"maxOutputTokens")) {
        c.modelParams.maxOutputTokens = static_cast<int>(root.get_int(L"maxOutputTokens"));
    }

    // Sanitize model parameters for the loaded provider and model
    c.modelParams = SanitizeModelParameters(c.provider, c.model, c.modelParams);

    c.debounceMs = static_cast<int>(root.get_int(L"debounceMs", c.debounceMs));
    c.minChars = static_cast<int>(root.get_int(L"minChars", c.minChars));
    c.maxBufferChars = static_cast<int>(root.get_int(L"maxBufferChars", c.maxBufferChars));
    c.requestTimeoutMs = static_cast<int>(root.get_int(L"requestTimeoutMs", c.requestTimeoutMs));
    c.maxRetries = static_cast<int>(root.get_int(L"maxRetries", c.maxRetries));

    c.smartPasteEnabled = root.get_bool(L"smartPasteEnabled", c.smartPasteEnabled);

    c.hotkeyToggle = {static_cast<UINT>(root.get_int(L"hotkeyToggleMods", c.hotkeyToggle.modifiers)),
                      static_cast<UINT>(root.get_int(L"hotkeyToggleVk", c.hotkeyToggle.vk))};
    c.hotkeySwap = {static_cast<UINT>(root.get_int(L"hotkeySwapMods", c.hotkeySwap.modifiers)),
                    static_cast<UINT>(root.get_int(L"hotkeySwapVk", c.hotkeySwap.vk))};
    c.hotkeyDiagnostics = {static_cast<UINT>(root.get_int(L"hotkeyDiagnosticsMods", c.hotkeyDiagnostics.modifiers)),
                           static_cast<UINT>(root.get_int(L"hotkeyDiagnosticsVk", c.hotkeyDiagnostics.vk))};
    c.hotkeySmartPaste = {static_cast<UINT>(root.get_int(L"hotkeySmartPasteMods", c.hotkeySmartPaste.modifiers)),
                          static_cast<UINT>(root.get_int(L"hotkeySmartPasteVk", c.hotkeySmartPaste.vk))};

    const int level = static_cast<int>(root.get_int(L"logLevel", static_cast<int64_t>(c.logLevel)));
    if (level >= static_cast<int>(LogLevel::Error) && level <= static_cast<int>(LogLevel::Trace)) {
        c.logLevel = static_cast<LogLevel>(level);
    }
    c.logRawText = root.get_bool(L"logRawText", c.logRawText);
    c.warnOnExternalSend = root.get_bool(L"warnOnExternalSend", c.warnOnExternalSend);

    c.cacheEnabled = root.get_bool(L"cacheEnabled", c.cacheEnabled);
    c.cacheMaxEntries = static_cast<int>(root.get_int(L"cacheMaxEntries", c.cacheMaxEntries));

    // Clamp numeric fields to sane ranges
    const AppConfig def;
    auto clamp = [&def](int value, int defValue, int lo, int hi) {
        return (value >= lo && value <= hi) ? value : defValue;
    };
    c.debounceMs = clamp(c.debounceMs, def.debounceMs, 50, 10000);
    c.minChars = clamp(c.minChars, def.minChars, 1, 20);
    c.maxBufferChars = clamp(c.maxBufferChars, def.maxBufferChars, 20, 2000);
    c.requestTimeoutMs = clamp(c.requestTimeoutMs, def.requestTimeoutMs, 1000, 300000);
    c.maxRetries = clamp(c.maxRetries, def.maxRetries, 0, 10);
    c.cacheMaxEntries = clamp(c.cacheMaxEntries, def.cacheMaxEntries, 10, 10000);
    return c;
}

json::Value ConfigStore::ToJson(const AppConfig& c) {
    json::Value root = json::Value::MakeObject();
    root.set(L"enabled", c.enabled);
    root.set(L"smartPasteEnabled", c.smartPasteEnabled);
    root.set(L"operation", std::wstring(OperationName(c.operation)));
    root.set(L"sourceLanguage", c.sourceLanguage);
    root.set(L"targetLanguage", c.targetLanguage);
    root.set(L"profile", std::wstring(ProfileName(c.profile)));
    if (!c.customProfileInstructions.empty()) {
        root.set(L"customProfileInstructions", c.customProfileInstructions);
    }
    root.set(L"mode", std::wstring(ModeName(c.mode)));
    root.set(L"provider", std::wstring(ProviderName(c.provider)));
    root.set(L"model", c.model);
    root.set(L"endpoint", c.endpoint);

    if (c.modelParams.reasoningEffort.has_value()) {
        root.set(L"reasoningEffort", *c.modelParams.reasoningEffort);
    }
    if (c.modelParams.temperature.has_value()) {
        root.set(L"temperature", *c.modelParams.temperature);
    }
    if (c.modelParams.topP.has_value()) {
        root.set(L"topP", *c.modelParams.topP);
    }
    if (c.modelParams.topK.has_value()) {
        root.set(L"topK", *c.modelParams.topK);
    }
    if (c.modelParams.maxOutputTokens.has_value()) {
        root.set(L"maxOutputTokens", *c.modelParams.maxOutputTokens);
    }

    // NOTE: never serialize API keys here; they are stored in CredentialStore.
    root.set(L"debounceMs", c.debounceMs);
    root.set(L"minChars", c.minChars);
    root.set(L"maxBufferChars", c.maxBufferChars);
    root.set(L"requestTimeoutMs", c.requestTimeoutMs);
    root.set(L"maxRetries", c.maxRetries);
    root.set(L"hotkeyToggleMods", c.hotkeyToggle.modifiers);
    root.set(L"hotkeyToggleVk", c.hotkeyToggle.vk);
    root.set(L"hotkeySwapMods", c.hotkeySwap.modifiers);
    root.set(L"hotkeySwapVk", c.hotkeySwap.vk);
    root.set(L"hotkeyDiagnosticsMods", c.hotkeyDiagnostics.modifiers);
    root.set(L"hotkeyDiagnosticsVk", c.hotkeyDiagnostics.vk);
    root.set(L"hotkeySmartPasteMods", c.hotkeySmartPaste.modifiers);
    root.set(L"hotkeySmartPasteVk", c.hotkeySmartPaste.vk);
    root.set(L"logLevel", static_cast<int64_t>(c.logLevel));
    root.set(L"logRawText", c.logRawText);
    root.set(L"warnOnExternalSend", c.warnOnExternalSend);
    root.set(L"cacheEnabled", c.cacheEnabled);
    root.set(L"cacheMaxEntries", c.cacheMaxEntries);
    return root;
}

} // namespace aiie