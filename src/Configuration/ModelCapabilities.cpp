#include "Configuration/ModelCapabilities.h"

namespace aiie {

std::vector<ModelInfo> KnownModelsForProvider(ProviderId provider) {
    switch (provider) {
    case ProviderId::Gemini:
        return {
            {L"gemini-3.6-flash", L"Gemini 3.6 Flash (Fast & Recommended)"},
            {L"gemini-2.5-flash", L"Gemini 2.5 Flash"},
            {L"gemini-2.5-pro", L"Gemini 2.5 Pro"},
        };
    case ProviderId::OpenAI:
        return {
            {L"gpt-5.6-luna", L"GPT-5.6 Luna (Fast & Recommended)"},
            {L"gpt-5.4-mini", L"GPT-5.4 Mini"},
            {L"gpt-5.4", L"GPT-5.4"},
            {L"gpt-4o", L"GPT-4o"},
            {L"gpt-4o-mini", L"GPT-4o Mini"},
        };
    case ProviderId::Mock:
        return {};
    }
    return {};
}

ModelCapabilities GetModelCapabilities(ProviderId provider, std::wstring_view model) {
    ModelCapabilities caps;

    switch (provider) {
    case ProviderId::Mock:
        return caps;

    case ProviderId::OpenAI:
        caps.supportsTemperature = true;
        caps.minTemperature = 0.0;
        caps.maxTemperature = 2.0;
        caps.defaultTemperature = 1.0;

        if (model.starts_with(L"o1") || model.starts_with(L"o3")) {
            caps.supportsTemperature = false;
        }

        caps.supportsTopP = true;
        caps.minTopP = 0.0;
        caps.maxTopP = 1.0;
        caps.defaultTopP = 1.0;

        caps.supportsTopK = false;

        caps.supportsMaxOutputTokens = true;
        caps.defaultMaxOutputTokens = 1024;
        return caps;

    case ProviderId::Gemini:
        caps.supportsTemperature = true;
        caps.minTemperature = 0.0;
        caps.maxTemperature = 2.0;
        caps.defaultTemperature = 0.2;

        caps.supportsTopP = true;
        caps.minTopP = 0.0;
        caps.maxTopP = 1.0;
        caps.defaultTopP = 0.95;

        caps.supportsTopK = true;
        caps.minTopK = 1;
        caps.maxTopK = 100;
        caps.defaultTopK = 40;

        caps.supportsMaxOutputTokens = true;
        caps.defaultMaxOutputTokens = 1024;
        return caps;
    }

    return caps;
}

} // namespace aiie
