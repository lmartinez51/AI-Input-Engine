#pragma once

#include "Translation/TranslationTypes.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace aiie {

struct OpenAIConfig {
    std::wstring endpoint = L"https://api.openai.com/v1/responses";
    std::wstring model = L"gpt-5.6-luna";
    std::wstring apiKey; // supplied at runtime; never logged
    ModelParameters modelParams;
    int timeoutMs = 30000;
    int maxRetries = 2;
};

// OpenAI Responses API provider.
//
// - asynchronous worker thread (network never blocks input or UI threads)
// - configurable complete endpoint URL / model / key
// - per-request cancellation + CancelAll()
// - timeouts, retry with backoff on 429/5xx, structured error parsing
// - defensive multi-tier response parser (output_text, output[], choices[])
// - API key is never logged (redaction in Logging + careful error messages)
class OpenAIProvider : public ITranslationProvider {
public:
    explicit OpenAIProvider(OpenAIConfig config);
    ~OpenAIProvider() override;

    OpenAIProvider(const OpenAIProvider&) = delete;
    OpenAIProvider& operator=(const OpenAIProvider&) = delete;

    std::wstring_view Name() const override { return L"openai"; }
    std::wstring_view DisplayName() const override { return L"OpenAI"; }
    bool RequiresNetwork() const override { return true; }
    ProviderStatus Status() const override;
    void CancelAll() override;
    void Translate(TranslationRequest request, TranslationCallback callback) override;

    // Builds the JSON request body using the Responses API schema. Exposed for tests.
    static std::wstring BuildRequestBody(const TranslationRequest& request, std::wstring_view model);

    // Parses a response body. Exposed for tests. Returns false and fills `error` on malformed payloads.
    static bool ParseResponseBody(std::wstring_view body, std::wstring& outText, std::wstring& error);

    // Performs one synchronous translation request to verify endpoint/model/key connectivity.
    // Used by Settings "Test Connection" and the CLI diagnostic tool. Never logs the key.
    static bool TestConnection(const std::wstring& endpoint, const std::wstring& model,
                               const std::wstring& apiKey, int timeoutMs, std::wstring& outError);

private:
    struct Pending {
        TranslationRequest request;
        TranslationCallback callback;
        std::shared_ptr<std::atomic<bool>> cancelled;
    };

    void WorkerLoop();
    TranslationResult ExecuteOne(const TranslationRequest& req,
                                 const std::shared_ptr<std::atomic<bool>>& cancelled);

    OpenAIConfig config_;

    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Pending> queue_;
    std::atomic<bool> shutdown_{false};

    mutable std::mutex statusMutex_;
    std::atomic<ProviderStatus::State> state_{ProviderStatus::State::Ready};
    std::wstring statusMessage_;
};

} // namespace aiie
