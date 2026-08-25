#pragma once

#include "Translation/TranslationTypes.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace aiie {

struct GeminiConfig {
    std::wstring endpoint = L"https://generativelanguage.googleapis.com/v1beta";
    std::wstring model = L"gemini-3.7-flash";
    std::wstring apiKey; // supplied by the user at runtime; never logged
    ModelParameters modelParams;
    int timeoutMs = 30000;
    int maxRetries = 2;
};

// Google Gemini (generativelanguage API) provider.
//
// - asynchronous worker thread (network never blocks input or UI threads)
// - configurable endpoint / model / key
// - per-request cancellation + CancelAll()
// - timeouts, retry with backoff on 429/5xx, structured error parsing
// - API key is never logged (redaction in Logging + careful messages)
class GeminiProvider : public ITranslationProvider {
public:
    explicit GeminiProvider(GeminiConfig config);
    ~GeminiProvider() override;

    GeminiProvider(const GeminiProvider&) = delete;
    GeminiProvider& operator=(const GeminiProvider&) = delete;

    std::wstring_view Name() const override { return L"gemini"; }
    std::wstring_view DisplayName() const override { return L"Google Gemini"; }
    bool RequiresNetwork() const override { return true; }
    ProviderStatus Status() const override;
    void CancelAll() override;
    void Translate(TranslationRequest request, TranslationCallback callback) override;

    // Builds the JSON request body. Exposed for tests.
    static std::wstring BuildRequestBody(const TranslationRequest& request, std::wstring_view model);
    // Parses a generateContent response body. Exposed for tests. Returns false
    // and fills `error` on malformed payloads.
    static bool ParseResponseBody(std::wstring_view body, std::wstring& outText,
                                  std::wstring& outDetectedSource, std::wstring& error);

    // Performs one synchronous generateContent request and reports whether the
    // endpoint/model/key combination works. Used by the Settings "Test
    // Connection" action. Never logs the key. Returns false on failure.
    static bool TestConnection(const std::wstring& endpoint, const std::wstring& model,
                               const std::wstring& apiKey, int timeoutMs, std::wstring& outError);

    // Fetches the list of available model ids (GET /models). Never logs the
    // key. Returns false on failure.
    static bool ListModels(const std::wstring& endpoint, const std::wstring& apiKey, int timeoutMs,
                           std::vector<std::wstring>& outModelIds, std::wstring& outError);

private:
    struct Pending {
        TranslationRequest request;
        TranslationCallback callback;
        std::shared_ptr<std::atomic<bool>> cancelled;
    };

    void WorkerLoop();
    void Process(Pending p);
    // Returns true on success. On failure, sets result fields.
    bool Execute(Pending& p, TranslationResult& out);

    GeminiConfig config_;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Pending> queue_;
    std::shared_ptr<std::atomic<bool>> activeCancelled_;
    std::atomic<bool> running_{true};
    std::atomic<bool> shutdown_{false};

    std::atomic<ProviderStatus::State> state_{ProviderStatus::State::Ready};
    mutable std::mutex statusMutex_;
    std::wstring statusMessage_;
    std::atomic<uint64_t> completed_{0};
    std::atomic<uint64_t> failed_{0};
};

} // namespace aiie