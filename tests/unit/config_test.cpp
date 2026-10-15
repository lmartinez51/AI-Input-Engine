#include "mini_test.h"

#include "Configuration/ConfigStore.h"
#include "Configuration/CredentialStore.h"
#include "Configuration/LanguageRegistry.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace aiie;

namespace {
std::wstring TempConfigPath() {
    wchar_t dir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + L"aiie_test_config.json";
}
// Isolated credential target so tests never touch the real stored key.
constexpr wchar_t kTestCred[] = L"AIInputEngine.Test/CredentialStore";
constexpr wchar_t kTestOpenAICred[] = L"AIInputEngine.Test/OpenAICredentialStore";
} // namespace

TEST(config_defaults) {
    const AppConfig c;
    CHECK(c.enabled);
    CHECK_STREQ(c.sourceLanguage, L"auto");
    CHECK_STREQ(c.targetLanguage, L"en");
    CHECK_EQ(c.provider, ProviderId::Mock);
    CHECK_EQ(c.debounceMs, 500);
    CHECK_EQ(c.hotkeyToggle.vk, VK_SPACE);
}

TEST(config_save_load_roundtrip) {
    const std::filesystem::path path(TempConfigPath());
    std::filesystem::remove(path);

    ConfigStore store(path);
    AppConfig c;
    c.enabled = false;
    c.sourceLanguage = L"es";
    c.targetLanguage = L"fr";
    c.mode = TransformMode::Professional;
    c.provider = ProviderId::Gemini;
    c.model = L"gemini-2.5-flash";
    c.endpoint = L"https://generativelanguage.googleapis.com/v1beta";
    c.debounceMs = 350;
    c.minChars = 3;
    c.hotkeyToggle = {MOD_CONTROL | MOD_SHIFT, L'Q'};
    c.logLevel = LogLevel::Debug;
    c.logRawText = true;
    c.cacheMaxEntries = 10;
    store.Set(c);
    CHECK(store.Save());

    ConfigStore loaded(path);
    CHECK(loaded.Load());
    const AppConfig& l = loaded.Get();
    CHECK(!l.enabled);
    CHECK_STREQ(l.sourceLanguage, L"es");
    CHECK_STREQ(l.targetLanguage, L"fr");
    CHECK_EQ(l.mode, TransformMode::Professional);
    CHECK_EQ(l.provider, ProviderId::Gemini);
    CHECK_STREQ(l.model, L"gemini-2.5-flash");
    CHECK_STREQ(l.endpoint, L"https://generativelanguage.googleapis.com/v1beta");
    CHECK_EQ(l.debounceMs, 350);
    CHECK_EQ(l.minChars, 3);
    CHECK_EQ(l.hotkeyToggle.modifiers, MOD_CONTROL | MOD_SHIFT);
    CHECK_EQ(l.hotkeyToggle.vk, L'Q');
    CHECK_EQ(l.logLevel, LogLevel::Debug);
    CHECK(l.logRawText);
    CHECK_EQ(l.cacheMaxEntries, 10);

    std::filesystem::remove(path);
}

TEST(config_openai_roundtrip) {
    const std::filesystem::path path(TempConfigPath() + L".openai");
    std::filesystem::remove(path);

    ConfigStore store(path);
    AppConfig c;
    c.provider = ProviderId::OpenAI;
    c.model = L"gpt-5.6-luna";
    c.endpoint = L"https://api.openai.com/v1/responses";
    store.Set(c);
    CHECK(store.Save());

    ConfigStore loaded(path);
    CHECK(loaded.Load());
    const AppConfig& l = loaded.Get();
    CHECK_EQ(l.provider, ProviderId::OpenAI);
    CHECK_STREQ(l.model, L"gpt-5.6-luna");
    CHECK_STREQ(l.endpoint, L"https://api.openai.com/v1/responses");

    std::filesystem::remove(path);
}

