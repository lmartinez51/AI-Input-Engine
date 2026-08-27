#include "Composition/InputEngine.h"

namespace aiie {

namespace {
bool IsSentenceTerminator(wchar_t c) {
    return c == L'.' || c == L'!' || c == L'?';
}
} // namespace

InputEngine::InputEngine(std::shared_ptr<IDispatcher> dispatcher,
                         std::shared_ptr<IInputBackend> backend,
                         std::shared_ptr<TranslationPipeline> pipeline,
                         std::shared_ptr<Logger> logger)
    : dispatcher_(std::move(dispatcher)), backend_(std::move(backend)),
      pipeline_(std::move(pipeline)), logger_(logger ? std::move(logger)
                                                     : std::shared_ptr<Logger>(&Logger::Instance(), [](Logger*) {})) {
    auto alive = alive_;
    auto disp = dispatcher_;
    debouncer_ = std::make_shared<Debouncer>([this, alive, disp] {
        // Debouncer thread -> input thread.
        if (!alive || !alive->load()) {
            return;
        }
        disp->Post([this, alive] {
            if (!alive || !alive->load()) {
                return;
            }
            OnDebounceFired();
        });
    });
    debouncer_->Start();

    speculativeManager_ = std::make_shared<SpeculativeTranslationManager>(
        config_.speculativeTranslationEnabled, config_.speculativeDebounceMs, config_.speculativeMinChars);
}

InputEngine::~InputEngine() {
    if (alive_) {
        alive_->store(false);
    }
    if (debouncer_) {
        debouncer_->Stop();
    }
    if (speculativeDebouncer_) {
        speculativeDebouncer_->Stop();
    }
}

TransformationIdentity InputEngine::CurrentIdentity() const {
    TransformationIdentity id;
    id.providerId = config_.provider;
    id.model = config_.model;
    id.sourceLanguage = config_.sourceLanguage;
    id.targetLanguage = (config_.operation == TransformOperation::CorrectAndRewrite)
                            ? config_.sourceLanguage
                            : config_.targetLanguage;
    id.operation = config_.operation;
    id.profile = config_.profile;
    id.customInstructions = config_.customProfileInstructions;
    if (config_.modelParams.reasoningEffort.has_value()) {
        id.reasoningEffort = config_.modelParams.reasoningEffort;
    }
    return id;
}

void InputEngine::ApplyConfig(const AppConfig& config) {
    std::lock_guard lock(mutex_);
    config_ = config;
    enabled_ = config.enabled;
    debouncer_->SetDelay(std::chrono::milliseconds(config.debounceMs));
    if (speculativeManager_) {
        speculativeManager_->Configure(config.speculativeTranslationEnabled,
                                       config.speculativeDebounceMs,
                                       config.speculativeMinChars);
    }
    if (config_.speculativeTranslationEnabled) {
        const int delayMs = std::max(config_.speculativeDebounceMs, SpeculativeTranslationManager::kPchpHesitationThresholdMs);
        if (!speculativeDebouncer_) {
            auto alive = alive_;
            auto disp = dispatcher_;
            speculativeDebouncer_ = std::make_shared<Debouncer>([this, alive, disp] {
                if (!alive || !alive->load()) {
                    return;
                }
                disp->Post([this, alive] {
                    if (!alive || !alive->load()) {
                        return;
                    }
                    OnSpeculativeDebounceFired();
                });
            });
            speculativeDebouncer_->SetDelay(std::chrono::milliseconds(delayMs));
            speculativeDebouncer_->Start();
        } else {
            speculativeDebouncer_->SetDelay(std::chrono::milliseconds(delayMs));
        }
    } else {
        if (speculativeDebouncer_) {
            speculativeDebouncer_->Stop();
            speculativeDebouncer_.reset();
        }
    }
}

void InputEngine::HandleCharacter(wchar_t c) {
    std::lock_guard lock(mutex_);
    if (!enabled_) {
        return;
    }
    if (buffer_.empty()) {
        backend_->OnBufferStarted();
        UpdateState(EngineState::Composing, L"");
    }
    buffer_.push_back(c);
    backend_->OnBufferChanged(buffer_);

    // Keystroke mutates buffer: any in-flight speculative request is stale, invalidate candidate
    // Scheduler history (lastRequestedSnapshot_) is preserved to enforce growth gate on subsequent pauses
    if (speculativeManager_) {
        speculativeManager_->InvalidateCandidate();
        speculativeManager_->CancelActiveSpeculative();
    }
    if (speculativeDebouncer_) {
        speculativeDebouncer_->Disarm();
    }

    if (static_cast<int>(buffer_.size()) >= config_.maxBufferChars) {
        // Safety cap: never let the buffer grow unbounded. Commit as-is.
        lastCommitted_ = buffer_;
        buffer_.clear();
        inflightRequestId_ = -1;
        backend_->OnBufferCommitted();
        debouncer_->Disarm();
        if (speculativeDebouncer_) {
            speculativeDebouncer_->Disarm();
        }
        if (speculativeManager_) {
            speculativeManager_->InvalidateCandidate();
            speculativeManager_->ResetSchedulerHistory();
        }
        UpdateState(EngineState::Idle, L"");
        return;
    }

    if (IsTerminalPunctuation(c)) {
        if (speculativeDebouncer_) {
            speculativeDebouncer_->Disarm();
        }
        const int effectiveMin = std::max(config_.minChars, kMinMeaningfulChars);
        if (static_cast<int>(buffer_.size()) >= effectiveMin) {
            if (inflightRequestId_ != -1 && inflightSnapshot_ == buffer_ && inflightCommitAuthority_ == CommitAuthority::Allowed) {
                logger_->Debug(cat::Composition, L"[TRIGGER] duplicate-snapshot-skipped (len={})", buffer_.size());
                return;
            }
            logger_->Info(cat::Composition, L"[TRIGGER] terminal-fast-path (char='{}', len={})", c, buffer_.size());
            debouncer_->Disarm();
            SubmitRequest(CommitAuthority::Allowed, true /* isTerminalFastPath */, false /* appendTrailingNewline */);
        } else {
            logger_->Debug(cat::Composition, L"[TRIGGER] ignored lone/short terminator (len={})", buffer_.size());
            debouncer_->Disarm();
        }
        return;
    }

    // Adaptive debounce selection for non-terminal characters
    int delayMs = kMidWordDebounceMs;
    if (IsClausePunctuation(c)) {
        delayMs = kClauseDebounceMs;
    } else if (c == L' ' || c == L'\t') {
        delayMs = kWordBoundaryDebounceMs;
    } else {
        delayMs = kMidWordDebounceMs;
    }
    debouncer_->SetDelay(std::chrono::milliseconds(delayMs));
    debouncer_->Arm();

    // Rearm PCHP hesitation debouncer: speculation arms only upon >= 450ms inactivity
    if (config_.speculativeTranslationEnabled && speculativeManager_ && speculativeDebouncer_) {
        speculativeDebouncer_->SetDelay(std::chrono::milliseconds(SpeculativeTranslationManager::kPchpHesitationThresholdMs));
        speculativeDebouncer_->Arm();
    }
}

void InputEngine::HandleBackspace() {
    std::lock_guard lock(mutex_);
    if (!enabled_ || buffer_.empty()) {
        return;
    }
    // Remove one full code point (handles surrogate pairs).
    const size_t len = buffer_.size();
    size_t remove = 1;
    if (len >= 2) {
        const wchar_t last = buffer_[len - 1];
        if (last >= 0xDC00 && last <= 0xDFFF && buffer_[len - 2] >= 0xD800 && buffer_[len - 2] <= 0xDBFF) {
            remove = 2;
        }
    }
    buffer_.erase(buffer_.size() - remove);
    // Buffer changed: any in-flight result is now stale.
    inflightRequestId_ = -1;
    inflightCommitAuthority_ = CommitAuthority::Forbidden;
    inflightAppendTrailingNewline_ = false;
    backend_->OnBufferChanged(buffer_);
    if (speculativeManager_) {
        speculativeManager_->InvalidateCandidate();
        speculativeManager_->CancelActiveSpeculative();
    }
    if (speculativeDebouncer_) {
        speculativeDebouncer_->Disarm();
    }
    if (buffer_.empty()) {
        debouncer_->Disarm();
        if (speculativeManager_) {
            speculativeManager_->ResetSchedulerHistory();
        }
        backend_->OnBufferCommitted();
        UpdateState(EngineState::Idle, L"");
    } else {
        const wchar_t tail = buffer_.back();
        if (IsTerminalPunctuation(tail)) {
            debouncer_->Disarm();
        } else {
            int delayMs = kMidWordDebounceMs;
            if (IsClausePunctuation(tail)) {
                delayMs = kClauseDebounceMs;
            } else if (tail == L' ' || tail == L'\t') {
                delayMs = kWordBoundaryDebounceMs;
            }
            debouncer_->SetDelay(std::chrono::milliseconds(delayMs));
            debouncer_->Arm();

            if (config_.speculativeTranslationEnabled && speculativeManager_ && speculativeDebouncer_) {
                if (static_cast<int>(buffer_.size()) >= SpeculativeTranslationManager::kPchpMinBufferLength) {
                    const int specDelayMs = std::max(config_.speculativeDebounceMs, SpeculativeTranslationManager::kPchpHesitationThresholdMs);
                    speculativeDebouncer_->SetDelay(std::chrono::milliseconds(specDelayMs));
                    speculativeDebouncer_->Arm();
                }
            }
        }
    }
}

void InputEngine::HandleTerminator(wchar_t c) {
    std::lock_guard lock(mutex_);
    if (!enabled_) {
        return;
    }
    if (buffer_.empty()) {
        backend_->OnBufferStarted();
        UpdateState(EngineState::Composing, L"");
    }
    buffer_.push_back(c);
    backend_->OnBufferChanged(buffer_);
    if (speculativeDebouncer_) {
        speculativeDebouncer_->Disarm();
    }

    const int effectiveMin = std::max(config_.minChars, kMinMeaningfulChars);
    if (static_cast<int>(buffer_.size()) >= effectiveMin) {
        if (inflightRequestId_ != -1 && inflightSnapshot_ == buffer_ && inflightCommitAuthority_ == CommitAuthority::Allowed) {
            logger_->Debug(cat::Composition, L"[TRIGGER] duplicate-snapshot-skipped (len={})", buffer_.size());
            return;
        }
        logger_->Info(cat::Composition, L"[TRIGGER] terminal-fast-path (char='{}', len={})", c, buffer_.size());
        debouncer_->Disarm();
        SubmitRequest(CommitAuthority::Allowed, true /* isTerminalFastPath */, false /* appendTrailingNewline */);
    } else {
        logger_->Debug(cat::Composition, L"[TRIGGER] ignored lone/short terminator (len={})", buffer_.size());
        debouncer_->Disarm();
    }
}

bool InputEngine::CanHandleShiftEnter() const {
    std::lock_guard lock(mutex_);
    const int effectiveMin = std::max(config_.minChars, kMinMeaningfulChars);
    return enabled_ && static_cast<int>(buffer_.size()) >= effectiveMin;
}

void InputEngine::HandleShiftEnter() {
    std::lock_guard lock(mutex_);
    if (!enabled_) {
        return;
    }
    if (speculativeDebouncer_) {
        speculativeDebouncer_->Disarm();
    }
    if (buffer_.empty()) {
        logger_->Debug(cat::Composition, L"[TRIGGER] shift+enter on empty buffer - transparent passthrough");
        return;
    }

    const int effectiveMin = std::max(config_.minChars, kMinMeaningfulChars);
    if (static_cast<int>(buffer_.size()) < effectiveMin) {
        logger_->Debug(cat::Composition, L"[TRIGGER] shift+enter buffer below minChars (len={}) - commit as-is + newline",
                       buffer_.size());
        lastCommitted_ = buffer_;
        lastCommitted_.push_back(L'\n');
        buffer_.clear();
        inflightRequestId_ = -1;
        inflightCommitAuthority_ = CommitAuthority::Forbidden;
        inflightAppendTrailingNewline_ = false;
        debouncer_->Disarm();
        if (speculativeManager_) {
            speculativeManager_->InvalidateCandidate();
            speculativeManager_->ResetSchedulerHistory();
        }
        backend_->CommitReplacement(lastCommitted_, lastCommitted_);
        backend_->OnBufferCommitted();
        UpdateState(EngineState::Idle, L"");
        return;
    }

    logger_->Info(cat::Composition, L"[TRIGGER] shift+enter intercepted (len={}) - translating + trailing newline",
                  buffer_.size());
    debouncer_->Disarm();
    SubmitRequest(CommitAuthority::Allowed, false /* isTerminalFastPath */, true /* appendTrailingNewline */);
}

void InputEngine::HandleCommitKey() {
    std::lock_guard lock(mutex_);
    if (!enabled_ || buffer_.empty()) {
        return;
    }
    // Commit whatever was typed untouched; translation already happened
    // incrementally (or will never happen for this text).
    lastCommitted_ = buffer_;
    buffer_.clear();
    inflightRequestId_ = -1;
    inflightCommitAuthority_ = CommitAuthority::Forbidden;
    inflightAppendTrailingNewline_ = false;
    backend_->OnBufferCommitted();
    debouncer_->Disarm();
    if (speculativeDebouncer_) {
        speculativeDebouncer_->Disarm();
    }
    if (speculativeManager_) {
        speculativeManager_->InvalidateCandidate();
        speculativeManager_->ResetSchedulerHistory();
    }
    UpdateState(EngineState::Idle, L"");
}

void InputEngine::SetEnabled(bool enabled) {
    std::lock_guard lock(mutex_);
    if (enabled_ == enabled) {
        return;
    }
    enabled_ = enabled;
    if (!enabled_) {
        buffer_.clear();
        inflightRequestId_ = -1;
        inflightCommitAuthority_ = CommitAuthority::Forbidden;
        inflightAppendTrailingNewline_ = false;
        debouncer_->Disarm();
        if (speculativeDebouncer_) {
            speculativeDebouncer_->Disarm();
        }
        if (speculativeManager_) {
            speculativeManager_->InvalidateCandidate();
            speculativeManager_->ResetSchedulerHistory();
        }
        UpdateState(EngineState::Idle, L"");
    }
}

void InputEngine::FlushBuffer() {
    std::lock_guard lock(mutex_);
    if (!enabled_ || buffer_.empty()) {
        return;
    }
    lastCommitted_ = buffer_;
    buffer_.clear();
    inflightRequestId_ = -1;
    inflightCommitAuthority_ = CommitAuthority::Forbidden;
    inflightAppendTrailingNewline_ = false;
    debouncer_->Disarm();
    if (speculativeDebouncer_) {
        speculativeDebouncer_->Disarm();
    }
    if (speculativeManager_) {
        speculativeManager_->InvalidateCandidate();
        speculativeManager_->ResetSchedulerHistory();
    }
    backend_->OnBufferCommitted();
    UpdateState(EngineState::Idle, L"");
}

void InputEngine::CancelActiveCompositionForFocusChange() {
    std::lock_guard lock(mutex_);
    if (inflightRequestId_ != -1 || !buffer_.empty()) {
        logger_->Debug(cat::Composition, L"focus changed: cancelling active composition (inflight #{}, buffer len={})",
                       inflightRequestId_, buffer_.size());
        inflightRequestId_ = -1;
        inflightCommitAuthority_ = CommitAuthority::Forbidden;
        inflightAppendTrailingNewline_ = false;
        buffer_.clear();
        debouncer_->Disarm();
        if (speculativeDebouncer_) {
            speculativeDebouncer_->Disarm();
        }
        if (speculativeManager_) {
            speculativeManager_->InvalidateCandidate();
            speculativeManager_->CancelActiveSpeculative();
            speculativeManager_->ResetSchedulerHistory();
        }
        backend_->OnBufferCommitted();
        UpdateState(EngineState::Idle, L"");
    }
}

void InputEngine::OnDebounceFired() {
    std::lock_guard lock(mutex_);
    if (!enabled_ || buffer_.empty()) {
        return;
    }
    // Weak signal policy:
    // With speculativeTranslationEnabled = false, weak signals produce ZERO provider calls.
    // With speculativeTranslationEnabled = true, speculative triggering is governed strictly by the PCHP hesitation debouncer.
    logger_->Debug(cat::Composition, L"[TRIGGER] weak-signal-idle: composing kept (len={})", buffer_.size());
}

void InputEngine::OnSpeculativeDebounceFired() {
    std::lock_guard lock(mutex_);
    if (!enabled_ || !config_.speculativeTranslationEnabled || buffer_.empty() || inflightRequestId_ != -1) {
        return;
    }
    const int effectiveMin = std::max(config_.speculativeMinChars, SpeculativeTranslationManager::kPchpMinBufferLength);
    if (static_cast<int>(buffer_.size()) < effectiveMin) {
        logger_->Debug(cat::Composition, L"[PCHP] gate-reject reason=min-buffer len={} min={}",
                       buffer_.size(), effectiveMin);
        return;
    }
    const auto identity = CurrentIdentity();
    if (speculativeManager_ && speculativeManager_->ShouldTriggerSpeculative(buffer_, identity)) {
        const int64_t id = nextRequestId_++;
        logger_->Debug(cat::Composition, L"[PCHP] hesitation-window-open len={} idle={}ms",
                       buffer_.size(), SpeculativeTranslationManager::kPchpHesitationThresholdMs);
        speculativeManager_->StartSpeculativeRequest(id, buffer_, identity, pipeline_, dispatcher_);
    }
}

bool InputEngine::SubmitRequest(CommitAuthority authority, bool isTerminalFastPath, bool appendTrailingNewline) {
    const int effectiveMin = std::max(config_.minChars, kMinMeaningfulChars);
    if (static_cast<int>(buffer_.size()) < effectiveMin) {
        return false;
    }
    if (inflightRequestId_ != -1 && inflightSnapshot_ == buffer_ &&
        inflightCommitAuthority_ == authority && inflightAppendTrailingNewline_ == appendTrailingNewline) {
        return false;
    }

    const int64_t id = nextRequestId_++;
    const std::wstring currentSnapshot = buffer_;

    TranslationRequest req;
    req.text = buffer_;
    req.operation = config_.operation;
    req.providerId = config_.provider;
    req.model = config_.model;
    req.sourceLanguage = config_.sourceLanguage;
    req.targetLanguage = (config_.operation == TransformOperation::CorrectAndRewrite)
                             ? config_.sourceLanguage
                             : config_.targetLanguage;
    req.profile = config_.profile;
    req.customInstructions = config_.customProfileInstructions;
    req.mode = config_.mode;
    req.context = lastCommitted_;
    req.useCache = config_.cacheEnabled;
    req.requestId = id;
    req.commitAuthority = authority;
    req.appendTrailingNewline = appendTrailingNewline;
    if (config_.modelParams.reasoningEffort.has_value()) {
        req.reasoningEffort = config_.modelParams.reasoningEffort;
    }

    // Check if a completed exact candidate can be reused (only for Allowed commit requests)
    if (authority == CommitAuthority::Allowed && config_.speculativeTranslationEnabled && speculativeManager_) {
        TranslationResult candidateRes;
        bool normalized = false;
        if (speculativeManager_->TryConsumeExactCandidate(req, candidateRes, &normalized)) {
            candidateRes.commitAuthority = CommitAuthority::Allowed;
            candidateRes.appendTrailingNewline = appendTrailingNewline;
            lastSubmitTime_ = std::chrono::steady_clock::now();
            inflightRequestId_ = id;
            inflightCommitAuthority_ = CommitAuthority::Allowed;
            inflightAppendTrailingNewline_ = appendTrailingNewline;
            inflightSnapshot_ = currentSnapshot;
            UpdateState(EngineState::Translating, L"");
            logger_->Info(cat::Composition, L"[PCHP] candidate-hit req=#{} ('{}' -> '{}') normalization={}",
                          id, req.text, candidateRes.text, normalized);
            auto alive = alive_;
            auto dispatcher = dispatcher_;
            if (dispatcher) {
                dispatcher->Post([this, alive, id, res = std::move(candidateRes)]() mutable {
                    if (!alive || !alive->load()) {
                        return;
                    }
                    OnTranslationResult(id, std::move(res));
                });
            } else {
                OnTranslationResult(id, std::move(candidateRes));
            }
            return true;
        } else {
            logger_->Debug(cat::Composition, L"[PCHP] candidate-miss for req=#{} (len={}), issuing final request",
                           id, req.text.size());
            speculativeManager_->CancelActiveSpeculative();
        }
    }

    // If submitting a fresh Allowed request to pipeline, cancel any pending speculative request
    if (authority == CommitAuthority::Allowed && speculativeManager_) {
        speculativeManager_->CancelActiveSpeculative();
    }

    // Submit to pipeline. The pipeline callback may fire on a provider thread; marshal to the input thread.
    auto alive = alive_;
    auto dispatcher = dispatcher_;
    const int64_t submittedId = pipeline_->Submit(std::move(req), [this, alive, dispatcher, id](TranslationResult result) {
        if (!alive || !alive->load()) {
            return;
        }
        dispatcher->Post([this, alive, id, result = std::move(result)]() mutable {
            if (!alive || !alive->load()) {
                return;
            }
            OnTranslationResult(id, std::move(result));
        });
    });

    if (submittedId == 0) {
        // Pipeline rejected submission (e.g. provider unavailable or capacity exhausted)
        logger_->Warn(cat::Composition,
                      L"[TRANSLATION] request #{} rejected by pipeline (authority={}, buffer len={}) - preserving composition",
                      id, authority == CommitAuthority::Allowed ? L"Allowed" : L"Forbidden", currentSnapshot.size());
        inflightRequestId_ = -1;
        inflightCommitAuthority_ = CommitAuthority::Forbidden;
        inflightAppendTrailingNewline_ = false;
        UpdateState(buffer_.empty() ? EngineState::Idle : EngineState::Composing, L"");
        return false;
    }

    // Submission accepted by pipeline: update engine in-flight state
    lastSubmitTime_ = std::chrono::steady_clock::now();
    inflightRequestId_ = submittedId;
    inflightCommitAuthority_ = authority;
    inflightAppendTrailingNewline_ = appendTrailingNewline;
    inflightSnapshot_ = currentSnapshot;
    UpdateState(EngineState::Translating, L"");
    logger_->Debug(cat::Composition, L"[TRANSLATION] final request accepted req=#{} ({} chars, fastPath={}, authority={}, newline={})",
                   submittedId, inflightSnapshot_.size(), isTerminalFastPath,
                   authority == CommitAuthority::Allowed ? L"Allowed" : L"Forbidden",
                   appendTrailingNewline);
    if (logger_->LogRawText()) {
        logger_->Debug(cat::Composition, L"request #{} text: '{}'", submittedId, inflightSnapshot_);
    }
    return true;
}

void InputEngine::OnTranslationResult(int64_t requestId, TranslationResult result) {
    std::lock_guard lock(mutex_);
    if (requestId != inflightRequestId_) {
        logger_->Debug(cat::Composition, L"result #{} ignored (inflight #{}, buffer changed)", requestId,
                       inflightRequestId_);
        return;
    }
    // Stale or cancelled results are normal lifecycle events (e.g. superseded by newer typing)
    // and must NEVER mutate composition state, set Error, or show failure status.
    if (result.error == L"stale" || result.error == L"cancelled") {
        logger_->Debug(cat::Composition, L"result #{} ({}) safely discarded (inflight #{})",
                       requestId, result.error, inflightRequestId_);
        return;
    }
    if (!result.ok) {
        // Fallback for genuine provider errors: keep the typed text exactly as-is. Nothing is deleted.
        inflightRequestId_ = -1;
        inflightCommitAuthority_ = CommitAuthority::Forbidden;
        inflightAppendTrailingNewline_ = false;
        UpdateState(EngineState::Error, result.error);
        logger_->Warn(cat::Composition, L"translation failed for request #{}: {} — keeping user text",
                      requestId, result.errorDetail.empty() ? result.error : result.errorDetail);
        backend_->ShowStatus(L"AI Input: translation failed, text kept");
        return;
    }
    // Version guard: the buffer must still match what was sent.
    if (buffer_ != inflightSnapshot_) {
        logger_->Debug(cat::Composition, L"result #{} stale (buffer changed), discarding", requestId);
        inflightRequestId_ = -1;
        inflightCommitAuthority_ = CommitAuthority::Forbidden;
        inflightAppendTrailingNewline_ = false;
        return;
    }

    // CRITICAL AUTHORITY GUARD:
    // A Forbidden result MUST NEVER mutate visible text, clear buffer_, or call OnBufferCommitted.
    if (result.commitAuthority != CommitAuthority::Allowed) {
        logger_->Debug(cat::Composition, L"result #{} (authority=Forbidden) kept in background, visible text untouched",
                       requestId);
        return;
    }

    // Preserve leading whitespace from source text (e.g. user typed space before sentence)
    size_t sourceLeadingWs = 0;
    while (sourceLeadingWs < inflightSnapshot_.size() &&
           (inflightSnapshot_[sourceLeadingWs] == L' ' || inflightSnapshot_[sourceLeadingWs] == L'\t')) {
        ++sourceLeadingWs;
    }
    if (sourceLeadingWs > 0) {
        size_t resultLeadingWs = 0;
        while (resultLeadingWs < result.text.size() &&
               (result.text[resultLeadingWs] == L' ' || result.text[resultLeadingWs] == L'\t')) {
            ++resultLeadingWs;
        }
        if (resultLeadingWs < sourceLeadingWs) {
            result.text.insert(0, inflightSnapshot_.substr(0, sourceLeadingWs - resultLeadingWs));
        }
    }

    // Preserve trailing whitespace from source text
    size_t sourceTrailingWs = 0;
    while (sourceTrailingWs < inflightSnapshot_.size() &&
           (inflightSnapshot_[inflightSnapshot_.size() - 1 - sourceTrailingWs] == L' ' ||
            inflightSnapshot_[inflightSnapshot_.size() - 1 - sourceTrailingWs] == L'\t')) {
        ++sourceTrailingWs;
    }
    if (sourceTrailingWs > 0) {
        size_t resultTrailingWs = 0;
        while (resultTrailingWs < result.text.size() &&
               (result.text[result.text.size() - 1 - resultTrailingWs] == L' ' ||
                result.text[result.text.size() - 1 - resultTrailingWs] == L'\t')) {
            ++resultTrailingWs;
        }
        if (resultTrailingWs < sourceTrailingWs) {
            result.text.append(inflightSnapshot_.substr(inflightSnapshot_.size() - (sourceTrailingWs - resultTrailingWs)));
        }
    }

    // If Shift+Enter or trailing newline requested, append exactly one newline
    if (result.appendTrailingNewline) {
        if (result.text.empty() || result.text.back() != L'\n') {
            result.text.push_back(L'\n');
        }
    }

    lastCommitted_ = result.text;
    buffer_.clear();
    inflightRequestId_ = -1;
    inflightCommitAuthority_ = CommitAuthority::Forbidden;
    inflightAppendTrailingNewline_ = false;
    debouncer_->Disarm();
    if (logger_->LogRawText()) {
        logger_->Debug(cat::Composition, L"request #{} result text: '{}'", requestId, result.text);
    }

    backend_->CommitReplacement(std::move(result.text), inflightSnapshot_);
    backend_->OnBufferCommitted();
    if (speculativeManager_) {
        speculativeManager_->ResetSchedulerHistory();
    }
    UpdateState(EngineState::Idle, L"");
    logger_->Info(cat::Composition, L"request #{} committed replacement (len={}, trailingNewline={})",
                  requestId, inflightSnapshot_.size(), result.appendTrailingNewline);
}

EngineStatus InputEngine::Status() const {
    std::lock_guard lock(mutex_);
    EngineStatus s;
    s.enabled = enabled_;
    s.bufferLength = buffer_.size();
    s.state = state_;
    s.lastMessage = lastMessage_;
    return s;
}

std::wstring InputEngine::Buffer() const {
    std::lock_guard lock(mutex_);
    return buffer_;
}

void InputEngine::ApplyReplacement(std::wstring replacement, size_t sourceLength) {
    // Reserved for future backends that need explicit text application.
    (void)replacement;
    (void)sourceLength;
}

void InputEngine::UpdateState(EngineState state, std::wstring_view message) {
    state_ = state;
    lastMessage_.assign(message);
    if (state == EngineState::Translating) {
        backend_->ShowStatus(L"Translating...");
    } else {
        backend_->HideStatus();
    }
}

} // namespace aiie