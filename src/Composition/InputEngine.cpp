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
    debouncer_ = std::make_shared<Debouncer>([this] {
        // Debouncer thread -> input thread.
        dispatcher_->Post([this] { OnDebounceFired(); });
    });
    debouncer_->Start();
}

InputEngine::~InputEngine() {
    debouncer_->Stop();
}

void InputEngine::ApplyConfig(const AppConfig& config) {
    std::lock_guard lock(mutex_);
    config_ = config;
    enabled_ = config.enabled;
    debouncer_->SetDelay(std::chrono::milliseconds(config.debounceMs));
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

    if (static_cast<int>(buffer_.size()) >= config_.maxBufferChars) {
        // Safety cap: never let the buffer grow unbounded. Commit as-is.
        lastCommitted_ = buffer_;
        buffer_.clear();
        inflightRequestId_ = -1;
        backend_->OnBufferCommitted();
        debouncer_->Disarm();
        UpdateState(EngineState::Idle, L"");
        return;
    }
    debouncer_->Arm();
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
    backend_->OnBufferChanged(buffer_);
    if (buffer_.empty()) {
        debouncer_->Disarm();
        backend_->OnBufferCommitted();
        UpdateState(EngineState::Idle, L"");
    } else {
        debouncer_->Arm();
    }
}

void InputEngine::HandleTerminator(wchar_t c) {
    std::lock_guard lock(mutex_);
    if (!enabled_) {
        return;
    }
    buffer_.push_back(c);
    backend_->OnBufferChanged(buffer_);
    // Immediate request — no debounce wait (if the buffer is large enough).
    debouncer_->Arm();
    if (static_cast<int>(buffer_.size()) >= config_.minChars) {
        debouncer_->FireNow();
    }
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
    backend_->OnBufferCommitted();
    debouncer_->Disarm();
    UpdateState(EngineState::Idle, L"");
}

void InputEngine::SetEnabled(bool enabled) {
    std::lock_guard lock(mutex_);
    if (enabled_ == enabled) {
        return;
    }
    enabled_ = enabled;
    if (!enabled) {
        // Commit current buffer untouched; never lose text.
        if (!buffer_.empty()) {
            lastCommitted_ = buffer_;
            buffer_.clear();
            backend_->OnBufferCommitted();
        }
        inflightRequestId_ = -1;
        debouncer_->Disarm();
        UpdateState(EngineState::Idle, L"");
    }
}

void InputEngine::FlushBuffer() {
    HandleCommitKey();
}

void InputEngine::OnDebounceFired() {
    std::lock_guard lock(mutex_);
    if (!enabled_ || buffer_.empty()) {
        return;
    }
    if (static_cast<int>(buffer_.size()) < config_.minChars) {
        return;
    }
    // Already waiting on an identical snapshot: nothing new to ask.
    if (inflightRequestId_ != -1 && inflightSnapshot_ == buffer_) {
        return;
    }
    SubmitRequest();
}

bool InputEngine::SubmitRequest() {
    if (static_cast<int>(buffer_.size()) < config_.minChars) {
        return false;
    }
    const int64_t id = inflightRequestId_ + 1;
    inflightRequestId_ = id;
    inflightSnapshot_ = buffer_;

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

    UpdateState(EngineState::Translating, L"");

    // The pipeline callback may fire on a provider thread; marshal to the
    // input thread. The request id is checked again inside OnTranslationResult.
    pipeline_->Submit(std::move(req), [this, id](TranslationResult result) {
        // Capture `this` is safe: the engine owns the pipeline and is torn
        // down before the pipeline in the owning process.
        dispatcher_->Post([this, id, result = std::move(result)]() mutable {
            OnTranslationResult(id, std::move(result));
        });
    });
    logger_->Debug(cat::Composition, L"request #{} submitted ({} chars)", id, inflightSnapshot_.size());
    if (logger_->LogRawText()) {
        logger_->Debug(cat::Composition, L"request #{} text: '{}'", id, inflightSnapshot_);
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
    if (!result.ok) {
        // Fallback: keep the typed text exactly as-is. Nothing is deleted.
        inflightRequestId_ = -1;
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

    lastCommitted_ = result.text;
    buffer_.clear();
    inflightRequestId_ = -1;
    debouncer_->Disarm();
    if (logger_->LogRawText()) {
        logger_->Debug(cat::Composition, L"request #{} result text: '{}'", requestId, result.text);
    }

    backend_->CommitReplacement(std::move(result.text), inflightSnapshot_);
    backend_->OnBufferCommitted();
    UpdateState(EngineState::Idle, L"");
    logger_->Info(cat::Composition, L"request #{} committed replacement", requestId);
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