TEST(config_legacy_gemini_migration) {
    const std::filesystem::path path(TempConfigPath() + L".legacy_fields");
    std::filesystem::remove(path);

    {
        std::ofstream f(path, std::ios::binary);
        f << "{\"provider\":\"gemini\",\"geminiModel\":\"legacy-gemini-pro\",\"geminiEndpoint\":\"https://legacy.example.com\"}";
    }

    ConfigStore store(path);
    CHECK(store.Load());
    const AppConfig& l = store.Get();
    CHECK_EQ(l.provider, ProviderId::Gemini);
    CHECK_STREQ(l.model, L"legacy-gemini-pro");
    CHECK_STREQ(l.endpoint, L"https://legacy.example.com");

    std::filesystem::remove(path);
}

TEST(config_never_contains_api_key) {
    const std::filesystem::path path(TempConfigPath() + L".nokey");
    std::filesystem::remove(path);
    CredentialStore::RemoveApiKey(kTestCred);

    ConfigStore store(path);
    AppConfig c;
    c.provider = ProviderId::Gemini;
    store.Set(c);
    CHECK(store.Save());

    {
        std::ifstream f(path, std::ios::binary);
        const std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(content.find("ApiKey") == std::string::npos);
        CHECK(content.find("geminiApiKey") == std::string::npos);
        CHECK(content.find("openaiApiKey") == std::string::npos);
        CHECK(content.find("AIza") == std::string::npos);
    }

    std::filesystem::remove(path);
}

TEST(config_migrates_legacy_plaintext_key_to_credential_manager) {
    const std::filesystem::path path(TempConfigPath() + L".legacy");
    CredentialStore::RemoveApiKey(); // real target: clean before, clean after
    {
        std::ofstream f(path, std::ios::binary);
        f << "{\"provider\":\"gemini\",\"geminiApiKey\":\"legacy-secret-value\"}";
    }
    ConfigStore store(path);
    CHECK(store.Load());
    // The key must have been moved out of the file (Load rewrites it).
    {
        std::ifstream f(path, std::ios::binary);
        const std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(content.find("legacy-secret-value") == std::string::npos);
        CHECK(content.find("geminiApiKey") == std::string::npos);
    }
    std::filesystem::remove(path);
    // The migrated key lives in the secure store; remove it after the test so
    // no test data survives in the real credential target.
    CredentialStore::RemoveApiKey();
}

TEST(credential_store_roundtrip) {
    CredentialStore::RemoveApiKey(kTestCred);
    CHECK(!CredentialStore::HasApiKey(kTestCred));

    std::wstring out;
    CHECK(!CredentialStore::GetApiKey(kTestCred, out));
    CHECK(out.empty());

    CHECK(CredentialStore::SetApiKey(kTestCred, L"test-secret-abc"));
    CHECK(CredentialStore::HasApiKey(kTestCred));
    CHECK(CredentialStore::GetApiKey(kTestCred, out));
    CHECK_STREQ(out, L"test-secret-abc");

    CHECK(CredentialStore::SetApiKey(kTestCred, L"replaced"));
    CHECK(CredentialStore::GetApiKey(kTestCred, out));
    CHECK_STREQ(out, L"replaced");

    CHECK(CredentialStore::RemoveApiKey(kTestCred));
    CHECK(!CredentialStore::HasApiKey(kTestCred));
}

TEST(credential_store_openai_roundtrip) {
    CredentialStore::RemoveApiKey(kTestOpenAICred);
    CHECK(!CredentialStore::HasApiKey(kTestOpenAICred));

    std::wstring out;
    CHECK(!CredentialStore::GetApiKey(kTestOpenAICred, out));

    CHECK(CredentialStore::SetApiKey(kTestOpenAICred, L"sk-test-openai-key-xyz"));
    CHECK(CredentialStore::HasApiKey(kTestOpenAICred));
    CHECK(CredentialStore::GetApiKey(kTestOpenAICred, out));
    CHECK_STREQ(out, L"sk-test-openai-key-xyz");

    CHECK(CredentialStore::RemoveApiKey(kTestOpenAICred));
    CHECK(!CredentialStore::HasApiKey(kTestOpenAICred));
}

TEST(config_missing_file_uses_defaults) {
    const std::filesystem::path path(TempConfigPath() + L".missing");
    std::filesystem::remove(path);
    ConfigStore store(path);
    CHECK(!store.Load()); // absent file is not an error
    CHECK(store.Get().enabled);
}

