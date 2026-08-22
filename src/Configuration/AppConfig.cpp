#include "Configuration/AppConfig.h"

namespace aiie {

std::wstring_view OperationName(TransformOperation op) {
    switch (op) {
    case TransformOperation::Translate: return L"translate";
    case TransformOperation::CorrectAndRewrite: return L"correct_and_rewrite";
    }
    return L"translate";
}

std::wstring_view OperationDisplayName(TransformOperation op) {
    switch (op) {
    case TransformOperation::Translate: return L"Translate";
    case TransformOperation::CorrectAndRewrite: return L"Correct & Rewrite";
    }
    return L"Translate";
}

bool TryParseOperation(std::wstring_view name, TransformOperation& out) {
    if (name == L"translate") {
        out = TransformOperation::Translate;
        return true;
    }
    if (name == L"correct_and_rewrite" || name == L"correct" || name == L"rewrite") {
        out = TransformOperation::CorrectAndRewrite;
        return true;
    }
    return false;
}

std::wstring_view ProfileName(TranslationProfile profile) {
    switch (profile) {
    case TranslationProfile::Neutral: return L"neutral";
    case TranslationProfile::Professional: return L"professional";
    case TranslationProfile::Casual: return L"casual";
    case TranslationProfile::Slang: return L"slang";
    case TranslationProfile::Custom: return L"custom";
    }
    return L"neutral";
}

std::wstring_view ProfileDisplayName(TranslationProfile profile, TransformOperation op) {
    if (op == TransformOperation::CorrectAndRewrite) {
        switch (profile) {
        case TranslationProfile::Neutral: return L"Neutral (natural correction)";
        case TranslationProfile::Professional: return L"Professional (polished & formal)";
        case TranslationProfile::Casual: return L"Casual (friendly tone)";
        case TranslationProfile::Slang: return L"Slang (preserve colloquial voice)";
        case TranslationProfile::Custom: return L"Custom (user-defined instructions)";
        }
    } else {
        switch (profile) {
        case TranslationProfile::Neutral: return L"Neutral (natural & clear)";
        case TranslationProfile::Professional: return L"Professional (business / formal)";
        case TranslationProfile::Casual: return L"Casual (friends & family)";
        case TranslationProfile::Slang: return L"Slang (WhatsApp / Discord)";
        case TranslationProfile::Custom: return L"Custom (user-defined instructions)";
        }
    }
    return L"Neutral (natural & clear)";
}

bool TryParseProfile(std::wstring_view name, TranslationProfile& out) {
    if (name == L"neutral" || name == L"translate") {
        out = TranslationProfile::Neutral;
        return true;
    }
    if (name == L"professional") {
        out = TranslationProfile::Professional;
        return true;
    }
    if (name == L"casual") {
        out = TranslationProfile::Casual;
        return true;
    }
    if (name == L"slang" || name == L"rewrite") {
        out = TranslationProfile::Slang;
        return true;
    }
    if (name == L"custom") {
        out = TranslationProfile::Custom;
        return true;
    }
    return false;
}

std::wstring_view ModeName(TransformMode mode) {
    switch (mode) {
    case TransformMode::Translate: return L"translate";
    case TransformMode::Rewrite: return L"rewrite";
    case TransformMode::Grammar: return L"grammar";
    case TransformMode::Professional: return L"professional";
    case TransformMode::Casual: return L"casual";
    case TransformMode::Technical: return L"technical";
    }
    return L"translate";
}

std::wstring_view ModeUiLabel(TransformMode mode) {
    switch (mode) {
    case TransformMode::Translate: return L"Translate (Semi-Formal)";
    case TransformMode::Rewrite: return L"Slang / Very Casual";
    case TransformMode::Grammar: return L"Grammar Polish";
    case TransformMode::Professional: return L"Formal / Professional";
    case TransformMode::Casual: return L"Casual";
    case TransformMode::Technical: return L"Technical";
    }
    return L"Translate";
}

bool TryParseMode(std::wstring_view name, TransformMode& out) {
    for (int i = 0; i <= static_cast<int>(TransformMode::Technical); ++i) {
        const auto m = static_cast<TransformMode>(i);
        if (ModeName(m) == name) {
            out = m;
            return true;
        }
    }
    return false;
}

std::wstring_view ProviderName(ProviderId id) {
    switch (id) {
    case ProviderId::Mock: return L"mock";
    case ProviderId::Gemini: return L"gemini";
    case ProviderId::OpenAI: return L"openai";
    }
    return L"mock";
}

std::wstring_view ProviderDisplayName(ProviderId id) {
    switch (id) {
    case ProviderId::Mock: return L"Mock (offline demo)";
    case ProviderId::Gemini: return L"Google Gemini";
    case ProviderId::OpenAI: return L"OpenAI";
    }
    return L"Mock (offline demo)";
}

bool TryParseProvider(std::wstring_view name, ProviderId& out) {
    if (name == L"mock") {
        out = ProviderId::Mock;
        return true;
    }
    if (name == L"gemini") {
        out = ProviderId::Gemini;
        return true;
    }
    if (name == L"openai") {
        out = ProviderId::OpenAI;
        return true;
    }
    return false;
}

std::wstring DefaultModelForProvider(ProviderId id) {
    switch (id) {
    case ProviderId::Mock:
        return L"";
    case ProviderId::Gemini:
        return L"gemini-3.6-flash";
    case ProviderId::OpenAI:
        return L"gpt-5.6-luna";
    }
    return L"";
}

std::wstring DefaultEndpointForProvider(ProviderId id) {
    switch (id) {
    case ProviderId::Mock:
        return L"";
    case ProviderId::Gemini:
        return L"https://generativelanguage.googleapis.com/v1beta";
    case ProviderId::OpenAI:
        return L"https://api.openai.com/v1/responses";
    }
    return L"";
}

} // namespace aiie