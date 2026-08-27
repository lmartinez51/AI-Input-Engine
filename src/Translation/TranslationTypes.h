#pragma once

#include "Configuration/AppConfig.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace aiie {

enum class CommitAuthority {
    Forbidden,  // Background / speculative / intermediate. NEVER allowed to touch screen.
    Allowed     // Strong completion signal or explicit user action. Authorized to commit replacement.
};

// Immutable data describing one transformation request.
struct TranslationRequest {
    std::wstring text;
    TransformOperation operation = TransformOperation::Translate;
    ProviderId providerId = ProviderId::Mock;
    std::wstring model;
    std::wstring sourceLanguage; // "auto", "es", "en", "zh-Hans", "zh-Hant", ...
    std::wstring targetLanguage;
    TranslationProfile profile = TranslationProfile::Neutral;
    std::wstring customInstructions;
    std::optional<std::wstring> reasoningEffort;
    TransformMode mode = TransformMode::Translate; // Kept for backward compatibility
    std::wstring context;    // previous sentence / surrounding context (may be empty)
    int64_t requestId = 0;   // assigned by the pipeline
    bool useCache = true;
    CommitAuthority commitAuthority = CommitAuthority::Forbidden;
    bool appendTrailingNewline = false;
    std::shared_ptr<std::atomic<bool>> cancellationToken;
};

struct TranslationResult {
    std::wstring text;
    bool ok = false;
    std::wstring error;           // user-presentable error message
    std::wstring errorDetail;     // diagnostic detail (provider-specific)
    std::wstring detectedSource;  // "es"/"en"/... when source was "auto"
    int64_t requestId = 0;
    bool fromCache = false;
    CommitAuthority commitAuthority = CommitAuthority::Forbidden;
    bool appendTrailingNewline = false;
};

// Provider health / connectivity status.
struct ProviderStatus {
    enum class State { Unknown, Ready, Busy, Error, Unavailable };
    State state = State::Unknown;
    std::wstring message;
};

using TranslationCallback = std::function<void(TranslationResult)>;

// A transformation provider. Implementations are asynchronous: Translate()
// returns immediately and invokes the callback (on a provider-owned thread)
// when the result is ready. Callbacks must be marshalled by the caller to the
// input thread if thread affinity is required.
class ITranslationProvider {
public:
    virtual ~ITranslationProvider() = default;

    virtual std::wstring_view Name() const = 0;
    virtual std::wstring_view DisplayName() const = 0;
    virtual bool RequiresNetwork() const = 0;
    virtual ProviderStatus Status() const = 0;

    // Cancels all in-flight requests. Pending callbacks may still fire with
    // ok=false ("cancelled"); the pipeline versioning discards them anyway.
    virtual void CancelAll() = 0;

    // Asynchronous request. The provider owns the request lifecycle.
    virtual void Translate(TranslationRequest request, TranslationCallback callback) = 0;
};

} // namespace aiie