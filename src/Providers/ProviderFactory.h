#pragma once

#include "Configuration/AppConfig.h"
#include "Providers/GeminiProvider.h"
#include "Providers/MockProvider.h"
#include "Providers/OpenAIProvider.h"

#include <memory>

namespace aiie {

// Creates the configured provider. The core never depends on a concrete
// provider class; only this factory knows them.
//
// API key precedence (documented in docs/DesignDecisions.md):
//   1. Secure application credential (Windows Credential Manager)
//   2. Environment variable fallback (GEMINI_API_KEY / OPENAI_API_KEY)
//   3. No credential -> provider reports "API key is not configured"
// Environment variables are never written over stored credentials.
class ProviderFactory {
public:
    static std::shared_ptr<ITranslationProvider> Create(const AppConfig& config);
};

} // namespace aiie