TEST(config_corrupt_file_keeps_defaults) {
    const std::filesystem::path path(TempConfigPath() + L".corrupt");
    {
        std::ofstream f(path, std::ios::binary);
        f << "{ not valid json";
    }
    ConfigStore store(path);
    CHECK(!store.Load());
    CHECK(store.Get().enabled);
    std::filesystem::remove(path);
}

TEST(config_load_json_parse_exception_handling) {
    const std::vector<std::string> malformed_json_cases = {
        "{ not valid json",
        "{\"enabled\": true,",
        "{\"provider\": \"gemini\", \"debounceMs\": }",
        "tru",
        "0123",
        "[1, 2,]",
        "\"unclosed string",
        "{\"key\": undefined}"
    };

    for (size_t i = 0; i < malformed_json_cases.size(); ++i) {
        const std::filesystem::path path(TempConfigPath() + L".parse_error_" + std::to_wstring(i));
        {
            std::ofstream f(path, std::ios::binary);
            f << malformed_json_cases[i];
        }

        ConfigStore store(path);
        // Ensure Load returns false when json::Parse throws json::JsonError
        CHECK(!store.Load());
        // Verify defaults remain intact
        const AppConfig& cfg = store.Get();
        CHECK(cfg.enabled);
        CHECK_EQ(cfg.debounceMs, 500);
        CHECK_EQ(cfg.provider, ProviderId::Mock);

        std::filesystem::remove(path);
    }
}

TEST(config_unknown_fields_ignored) {
    const std::filesystem::path path(TempConfigPath() + L".unknown");
    {
        std::ofstream f(path, std::ios::binary);
        f << "{\"enabled\":true,\"futureField\":123,\"provider\":\"mock\"}";
    }
    ConfigStore store(path);
    CHECK(store.Load());
    CHECK_EQ(store.Get().provider, ProviderId::Mock);
    std::filesystem::remove(path);
}

TEST(config_invalid_values_clamped_to_defaults) {
    const std::filesystem::path path(TempConfigPath() + L".invalid");
    {
        std::ofstream f(path, std::ios::binary);
        f << "{\"debounceMs\":-5,\"logLevel\":99,\"provider\":\"nonexistent\",\"mode\":\"bogus\"}";
    }
    ConfigStore store(path);
    CHECK(store.Load());
    CHECK_EQ(store.Get().debounceMs, 500);
    CHECK_EQ(store.Get().logLevel, LogLevel::Info);
    CHECK_EQ(store.Get().provider, ProviderId::Mock);
    CHECK_EQ(store.Get().mode, TransformMode::Translate);
    std::filesystem::remove(path);
}

TEST(config_language_support) {
    CHECK(IsSupportedLanguage(L"auto"));
    CHECK(IsSupportedLanguage(L"es"));
    CHECK(IsSupportedLanguage(L"en"));
    CHECK(!IsSupportedLanguage(L"xx"));
    CHECK_STREQ(LanguageDisplayName(L"es"), L"Spanish");
    CHECK_STREQ(LanguageDisplayName(L"zz"), L"zz");
}

TEST(config_mode_parse) {
    TransformMode m;
    CHECK(TryParseMode(L"translate", m));
    CHECK_EQ(m, TransformMode::Translate);
    CHECK(TryParseMode(L"rewrite", m));
    CHECK_EQ(m, TransformMode::Rewrite);
    CHECK(!TryParseMode(L"nope", m));
}

TEST(config_profile_and_model_params_roundtrip) {
    const std::filesystem::path path(TempConfigPath() + L".profile_params");
    std::filesystem::remove(path);

    ConfigStore store(path);
    AppConfig c;
    c.profile = TranslationProfile::Custom;
    c.customProfileInstructions = L"Translate into British English slang";
    c.provider = ProviderId::Gemini;
    c.model = L"gemini-3.7-flash";
    c.modelParams.temperature = 0.7;
    c.modelParams.topP = 0.9;
    c.modelParams.topK = 40;
    c.modelParams.maxOutputTokens = 2048;
    store.Set(c);
    CHECK(store.Save());

    ConfigStore loaded(path);
    CHECK(loaded.Load());
    const AppConfig& l = loaded.Get();
    CHECK_EQ(l.profile, TranslationProfile::Custom);
    CHECK_STREQ(l.customProfileInstructions, L"Translate into British English slang");
    CHECK(l.modelParams.temperature.has_value());
    CHECK_EQ(*l.modelParams.temperature, 0.7);
    CHECK(l.modelParams.topP.has_value());
    CHECK_EQ(*l.modelParams.topP, 0.9);
    CHECK(l.modelParams.topK.has_value());
    CHECK_EQ(*l.modelParams.topK, 40);
    CHECK(l.modelParams.maxOutputTokens.has_value());
    CHECK_EQ(*l.modelParams.maxOutputTokens, 2048);

    std::filesystem::remove(path);
}

