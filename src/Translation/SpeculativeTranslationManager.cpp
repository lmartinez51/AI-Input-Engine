#include "Translation/SpeculativeTranslationManager.h"

#include "Core/Logging.h"

#include <cmath>

namespace aiie {

SpeculativeTranslationManager::SpeculativeTranslationManager(bool enabled, int debounceMs, int minChars)
    : enabled_(enabled), debounceMs_(debounceMs), minChars_(minChars) {}

SpeculativeTranslationManager::~SpeculativeTranslationManager() {
    if (alive_) {
        alive_->store(false);
    }
    CancelActiveSpeculative();
}

void SpeculativeTranslationManager::Configure(bool enabled, int debounceMs, int minChars) {
    std::lock_guard lock(mutex_);
    enabled_ = enabled;
    debounceMs_ = std::max(debounceMs, kPchpHesitationThresholdMs);
    minChars_ = std::max(minChars, kPchpMinBufferLength);
    if (!enabled_) {
        InvalidateCandidate();
        ResetSchedulerHistory();
    }
}

bool SpeculativeTranslationManager::IsEnabled() const {
    std::lock_guard lock(mutex_);
    return enabled_;
}

bool SpeculativeTranslationManager::ShouldTriggerSpeculative(std::wstring_view newBuffer,
                                                             const TransformationIdentity& identity) const {
    std::lock_guard lock(mutex_);
    if (!enabled_) {
        return false;
    }

    const int effectiveMin = std::max(minChars_, kPchpMinBufferLength);
    if (static_cast<int>(newBuffer.size()) < effectiveMin) {
        Logger::Instance().Debug(cat::Translation,
                                 L"[PCHP] gate-reject reason=min-buffer len={} min={}",
                                 newBuffer.size(), effectiveMin);
        return false;
    }

    // Cooldown gate: prevent rapid re-speculation
    const auto now = std::chrono::steady_clock::now();
    if (lastSpeculativeStartTime_.time_since_epoch().count() > 0) {
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSpeculativeStartTime_).count();
        if (elapsedMs < kPchpCooldownMs) {
            Logger::Instance().Debug(cat::Translation,
                                     L"[PCHP] gate-reject reason=cooldown elapsed={}ms required={}ms",
                                     elapsedMs, kPchpCooldownMs);
            return false;
        }
    }

    // Candidate Ready Suppression: do not re-speculate if an exact matching candidate is already completed
    if (candidate_.has_value() && candidate_->completed && candidate_->ok) {
        if (candidate_->sourceText == newBuffer && candidate_->identity == identity) {
            Logger::Instance().Debug(cat::Translation,
                                     L"[PCHP] gate-reject reason=candidate-ready-suppression len={}",
                                     newBuffer.size());
            return false;
        }
    }

    // Growth gate: require minimum character growth since previous speculative request
    if (!lastRequestedSnapshot_.empty()) {
        if (newBuffer == lastRequestedSnapshot_ && identity == activeIdentity_) {
            Logger::Instance().Debug(cat::Translation,
                                     L"[PCHP] gate-reject reason=identical-snapshot len={}",
                                     newBuffer.size());
            return false;
        }
        if (newBuffer.starts_with(lastRequestedSnapshot_)) {
            const int growth = static_cast<int>(newBuffer.size()) - static_cast<int>(lastRequestedSnapshot_.size());
            if (growth < kPchpMinGrowthChars) {
                Logger::Instance().Debug(cat::Translation,
                                         L"[PCHP] gate-reject reason=growth delta={} min={} baseline={} current={}",
                                         growth, kPchpMinGrowthChars, lastRequestedSnapshot_.size(), newBuffer.size());
                return false;
            }
            Logger::Instance().Debug(cat::Translation,
                                     L"[PCHP] gate-allow growth={} baseline={} current={}",
                                     growth, lastRequestedSnapshot_.size(), newBuffer.size());
        } else {
            const int diff = std::abs(static_cast<int>(newBuffer.size()) - static_cast<int>(lastRequestedSnapshot_.size()));
            if (diff < kPchpMinGrowthChars) {
                Logger::Instance().Debug(cat::Translation,
                                         L"[PCHP] gate-reject reason=growth delta={} min={} baseline={} current={}",
                                         diff, kPchpMinGrowthChars, lastRequestedSnapshot_.size(), newBuffer.size());
                return false;
            }
            Logger::Instance().Debug(cat::Translation,
                                     L"[PCHP] gate-allow growth={} baseline={} current={}",
                                     diff, lastRequestedSnapshot_.size(), newBuffer.size());
        }
    } else {
        Logger::Instance().Debug(cat::Translation,
                                 L"[PCHP] gate-allow first-speculation len={}",
                                 newBuffer.size());
    }

