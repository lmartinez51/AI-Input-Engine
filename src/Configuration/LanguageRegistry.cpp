#include "Configuration/LanguageRegistry.h"

namespace aiie {

namespace {

const std::vector<LanguageDescriptor>& BuildRegistry() {
    static const std::vector<LanguageDescriptor> kLanguages = {
        {L"auto", L"Auto-Detect", true, false, false},
        {L"en", L"English", true, true, true},
        {L"es", L"Spanish", true, true, true},
        {L"zh-Hans", L"Chinese (Simplified)", true, true, true},
        {L"zh-Hant", L"Chinese (Traditional)", true, true, true},
        {L"fr", L"French", true, true, true},
        {L"de", L"German", true, true, true},
        {L"pt", L"Portuguese", true, true, true},
        {L"it", L"Italian", true, true, true},
        {L"ja", L"Japanese", true, true, true},
    };
    return kLanguages;
}

} // namespace

const std::vector<LanguageDescriptor>& GetSupportedLanguages() {
    return BuildRegistry();
}

std::wstring_view LanguageDisplayName(std::wstring_view code) {
    if (code.empty()) {
        return L"";
    }
    for (const auto& lang : GetSupportedLanguages()) {
        if (lang.code == code) {
            return lang.displayName;
        }
    }
    return code; // Unknown codes pass through as-is
}

std::wstring_view LanguageCodeFromDisplayName(std::wstring_view displayName) {
    if (displayName.empty()) {
        return L"";
    }
    for (const auto& lang : GetSupportedLanguages()) {
        if (lang.displayName == displayName) {
            return lang.code;
        }
    }
    // Check if it was already a code
    for (const auto& lang : GetSupportedLanguages()) {
        if (lang.code == displayName) {
            return lang.code;
        }
    }
    return displayName;
}

bool IsSupportedLanguage(std::wstring_view code) {
    for (const auto& lang : GetSupportedLanguages()) {
        if (lang.code == code) {
            return true;
        }
    }
    return false;
}

bool IsValidLanguageForOperation(std::wstring_view code, TransformOperation op, bool isTarget) {
    for (const auto& lang : GetSupportedLanguages()) {
        if (lang.code == code) {
            if (op == TransformOperation::Translate) {
                return isTarget ? lang.allowAsTranslateTarget : lang.allowAsTranslateSource;
            } else if (op == TransformOperation::CorrectAndRewrite) {
                return lang.allowAsRewriteLanguage;
            }
        }
    }
    return false;
}

} // namespace aiie