TEST(config_openai_model_params_sanitization_on_load) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / L"aiie_test_openai_sanitize.json";

    ConfigStore store(path);
    AppConfig c;
    c.provider = ProviderId::OpenAI;
    c.model = L"gpt-4o";
    c.modelParams.reasoningEffort = L"low"; // unsupported on gpt-4o -> sanitized
    c.modelParams.topK = 40;               // unsupported on OpenAI -> sanitized
    c.modelParams.temperature = 0.8;
    c.modelParams.topP = 0.95;
    store.Set(c);
    CHECK(store.Save());

    ConfigStore loaded(path);
    CHECK(loaded.Load());
    const AppConfig& l = loaded.Get();
    CHECK(!l.modelParams.reasoningEffort.has_value());
    CHECK(!l.modelParams.topK.has_value());
    CHECK(l.modelParams.temperature.has_value());
    CHECK_EQ(*l.modelParams.temperature, 0.8);
    CHECK(l.modelParams.topP.has_value());
    CHECK_EQ(*l.modelParams.topP, 0.95);

    std::filesystem::remove(path);
}

TEST(config_profile_parse) {
    TranslationProfile p;
    CHECK(TryParseProfile(L"neutral", p));
    CHECK_EQ(p, TranslationProfile::Neutral);
    CHECK(TryParseProfile(L"professional", p));
    CHECK_EQ(p, TranslationProfile::Professional);
    CHECK(TryParseProfile(L"casual", p));
    CHECK_EQ(p, TranslationProfile::Casual);
    CHECK(TryParseProfile(L"slang", p));
    CHECK_EQ(p, TranslationProfile::Slang);
    CHECK(TryParseProfile(L"custom", p));
    CHECK_EQ(p, TranslationProfile::Custom);
    CHECK(!TryParseProfile(L"invalid_profile", p));
}

TEST(config_operation_parse) {
    TransformOperation op;
    CHECK(TryParseOperation(L"translate", op));
    CHECK_EQ(op, TransformOperation::Translate);
    CHECK(TryParseOperation(L"correct_and_rewrite", op));
    CHECK_EQ(op, TransformOperation::CorrectAndRewrite);
    CHECK(TryParseOperation(L"rewrite", op));
    CHECK_EQ(op, TransformOperation::CorrectAndRewrite);
    CHECK(!TryParseOperation(L"invalid_op", op));
}

TEST(config_operation_roundtrip) {
    const std::filesystem::path path(TempConfigPath());
    std::filesystem::remove(path);

    ConfigStore store(path);
    AppConfig c;
    c.operation = TransformOperation::CorrectAndRewrite;
    c.sourceLanguage = L"es";
    c.targetLanguage = L"es";
    c.profile = TranslationProfile::Professional;
    store.Set(c);
    CHECK(store.Save());

    ConfigStore loaded(path);
    CHECK(loaded.Load());
    const AppConfig& l = loaded.Get();
    CHECK_EQ(l.operation, TransformOperation::CorrectAndRewrite);
    CHECK_STREQ(l.sourceLanguage, L"es");
    CHECK_STREQ(l.targetLanguage, L"es");
    CHECK_EQ(l.profile, TranslationProfile::Professional);

    std::filesystem::remove(path);
}

