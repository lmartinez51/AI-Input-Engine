#pragma once

#include "Configuration/AppConfig.h"
#include "Configuration/LanguageRegistry.h"
#include "Translation/TranslationTypes.h"

#include <string>
#include <string_view>

namespace aiie {

// Constructs system/developer instructions for translation or correction/rewriting models.
std::wstring BuildTransformationInstructions(TransformOperation operation,
                                             std::wstring_view sourceLang,
                                             std::wstring_view targetLang,
                                             TranslationProfile profile,
                                             std::wstring_view customInstructions = L"");

// Convenience overload accepting a TranslationRequest.
std::wstring BuildInstructionsForRequest(const TranslationRequest& request);

// Formats the user message payload with optional context in a provider-neutral standard.
std::wstring FormatUserPayload(const TranslationRequest& request);

// Legacy backward-compatible overloads.
std::wstring BuildTranslationInstructions(std::wstring_view sourceLang,
                                          std::wstring_view targetLang,
                                          TranslationProfile profile,
                                          std::wstring_view customInstructions = L"");

std::wstring BuildTranslationInstructions(std::wstring_view sourceLang,
                                          std::wstring_view targetLang,
                                          TransformMode mode);

// Returns the style directive text for a given profile and operation.
std::wstring StyleDirectiveForProfile(TranslationProfile profile,
                                      TransformOperation operation = TransformOperation::Translate,
                                      std::wstring_view customInstructions = L"");

// Returns the style directive text for a legacy transform mode.
std::wstring StyleDirectiveForMode(TransformMode mode);

} // namespace aiie
