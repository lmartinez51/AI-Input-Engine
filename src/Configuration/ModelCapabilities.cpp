#include "Configuration/ModelCapabilities.h"

#include <algorithm>

namespace aiie {

std::vector<ModelInfo> KnownModelsForProvider(ProviderId provider) {
    switch (provider) {
    case ProviderId::Gemini:
        return {
            {L"gemini-3.7-flash", L"Gemini 3.7 Flash (Fast & Recommended)"},
            {L"gemini-3.6-flash", L"Gemini 3.6 Flash"},
            {L"gemini-3.5-flash-lite", L"Gemini 3.5 Flash-Lite (Ultra-Fast)"},
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

    case ProviderId::OpenAI: {
        caps.streamingSupported = true;
        caps.imageInputSupported = true;

        if (model.starts_with(L"o1") || model.starts_with(L"o3")) {
            // Pure reasoning models: temperature and top_p not supported by OpenAI REST API
            caps.reasoning.supported = true;
            caps.reasoning.defaultLevel = L"medium";
            caps.reasoning.allowedLevels = {L"low", L"medium", L"high"};

            caps.temperature.supported = false;
            caps.topP.supported = false;
            caps.topK.supported = false;

            caps.maxOutputTokens.supported = true;
            caps.maxOutputTokens.min = 1;
            caps.maxOutputTokens.max = 65536;
            caps.maxOutputTokens.defaultValue = 1024;
            return caps;
        }

        if (model.starts_with(L"gpt-5")) {
            // GPT-5 family: supports reasoning.effort (none, low, medium)
            caps.reasoning.supported = true;
            caps.reasoning.defaultLevel = L"none";
            caps.reasoning.allowedLevels = {L"none", L"low", L"medium"};

            caps.temperature.supported = true;
            caps.temperature.min = 0.0;
            caps.temperature.max = 2.0;
            caps.temperature.defaultValue = 1.0;

            caps.topP.supported = true;
            caps.topP.min = 0.0;
            caps.topP.max = 1.0;
            caps.topP.defaultValue = 1.0;

            caps.topK.supported = false;

            caps.maxOutputTokens.supported = true;
            caps.maxOutputTokens.min = 1;
            caps.maxOutputTokens.max = 16384;
            caps.maxOutputTokens.defaultValue = 1024;
            return caps;
        }

        // Standard autoregressive models (gpt-4o, gpt-4o-mini, etc.)
        caps.reasoning.supported = false;
        caps.reasoning.defaultLevel = L"none";
        caps.reasoning.allowedLevels.clear();

        caps.temperature.supported = true;
        caps.temperature.min = 0.0;
        caps.temperature.max = 2.0;
        caps.temperature.defaultValue = 1.0;

        caps.topP.supported = true;
        caps.topP.min = 0.0;
        caps.topP.max = 1.0;
        caps.topP.defaultValue = 1.0;

        caps.topK.supported = false;

        caps.maxOutputTokens.supported = true;
        caps.maxOutputTokens.min = 1;
        caps.maxOutputTokens.max = 16384;
        caps.maxOutputTokens.defaultValue = 1024;
        return caps;
    }

    case ProviderId::Gemini: {
        caps.streamingSupported = true;
        caps.imageInputSupported = true;

        caps.reasoning.supported = false;
        caps.reasoning.defaultLevel = L"none";
        caps.reasoning.allowedLevels.clear();

        caps.temperature.supported = true;
        caps.temperature.min = 0.0;
        caps.temperature.max = 2.0;
        caps.temperature.defaultValue = 0.2;

        caps.topP.supported = true;
        caps.topP.min = 0.0;
        caps.topP.max = 1.0;
        caps.topP.defaultValue = 0.95;

        caps.topK.supported = true;
        caps.topK.min = 1;
        caps.topK.max = 100;
        caps.topK.defaultValue = 40;

        caps.maxOutputTokens.supported = true;
        caps.maxOutputTokens.min = 1;
        caps.maxOutputTokens.max = 8192;
        caps.maxOutputTokens.defaultValue = 1024;
        return caps;
    }
    }

    return caps;
}

ModelParameters SanitizeModelParameters(ProviderId provider, std::wstring_view model, const ModelParameters& params) {
    const ModelCapabilities caps = GetModelCapabilities(provider, model);
    ModelParameters sanitized;

    if (caps.SupportsReasoningEffort() && params.reasoningEffort.has_value()) {
        const auto& allowed = caps.reasoning.allowedLevels;
        if (std::find(allowed.begin(), allowed.end(), *params.reasoningEffort) != allowed.end()) {
            sanitized.reasoningEffort = params.reasoningEffort;
        } else {
            sanitized.reasoningEffort = caps.reasoning.defaultLevel;
        }
    } else {
        sanitized.reasoningEffort.reset();
    }

    if (caps.SupportsTemperature() && params.temperature.has_value()) {
        bool omitTempDueToReasoning = false;
        if (sanitized.reasoningEffort.has_value() && *sanitized.reasoningEffort != L"none") {
            // When non-none reasoning effort is active on models supporting both, omit temperature override
            omitTempDueToReasoning = true;
        }
        if (!omitTempDueToReasoning) {
            sanitized.temperature = std::clamp(*params.temperature, caps.temperature.min, caps.temperature.max);
        }
    } else {
        sanitized.temperature.reset();
    }

    if (caps.SupportsTopP() && params.topP.has_value()) {
        sanitized.topP = std::clamp(*params.topP, caps.topP.min, caps.topP.max);
    } else {
        sanitized.topP.reset();
    }

    if (caps.SupportsTopK() && params.topK.has_value()) {
        sanitized.topK = std::clamp(*params.topK, caps.topK.min, caps.topK.max);
    } else {
        sanitized.topK.reset();
    }

    if (caps.SupportsMaxOutputTokens() && params.maxOutputTokens.has_value()) {
        sanitized.maxOutputTokens = std::clamp(*params.maxOutputTokens, caps.maxOutputTokens.min, caps.maxOutputTokens.max);
    } else {
        sanitized.maxOutputTokens.reset();
    }

    return sanitized;
}

} // namespace aiie
