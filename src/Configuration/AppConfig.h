#pragma once

#include "Core/Hotkey.h"
#include "Core/Logging.h"

#include <optional>
#include <string>
#include <string_view>

namespace aiie {

// First-class Transformation Operations.
enum class TransformOperation {
    Translate = 0,      // Cross-language translation (e.g. ES -> EN, ES -> zh-Hans)
    CorrectAndRewrite,  // Same-language grammar, spelling, punctuation & tone rewrite (e.g. ES -> ES)
};

std::wstring_view OperationName(TransformOperation op);
std::wstring_view OperationDisplayName(TransformOperation op);
bool TryParseOperation(std::wstring_view name, TransformOperation& out);

// First-class Profiles governing linguistic style, tone, and register.
enum class TranslationProfile {
    Neutral = 0,    // Default: Natural, clear, polite everyday language / natural correction
    Professional,   // Workplace, business, executive, formal communication
    Casual,         // Friends, family, informal, natural conversational tone
    Slang,          // Very casual, WhatsApp/Discord, contemporary colloquialisms & personality
    Custom,         // User-defined instructions with core safety constraints
};

std::wstring_view ProfileName(TranslationProfile profile);
std::wstring_view ProfileDisplayName(TranslationProfile profile,
                                     TransformOperation op = TransformOperation::Translate);
bool TryParseProfile(std::wstring_view name, TranslationProfile& out);

// Transformation modes (kept for backward compatibility with legacy configs).
enum class TransformMode {
    Translate = 0,
    Rewrite,
    Grammar,
    Professional,
    Casual,
    Technical,
};

std::wstring_view ModeName(TransformMode mode);
bool TryParseMode(std::wstring_view name, TransformMode& out);
std::wstring_view ModeUiLabel(TransformMode mode);

enum class ProviderId {
    Mock = 0,
    Gemini,
    OpenAI,
};

std::wstring_view ProviderName(ProviderId id);
std::wstring_view ProviderDisplayName(ProviderId id);
bool TryParseProvider(std::wstring_view name, ProviderId& out);

std::wstring DefaultModelForProvider(ProviderId id);
std::wstring DefaultEndpointForProvider(ProviderId id);

// Provider-neutral sampling / execution parameters.
struct ModelParameters {
    std::optional<double> temperature;
    std::optional<double> topP;
    std::optional<int> topK;
    std::optional<int> maxOutputTokens;
    std::optional<std::wstring> reasoningEffort; // e.g. "none", "low", "medium"
};

struct AppConfig {
    bool enabled = true;

    // Transformation settings.
    TransformOperation operation = TransformOperation::Translate;
    std::wstring sourceLanguage = L"auto";
    std::wstring targetLanguage = L"en";
    TranslationProfile profile = TranslationProfile::Neutral;
    std::wstring customProfileInstructions;
    TransformMode mode = TransformMode::Translate; // Kept for backward compatibility

    // Provider & model settings.
    ProviderId provider = ProviderId::Mock;
    std::wstring model;       // if empty, resolved via DefaultModelForProvider(provider)
    std::wstring endpoint;    // complete HTTP URL; if empty, resolved via DefaultEndpointForProvider(provider)
    ModelParameters modelParams;

    // NOTE: API keys are NOT part of AppConfig. Secrets live in Windows
    // Credential Manager (CredentialStore); config.json holds only non-secret
    // settings.

    // Live-transformation behavior.
    int debounceMs = 500;
    int minChars = 5;
    int maxBufferChars = 400;
    int requestTimeoutMs = 60000;
    int maxRetries = 2;

    // Smart Paste settings.
    bool smartPasteEnabled = true;

    // Hotkeys.
    Hotkey hotkeyToggle{MOD_CONTROL | MOD_ALT, VK_SPACE};
    Hotkey hotkeySwap{MOD_CONTROL | MOD_ALT, L'S'};
    Hotkey hotkeyDiagnostics{MOD_CONTROL | MOD_ALT, L'D'};
    Hotkey hotkeySmartPaste{MOD_CONTROL | MOD_SHIFT, 'V'};

    // Logging / privacy.
    LogLevel logLevel = LogLevel::Info;
    bool logRawText = false;
    bool warnOnExternalSend = true;

    // Cache.
    bool cacheEnabled = true;
    int cacheMaxEntries = 200;
};

} // namespace aiie