TEST(language_registry_chinese_and_validation) {
    CHECK(IsSupportedLanguage(L"zh-Hans"));
    CHECK(IsSupportedLanguage(L"zh-Hant"));
    CHECK_STREQ(LanguageDisplayName(L"zh-Hans"), L"Chinese (Simplified)");
    CHECK_STREQ(LanguageDisplayName(L"zh-Hant"), L"Chinese (Traditional)");
    CHECK_STREQ(LanguageDisplayName(L"pt"), L"Portuguese");
    CHECK_STREQ(LanguageDisplayName(L"es"), L"Spanish");
    CHECK_STREQ(LanguageDisplayName(L"en"), L"English");
    CHECK_STREQ(LanguageDisplayName(L"auto"), L"Auto-Detect");

    // Display Name -> Code reverse mapping
    CHECK_STREQ(LanguageCodeFromDisplayName(L"Chinese (Simplified)"), L"zh-Hans");
    CHECK_STREQ(LanguageCodeFromDisplayName(L"Chinese (Traditional)"), L"zh-Hant");
    CHECK_STREQ(LanguageCodeFromDisplayName(L"Portuguese"), L"pt");
    CHECK_STREQ(LanguageCodeFromDisplayName(L"Spanish"), L"es");
    CHECK_STREQ(LanguageCodeFromDisplayName(L"English"), L"en");
    CHECK_STREQ(LanguageCodeFromDisplayName(L"Auto-Detect"), L"auto");

    // Auto rules validation:
    // Translate: source 'auto' is valid, target 'auto' is INVALID
    CHECK(IsValidLanguageForOperation(L"auto", TransformOperation::Translate, false));
    CHECK(!IsValidLanguageForOperation(L"auto", TransformOperation::Translate, true));

    // CorrectAndRewrite: 'auto' is INVALID; explicit language is REQUIRED
    CHECK(!IsValidLanguageForOperation(L"auto", TransformOperation::CorrectAndRewrite, false));
    CHECK(IsValidLanguageForOperation(L"es", TransformOperation::CorrectAndRewrite, false));
    CHECK(IsValidLanguageForOperation(L"en", TransformOperation::CorrectAndRewrite, false));
    CHECK(IsValidLanguageForOperation(L"zh-Hans", TransformOperation::CorrectAndRewrite, false));
    CHECK(IsValidLanguageForOperation(L"zh-Hant", TransformOperation::CorrectAndRewrite, false));
}

