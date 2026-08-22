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

// Capability descriptors for provider/model combinations.
// Controls which advanced parameters are exposed in the UI and sent over the wire.
struct ModelCapabilities {
    bool supportsTemperature = false;
    double minTemperature = 0.0;
    double maxTemperature = 2.0;
    double defaultTemperature = 1.0;

    bool supportsTopP = false;
    double minTopP = 0.0;
    double maxTopP = 1.0;
    double defaultTopP = 1.0;

    bool supportsTopK = false;
    int minTopK = 1;
    int maxTopK = 100;
    int defaultTopK = 40;

    bool supportsMaxOutputTokens = false;
    int defaultMaxOutputTokens = 1024;
};

// Returns known model list for a given provider.
std::vector<ModelInfo> KnownModelsForProvider(ProviderId provider);

// Resolves capabilities for a given provider and model name.
ModelCapabilities GetModelCapabilities(ProviderId provider, std::wstring_view model);

} // namespace aiie