    return true;
}

void SpeculativeTranslationManager::StartSpeculativeRequest(int64_t requestId, const std::wstring& snapshot,
                                                            const TransformationIdentity& identity,
                                                            std::shared_ptr<TranslationPipeline> pipeline,
                                                            std::shared_ptr<IDispatcher> dispatcher) {
    if (!pipeline) {
        return;
    }

    std::lock_guard lock(mutex_);
    if (!enabled_) {
        return;
    }

    if (physicallyActiveRequestId_ != 0) {
        // Physical slot is occupied. Cancel in-flight request physically and queue newest snapshot
        if (activeCancelled_) {
            activeCancelled_->store(true);
            Logger::Instance().Debug(cat::Translation, L"[PCHP] speculative-cancel req=#{} reason=superseded", physicallyActiveRequestId_);
            ++telemetry_.requestsCancelled;
        }
        logicalActiveRequestId_ = 0; // Stale in-flight result will be discarded on finish
        queuedSnapshot_ = QueuedSnapshot{requestId, snapshot, identity, pipeline, dispatcher};
        lastRequestedSnapshot_ = snapshot;
        Logger::Instance().Debug(cat::Translation, L"[PCHP] speculative-queued req=#{} len={} (waiting_slot)",
                                 requestId, snapshot.size());
        return;
    }

    StartSpeculativeRequestInternal(requestId, snapshot, identity, std::move(pipeline), std::move(dispatcher));
}

void SpeculativeTranslationManager::StartSpeculativeRequestInternal(int64_t requestId, const std::wstring& snapshot,
                                                                    const TransformationIdentity& identity,
                                                                    std::shared_ptr<TranslationPipeline> pipeline,
                                                                    std::shared_ptr<IDispatcher> dispatcher) {
    if (!pipeline || !enabled_) {
        return;
    }

    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    activeCancelled_ = cancelled;

    TranslationRequest req;
    req.text = snapshot;
    req.operation = identity.operation;
    req.providerId = identity.providerId;
    req.model = identity.model;
    req.sourceLanguage = identity.sourceLanguage;
    req.targetLanguage = identity.targetLanguage;
    req.profile = identity.profile;
    req.customInstructions = identity.customInstructions;
    if (identity.reasoningEffort.has_value()) {
        req.reasoningEffort = identity.reasoningEffort;
    }
    req.useCache = false; // Speculative requests maintain their own isolated candidate cache
    req.requestId = requestId;
    req.commitAuthority = CommitAuthority::Forbidden;
    req.appendTrailingNewline = false;
    req.cancellationToken = cancelled;

    const auto tStart = std::chrono::steady_clock::now();
    lastSpeculativeStartTime_ = tStart;
    auto alive = alive_;

    const int64_t submittedId = pipeline->Submit(std::move(req),
        [this, alive, dispatcher, requestId, snapshot, identity, cancelled, tStart](TranslationResult res) {
            if (!alive || !alive->load()) {
                return;
            }
            if (cancelled && cancelled->load()) {
                // Harmless physical cancellation completion: release slot and trigger queued snapshot
                if (dispatcher) {
                    dispatcher->Post([this, alive, requestId, res = std::move(res), snapshot, identity]() mutable {
                        if (!alive || !alive->load()) return;
                        OnSpeculativeResult(requestId, std::move(res), snapshot, identity, 0);
                    });
                } else {
                    OnSpeculativeResult(requestId, std::move(res), snapshot, identity, 0);
                }
                return;
            }
            const auto tEnd = std::chrono::steady_clock::now();
            const int64_t latMs = std::chrono::duration_cast<std::chrono::milliseconds>(tEnd - tStart).count();

            if (dispatcher) {
                dispatcher->Post([this, alive, requestId, res = std::move(res), snapshot, identity, latMs]() mutable {
                    if (!alive || !alive->load()) {
                        return;
                    }
                    OnSpeculativeResult(requestId, std::move(res), snapshot, identity, latMs);
                });
            } else {
                OnSpeculativeResult(requestId, std::move(res), snapshot, identity, latMs);
            }
        });

    if (submittedId == 0) {
        // Pipeline rejected speculative request
        cancelled->store(true);
        activeCancelled_.reset();
        physicallyActiveRequestId_ = 0;
        logicalActiveRequestId_ = 0;
        Logger::Instance().Debug(cat::Translation, L"[PCHP] speculative-rejected req=#{} reason=pipeline_rejected", requestId);
        return;
    }

    physicallyActiveRequestId_ = submittedId;
    logicalActiveRequestId_ = submittedId;
    activeSnapshot_ = snapshot;
    activeIdentity_ = identity;
    lastRequestedSnapshot_ = snapshot;
    ++telemetry_.requestsStarted;

    Logger::Instance().Debug(cat::Translation,
                             L"[PCHP] speculative-start req=#{} len={}",
                             submittedId, snapshot.size());
}

