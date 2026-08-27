#pragma once

#include "Configuration/AppConfig.h"
#include "Core/Dispatcher.h"
#include "Core/Logging.h"
#include "Translation/TranslationPipeline.h"
#include "Translation/TranslationTypes.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace aiie {

// Encapsulates the entire transformation-affecting parameter tuple for exact candidate identity comparison.
struct TransformationIdentity {
    ProviderId providerId = ProviderId::Mock;
    std::wstring model;
    std::wstring sourceLanguage;
    std::wstring targetLanguage;
    TransformOperation operation = TransformOperation::Translate;
    TranslationProfile profile = TranslationProfile::Neutral;
    std::wstring customInstructions;
    std::optional<std::wstring> reasoningEffort;

    bool operator==(const TransformationIdentity& o) const {
        return providerId == o.providerId &&
               model == o.model &&
               sourceLanguage == o.sourceLanguage &&
               targetLanguage == o.targetLanguage &&
               operation == o.operation &&
               profile == o.profile &&
               customInstructions == o.customInstructions &&
               reasoningEffort == o.reasoningEffort;
    }

    bool operator!=(const TransformationIdentity& o) const {
        return !(*this == o);
    }
};

// Isolated candidate representation produced by background speculative streaming.
struct SpeculativeTranslationCandidate {
    std::wstring sourceText;
    std::wstring translatedText;
    TransformationIdentity identity;
    int64_t speculativeRequestId = 0;
    std::chrono::steady_clock::time_point requestStartTimestamp{};
    std::chrono::steady_clock::time_point completionTimestamp{};
    int64_t ttftMs = 0;
    int64_t totalLatencyMs = 0;
    bool completed = false;
    bool ok = false;
    std::wstring error;
};

// Internal diagnostic metrics for speculative translation (PCHP Telemetry).
struct SpeculativeTelemetry {
    int64_t requestsStarted = 0;
    int64_t requestsCancelled = 0;
    int64_t requestsCompleted = 0;
    int64_t candidateHits = 0;
    int64_t candidateHitsStrict = 0;
    int64_t candidateHitsNormalized = 0;
    int64_t candidateMisses = 0;
    int64_t candidateMissNotReady = 0;
    int64_t candidateMissSourceMismatch = 0;
    int64_t candidateMissConfigMismatch = 0;
    int64_t finalRequestsAvoided = 0;
    int64_t lastCandidateLeadTimeMs = 0;
    int64_t lastHesitationDurationMs = 0;
    int64_t lastSpeculativeTtftMs = 0;
    int64_t lastSpeculativeTotalLatencyMs = 0;
};

// Isolated Pre-Commit Hesitation Prediction (PCHP) speculative translation manager.
// Coordinates speculative background requests, candidate caching, cancellation,
// and exact-match + Policy 2 terminal normalization verification without owning
// TSF, hooks, clipboard, or injection.
class SpeculativeTranslationManager {
public:
    // Canonical PCHP Production Constants
    static constexpr int kPchpHesitationThresholdMs = 450;
    static constexpr int kPchpMinBufferLength = 10;
    static constexpr int kPchpMinGrowthChars = 8;
    static constexpr int kPchpCooldownMs = 750;

    SpeculativeTranslationManager(bool enabled = false, int debounceMs = kPchpHesitationThresholdMs, int minChars = kPchpMinBufferLength);
    ~SpeculativeTranslationManager();

    void Configure(bool enabled, int debounceMs = kPchpHesitationThresholdMs, int minChars = kPchpMinBufferLength);
    bool IsEnabled() const;

    // Evaluates whether a new speculative request should be started given PCHP gates.
    bool ShouldTriggerSpeculative(std::wstring_view newBuffer, const TransformationIdentity& identity) const;

    // Starts a speculative translation request via the pipeline.
    // Automatically cancels and replaces any previous in-flight speculative request (at most 1 active).
    void StartSpeculativeRequest(int64_t requestId, const std::wstring& snapshot,
                                 const TransformationIdentity& identity,
                                 std::shared_ptr<TranslationPipeline> pipeline,
                                 std::shared_ptr<IDispatcher> dispatcher);

    // Callback when a speculative translation finishes.
    void OnSpeculativeResult(int64_t requestId, TranslationResult result,
                             const std::wstring& snapshot, const TransformationIdentity& identity,
                             int64_t totalLatencyMs);

    // Attempts to atomically consume a completed exact-match candidate for the final request.
    // Supports strict exact match and Policy 2 terminal punctuation normalization.
    // Returns true and populates outResult (with CommitAuthority::Allowed) if match exists.
    bool TryConsumeExactCandidate(const TranslationRequest& finalReq, TranslationResult& outResult, bool* outNormalized = nullptr);

    // Invalidates any stored candidate (e.g. on text edit, backspace).
    // PRESERVES lastRequestedSnapshot_ and lastSpeculativeStartTime_ scheduler history.
    void InvalidateCandidate();

    // Resets the entire scheduler history (called ONLY on sentence commit, focus change, identity change).
    void ResetSchedulerHistory();

    // Cancels any active speculative network request without clearing existing completed candidate unless specified.
    void CancelActiveSpeculative();

    // Queries
    std::optional<SpeculativeTranslationCandidate> CurrentCandidate() const;
    int64_t ActiveSpeculativeRequestId() const;
    std::wstring LastRequestedSnapshot() const;
    SpeculativeTelemetry GetTelemetry() const;
    void ResetTelemetry();

private:
    struct QueuedSnapshot {
        int64_t nextId = 0;
        std::wstring text;
        TransformationIdentity identity;
        std::weak_ptr<TranslationPipeline> pipeline;
        std::weak_ptr<IDispatcher> dispatcher;
    };

    void StartSpeculativeRequestInternal(int64_t requestId, const std::wstring& snapshot,
                                         const TransformationIdentity& identity,
                                         std::shared_ptr<TranslationPipeline> pipeline,
                                         std::shared_ptr<IDispatcher> dispatcher);

    mutable std::recursive_mutex mutex_;
    bool enabled_ = false;
    int debounceMs_ = kPchpHesitationThresholdMs;
    int minChars_ = kPchpMinBufferLength;

    // Single in-flight speculative request tracker (physical & logical separation)
    int64_t physicallyActiveRequestId_ = 0;
    int64_t logicalActiveRequestId_ = 0;
    std::wstring activeSnapshot_;
    TransformationIdentity activeIdentity_;
    std::shared_ptr<std::atomic<bool>> activeCancelled_;
    std::chrono::steady_clock::time_point lastSpeculativeStartTime_{};

    // Queued newest snapshot waiting for physical slot release
    std::optional<QueuedSnapshot> queuedSnapshot_;

    // Last completed candidate
    std::optional<SpeculativeTranslationCandidate> candidate_;

    // Snapshot of the text when the last speculative request was initiated
    std::wstring lastRequestedSnapshot_;

    // Telemetry
    SpeculativeTelemetry telemetry_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
};

} // namespace aiie
