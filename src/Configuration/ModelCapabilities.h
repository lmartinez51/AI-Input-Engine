#pragma once

#include "Configuration/AppConfig.h"

#include <string>
#include <string_view>
#include <vector>

namespace aiie {

struct ModelInfo {
    std::wstring id;
    std::wstring displayName;
};

struct NumericCapability {
    bool supported = false;
    double min = 0.0;
    double max = 0.0;
    double defaultValue = 0.0;
};

struct IntCapability {
    bool supported = false;
    int min = 0;
    int max = 0;
    int defaultValue = 0;
};

struct ReasoningCapability {
    bool supported = false;
    std::wstring defaultLevel = L"none";
    std::vector<std::wstring> allowedLevels; // e.g. {"none", "low", "medium"} or {"low", "medium", "high"}
};

// Capability descriptors for provider/model combinations.
// Controls which advanced parameters are exposed in the UI and sent over the wire.
struct ModelCapabilities {
    bool streamingSupported = false;
    bool imageInputSupported = false;

    ReasoningCapability reasoning;
    NumericCapability temperature;
    NumericCapability topP;
    IntCapability topK;
    IntCapability maxOutputTokens;

    // Backward compatibility helper accessors
    bool SupportsTemperature() const { return temperature.supported; }
    bool SupportsTopP() const { return topP.supported; }
    bool SupportsTopK() const { return topK.supported; }
    bool SupportsMaxOutputTokens() const { return maxOutputTokens.supported; }
    bool SupportsReasoningEffort() const { return reasoning.supported; }
    bool SupportsStreaming() const { return streamingSupported; }
    bool SupportsImageInput() const { return imageInputSupported; }
};

// Returns known model list for a given provider.
std::vector<ModelInfo> KnownModelsForProvider(ProviderId provider);

// Resolves capabilities for a given provider and model name.
ModelCapabilities GetModelCapabilities(ProviderId provider, std::wstring_view model);

// Validates and sanitizes model parameters against model capabilities.
// Clears or clamps unsupported/out-of-bounds parameters and resolves conflicts.
ModelParameters SanitizeModelParameters(ProviderId provider, std::wstring_view model, const ModelParameters& params);

} // namespace aiie
