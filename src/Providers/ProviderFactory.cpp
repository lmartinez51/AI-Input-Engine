#include "Providers/ProviderFactory.h"

#include "Configuration/CredentialStore.h"

#include <windows.h>

#include <cstdlib>
#include <string>

namespace aiie {

namespace {

// Development/testing credential fallback from environment variable.
// Never returns more than 4096 bytes.
std::wstring EnvApiKey(const wchar_t* varName) {
    wchar_t buf[4096] = {};
    const DWORD n = GetEnvironmentVariableW(varName, buf, 4096);
    return n > 0 && n < 4096 ? std::wstring(buf) : std::wstring();
}

} // namespace

std::shared_ptr<ITranslationProvider> ProviderFactory::Create(const AppConfig& config) {
    switch (config.provider) {
    case ProviderId::Mock:
        return std::make_shared<MockProvider>();
    case ProviderId::Gemini: {
        GeminiConfig gc;
        gc.endpoint = config.endpoint.empty() ? DefaultEndpointForProvider(ProviderId::Gemini) : config.endpoint;
        gc.model = config.model.empty() ? DefaultModelForProvider(ProviderId::Gemini) : config.model;
        gc.modelParams = config.modelParams;
        gc.timeoutMs = config.requestTimeoutMs;
        gc.maxRetries = config.maxRetries;
        // Precedence: stored credential > environment variable > none.
        if (!CredentialStore::GetGeminiApiKey(gc.apiKey)) {
            gc.apiKey = EnvApiKey(L"GEMINI_API_KEY");
        }
        return std::make_shared<GeminiProvider>(std::move(gc));
    }
    case ProviderId::OpenAI: {
        OpenAIConfig oc;
        oc.endpoint = config.endpoint.empty() ? DefaultEndpointForProvider(ProviderId::OpenAI) : config.endpoint;
        oc.model = config.model.empty() ? DefaultModelForProvider(ProviderId::OpenAI) : config.model;
        oc.modelParams = config.modelParams;
        oc.timeoutMs = config.requestTimeoutMs;
        oc.maxRetries = config.maxRetries;
        // Precedence: stored credential > environment variable > none.
        if (!CredentialStore::GetOpenAIApiKey(oc.apiKey)) {
            oc.apiKey = EnvApiKey(L"OPENAI_API_KEY");
        }
        return std::make_shared<OpenAIProvider>(std::move(oc));
    }
    }
    return std::make_shared<MockProvider>();
}

} // namespace aiie