TEST(config_string_display_mappers) {
    // TransformOperation mappers
    CHECK_STREQ(OperationName(TransformOperation::Translate), L"translate");
    CHECK_STREQ(OperationName(TransformOperation::CorrectAndRewrite), L"correct_and_rewrite");
    CHECK_STREQ(OperationName(static_cast<TransformOperation>(999)), L"translate");

    CHECK_STREQ(OperationDisplayName(TransformOperation::Translate), L"Translate");
    CHECK_STREQ(OperationDisplayName(TransformOperation::CorrectAndRewrite), L"Correct & Rewrite");
    CHECK_STREQ(OperationDisplayName(static_cast<TransformOperation>(999)), L"Translate");

    // TranslationProfile mappers
    CHECK_STREQ(ProfileName(TranslationProfile::Neutral), L"neutral");
    CHECK_STREQ(ProfileName(TranslationProfile::Professional), L"professional");
    CHECK_STREQ(ProfileName(TranslationProfile::Casual), L"casual");
    CHECK_STREQ(ProfileName(TranslationProfile::Slang), L"slang");
    CHECK_STREQ(ProfileName(TranslationProfile::Custom), L"custom");
    CHECK_STREQ(ProfileName(static_cast<TranslationProfile>(999)), L"neutral");

    // ProfileDisplayName for Translate operation
    CHECK_STREQ(ProfileDisplayName(TranslationProfile::Neutral, TransformOperation::Translate), L"Neutral (natural & clear)");
    CHECK_STREQ(ProfileDisplayName(TranslationProfile::Professional, TransformOperation::Translate), L"Professional (business / formal)");
    CHECK_STREQ(ProfileDisplayName(TranslationProfile::Casual, TransformOperation::Translate), L"Casual (friends & family)");
    CHECK_STREQ(ProfileDisplayName(TranslationProfile::Slang, TransformOperation::Translate), L"Slang (WhatsApp / Discord)");
    CHECK_STREQ(ProfileDisplayName(TranslationProfile::Custom, TransformOperation::Translate), L"Custom (user-defined instructions)");
    CHECK_STREQ(ProfileDisplayName(static_cast<TranslationProfile>(999), TransformOperation::Translate), L"Neutral (natural & clear)");

    // ProfileDisplayName for CorrectAndRewrite operation
    CHECK_STREQ(ProfileDisplayName(TranslationProfile::Neutral, TransformOperation::CorrectAndRewrite), L"Neutral (natural correction)");
    CHECK_STREQ(ProfileDisplayName(TranslationProfile::Professional, TransformOperation::CorrectAndRewrite), L"Professional (polished & formal)");
    CHECK_STREQ(ProfileDisplayName(TranslationProfile::Casual, TransformOperation::CorrectAndRewrite), L"Casual (friendly tone)");
    CHECK_STREQ(ProfileDisplayName(TranslationProfile::Slang, TransformOperation::CorrectAndRewrite), L"Slang (preserve colloquial voice)");
    CHECK_STREQ(ProfileDisplayName(TranslationProfile::Custom, TransformOperation::CorrectAndRewrite), L"Custom (user-defined instructions)");
    CHECK_STREQ(ProfileDisplayName(static_cast<TranslationProfile>(999), TransformOperation::CorrectAndRewrite), L"Neutral (natural & clear)");

    // TransformMode mappers
    CHECK_STREQ(ModeName(TransformMode::Translate), L"translate");
    CHECK_STREQ(ModeName(TransformMode::Rewrite), L"rewrite");
    CHECK_STREQ(ModeName(TransformMode::Grammar), L"grammar");
    CHECK_STREQ(ModeName(TransformMode::Professional), L"professional");
    CHECK_STREQ(ModeName(TransformMode::Casual), L"casual");
    CHECK_STREQ(ModeName(TransformMode::Technical), L"technical");
    CHECK_STREQ(ModeName(static_cast<TransformMode>(999)), L"translate");

    CHECK_STREQ(ModeUiLabel(TransformMode::Translate), L"Translate (Semi-Formal)");
    CHECK_STREQ(ModeUiLabel(TransformMode::Rewrite), L"Slang / Very Casual");
    CHECK_STREQ(ModeUiLabel(TransformMode::Grammar), L"Grammar Polish");
    CHECK_STREQ(ModeUiLabel(TransformMode::Professional), L"Formal / Professional");
    CHECK_STREQ(ModeUiLabel(TransformMode::Casual), L"Casual");
    CHECK_STREQ(ModeUiLabel(TransformMode::Technical), L"Technical");
    CHECK_STREQ(ModeUiLabel(static_cast<TransformMode>(999)), L"Translate");

    // ProviderId mappers
    CHECK_STREQ(ProviderName(ProviderId::Mock), L"mock");
    CHECK_STREQ(ProviderName(ProviderId::Gemini), L"gemini");
    CHECK_STREQ(ProviderName(ProviderId::OpenAI), L"openai");
    CHECK_STREQ(ProviderName(static_cast<ProviderId>(999)), L"mock");

    CHECK_STREQ(ProviderDisplayName(ProviderId::Mock), L"Mock (offline demo)");
    CHECK_STREQ(ProviderDisplayName(ProviderId::Gemini), L"Google Gemini");
    CHECK_STREQ(ProviderDisplayName(ProviderId::OpenAI), L"OpenAI");
    CHECK_STREQ(ProviderDisplayName(static_cast<ProviderId>(999)), L"Mock (offline demo)");

    // DefaultModelForProvider & DefaultEndpointForProvider
    CHECK_STREQ(DefaultModelForProvider(ProviderId::Mock), L"");
    CHECK_STREQ(DefaultModelForProvider(ProviderId::Gemini), L"gemini-3.7-flash");
    CHECK_STREQ(DefaultModelForProvider(ProviderId::OpenAI), L"gpt-5.6-luna");
    CHECK_STREQ(DefaultModelForProvider(static_cast<ProviderId>(999)), L"");

    CHECK_STREQ(DefaultEndpointForProvider(ProviderId::Mock), L"");
    CHECK_STREQ(DefaultEndpointForProvider(ProviderId::Gemini), L"https://generativelanguage.googleapis.com/v1beta");
    CHECK_STREQ(DefaultEndpointForProvider(ProviderId::OpenAI), L"https://api.openai.com/v1/responses");
    CHECK_STREQ(DefaultEndpointForProvider(static_cast<ProviderId>(999)), L"");
}