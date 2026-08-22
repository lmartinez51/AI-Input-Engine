#pragma once

#include "Configuration/AppConfig.h"

#include <string>
#include <string_view>
#include <vector>

namespace aiie {

struct LanguageDescriptor {
    std::wstring_view code;
    std::wstring_view displayName;
    bool allowAsTranslateSource = true;
    bool allowAsTranslateTarget = true;
    bool allowAsRewriteLanguage = true;
};

// Returns the authoritative list of supported languages.
const std::vector<LanguageDescriptor>& GetSupportedLanguages();

// Returns the human-readable display name for a language code (e.g. "es" -> "Spanish").
std::wstring_view LanguageDisplayName(std::wstring_view code);

// Returns the internal language code for a display name (e.g. "Spanish" -> "es").
std::wstring_view LanguageCodeFromDisplayName(std::wstring_view displayName);

// Returns true if the language code is recognized.
bool IsSupportedLanguage(std::wstring_view code);

// Validates if a language is valid for a given operation context.
bool IsValidLanguageForOperation(std::wstring_view code, TransformOperation op, bool isTarget = false);

} // namespace aiie