void SpeculativeTranslationManager::OnSpeculativeResult(int64_t requestId, TranslationResult result,
                                                        const std::wstring& snapshot,
                                                        const TransformationIdentity& identity,
                                                        int64_t totalLatencyMs) {
    std::lock_guard lock(mutex_);
    const bool wasPhysical = (requestId == physicallyActiveRequestId_);
    if (wasPhysical) {
        physicallyActiveRequestId_ = 0;
        activeCancelled_.reset();
    }

    const bool isDirectTestOrActive = (requestId == logicalActiveRequestId_) ||
                                      (logicalActiveRequestId_ == 0 && !wasPhysical);

    if (isDirectTestOrActive && result.ok && !result.text.empty()) {
        logicalActiveRequestId_ = 0;

        SpeculativeTranslationCandidate cand;
        cand.sourceText = snapshot;
        cand.translatedText = std::move(result.text);
        cand.identity = identity;
        cand.speculativeRequestId = requestId;
        cand.completionTimestamp = std::chrono::steady_clock::now();
        cand.totalLatencyMs = totalLatencyMs;
        cand.completed = true;
        cand.ok = true;

        candidate_ = std::move(cand);
        ++telemetry_.requestsCompleted;
        telemetry_.lastSpeculativeTotalLatencyMs = totalLatencyMs;

        Logger::Instance().Debug(cat::Translation,
                                 L"[PCHP] candidate-ready req=#{} in {} ms (len={})",
                                 requestId, totalLatencyMs, snapshot.size());
    } else {
        if (requestId == logicalActiveRequestId_) {
            logicalActiveRequestId_ = 0;
        }
        Logger::Instance().Debug(cat::Translation,
                                 L"[PCHP] slot-released req=#{} (cancelled or incomplete)",
                                 requestId);
    }

    // Check if a newer snapshot was queued while waiting for physical slot release
    if (queuedSnapshot_.has_value() && physicallyActiveRequestId_ == 0) {
        auto qs = *queuedSnapshot_;
        queuedSnapshot_.reset();
        Logger::Instance().Debug(cat::Translation,
                                 L"[PCHP] queued-snapshot-started req=#{} len={}",
                                 qs.nextId, qs.text.size());
        auto pipe = qs.pipeline.lock();
        auto disp = qs.dispatcher.lock();
        if (pipe) {
            StartSpeculativeRequestInternal(qs.nextId, qs.text, qs.identity, std::move(pipe), std::move(disp));
        }
    }
}

bool SpeculativeTranslationManager::TryConsumeExactCandidate(const TranslationRequest& finalReq,
                                                             TranslationResult& outResult,
                                                             bool* outNormalized) {
    if (outNormalized) {
        *outNormalized = false;
    }

    std::lock_guard lock(mutex_);
    if (!enabled_ || !candidate_.has_value()) {
        Logger::Instance().Debug(cat::Translation, L"[PCHP] candidate-miss reason=not_available");
        ++telemetry_.candidateMisses;
        ++telemetry_.candidateMissNotReady;
        return false;
    }

    const auto& c = *candidate_;
    if (!c.completed || !c.ok) {
        Logger::Instance().Debug(cat::Translation, L"[PCHP] candidate-miss reason=incomplete");
        ++telemetry_.candidateMisses;
        ++telemetry_.candidateMissNotReady;
        return false;
    }

    // Exact identity comparison across all 8 configuration dimensions
    const bool configMatches = (c.identity.providerId == finalReq.providerId &&
                                c.identity.model == finalReq.model &&
                                c.identity.sourceLanguage == finalReq.sourceLanguage &&
                                c.identity.targetLanguage == finalReq.targetLanguage &&
                                c.identity.operation == finalReq.operation &&
                                c.identity.profile == finalReq.profile &&
                                c.identity.customInstructions == finalReq.customInstructions &&
                                c.identity.reasoningEffort == finalReq.reasoningEffort);

    if (!configMatches) {
        Logger::Instance().Debug(cat::Translation, L"[PCHP] candidate-miss reason=config_mismatch");
        ++telemetry_.candidateMisses;
        ++telemetry_.candidateMissConfigMismatch;
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    const int64_t leadTimeMs = (c.completionTimestamp.time_since_epoch().count() > 0)
        ? std::chrono::duration_cast<std::chrono::milliseconds>(now - c.completionTimestamp).count()
        : 0;

    // 1. Strict Exact Identity Match
    if (c.sourceText == finalReq.text) {
        outResult.ok = true;
        outResult.text = c.translatedText;
        outResult.requestId = finalReq.requestId;
        outResult.detectedSource = c.identity.sourceLanguage;
        outResult.fromCache = false;
        outResult.commitAuthority = CommitAuthority::Allowed;
        outResult.appendTrailingNewline = finalReq.appendTrailingNewline;

        ++telemetry_.candidateHits;
        ++telemetry_.candidateHitsStrict;
        ++telemetry_.finalRequestsAvoided;
        telemetry_.lastCandidateLeadTimeMs = leadTimeMs;

        Logger::Instance().Info(cat::Translation,
                                L"[PCHP] candidate-hit req=#{} lead={}ms normalization=false",
                                finalReq.requestId, leadTimeMs);

        // Cancel any lingering in-flight speculative work physically and consume candidate
        if (activeCancelled_) {
            activeCancelled_->store(true);
            activeCancelled_.reset();
        }
        physicallyActiveRequestId_ = 0;
        logicalActiveRequestId_ = 0;
        queuedSnapshot_.reset();
        candidate_.reset();
        return true;
    }

    // 2. Policy 2: Terminal Punctuation Normalization (Single trailing '.' '!' '?')
    if (finalReq.text.size() > 1 && !c.sourceText.empty()) {
        const wchar_t termCh = finalReq.text.back();
        if (termCh == L'.' || termCh == L'!' || termCh == L'?') {
            const std::wstring_view finalWithoutTerm = std::wstring_view(finalReq.text).substr(0, finalReq.text.size() - 1);

            // Adversarial checks: candidate must not end in punctuation, and no whitespace before punctuation
            const wchar_t candLastCh = c.sourceText.back();
            const bool candEndsWithPunct = (candLastCh == L'.' || candLastCh == L'!' || candLastCh == L'?');
            const bool candEndsWithSpace = (candLastCh == L' ' || candLastCh == L'\t');

            if (!candEndsWithPunct && !candEndsWithSpace && c.sourceText == finalWithoutTerm) {
                // Valid Policy 2 Terminal Normalization Hit!
                std::wstring adjustedText = c.translatedText;
                if (!adjustedText.empty()) {
                    const wchar_t transLastCh = adjustedText.back();
                    if (transLastCh == L'.' || transLastCh == L'!' || transLastCh == L'?') {
                        adjustedText.back() = termCh;
                    } else {
                        adjustedText.push_back(termCh);
                    }
                } else {
                    adjustedText.push_back(termCh);
                }

                outResult.ok = true;
                outResult.text = std::move(adjustedText);
                outResult.requestId = finalReq.requestId;
                outResult.detectedSource = c.identity.sourceLanguage;
                outResult.fromCache = false;
                outResult.commitAuthority = CommitAuthority::Allowed;
                outResult.appendTrailingNewline = finalReq.appendTrailingNewline;

                if (outNormalized) {
                    *outNormalized = true;
                }

                ++telemetry_.candidateHits;
                ++telemetry_.candidateHitsNormalized;
                ++telemetry_.finalRequestsAvoided;
                telemetry_.lastCandidateLeadTimeMs = leadTimeMs;

                Logger::Instance().Info(cat::Translation,
                                        L"[PCHP] candidate-hit req=#{} lead={}ms normalization=true",
                                        finalReq.requestId, leadTimeMs);

                if (activeCancelled_) {
                    activeCancelled_->store(true);
                    activeCancelled_.reset();
                }
                physicallyActiveRequestId_ = 0;
                logicalActiveRequestId_ = 0;
                queuedSnapshot_.reset();
                candidate_.reset();
                return true;
            }
        }
    }

    Logger::Instance().Debug(cat::Translation,
                             L"[PCHP] candidate-miss reason=source_mismatch (cand_len={}, req_len={})",
                             c.sourceText.size(), finalReq.text.size());

    ++telemetry_.candidateMisses;
    ++telemetry_.candidateMissSourceMismatch;
    return false;
}

void SpeculativeTranslationManager::InvalidateCandidate() {
    std::lock_guard lock(mutex_);
    if (candidate_.has_value() || physicallyActiveRequestId_ != 0 || queuedSnapshot_.has_value()) {
        Logger::Instance().Debug(cat::Translation, L"[PCHP] candidate-invalidated");
    }
    candidate_.reset();
    queuedSnapshot_.reset();
    // NOTE: lastRequestedSnapshot_ and lastSpeculativeStartTime_ are PRESERVED!
    if (activeCancelled_) {
        activeCancelled_->store(true);
        Logger::Instance().Debug(cat::Translation, L"[PCHP] speculative-cancel req=#{} physical=true reason=invalidation", physicallyActiveRequestId_);
        activeCancelled_.reset();
    }
    if (physicallyActiveRequestId_ != 0) {
        ++telemetry_.requestsCancelled;
    }
    physicallyActiveRequestId_ = 0;
    logicalActiveRequestId_ = 0;
}

void SpeculativeTranslationManager::ResetSchedulerHistory() {
    std::lock_guard lock(mutex_);
    Logger::Instance().Debug(cat::Translation, L"[PCHP] scheduler-history-reset (baseline len={})", lastRequestedSnapshot_.size());
    lastRequestedSnapshot_.clear();
    lastSpeculativeStartTime_ = std::chrono::steady_clock::time_point{};
}

void SpeculativeTranslationManager::CancelActiveSpeculative() {
    std::lock_guard lock(mutex_);
    queuedSnapshot_.reset();
    if (activeCancelled_) {
        activeCancelled_->store(true);
        Logger::Instance().Debug(cat::Translation, L"[PCHP] speculative-cancel req=#{} physical=true reason=user_resumed", physicallyActiveRequestId_);
        activeCancelled_.reset();
    }
    if (physicallyActiveRequestId_ != 0) {
        ++telemetry_.requestsCancelled;
    }
    physicallyActiveRequestId_ = 0;
    logicalActiveRequestId_ = 0;
}

std::optional<SpeculativeTranslationCandidate> SpeculativeTranslationManager::CurrentCandidate() const {
    std::lock_guard lock(mutex_);
    return candidate_;
}

int64_t SpeculativeTranslationManager::ActiveSpeculativeRequestId() const {
    std::lock_guard lock(mutex_);
    if (queuedSnapshot_.has_value()) {
        return queuedSnapshot_->nextId;
    }
    return (logicalActiveRequestId_ != 0) ? logicalActiveRequestId_ : physicallyActiveRequestId_;
}

std::wstring SpeculativeTranslationManager::LastRequestedSnapshot() const {
    std::lock_guard lock(mutex_);
    return lastRequestedSnapshot_;
}

SpeculativeTelemetry SpeculativeTranslationManager::GetTelemetry() const {
    std::lock_guard lock(mutex_);
    return telemetry_;
}

void SpeculativeTranslationManager::ResetTelemetry() {
    std::lock_guard lock(mutex_);
    telemetry_ = SpeculativeTelemetry{};
}

} // namespace aiie
