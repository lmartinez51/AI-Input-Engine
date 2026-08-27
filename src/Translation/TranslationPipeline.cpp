#include "Translation/TranslationPipeline.h"

#include "Core/Logging.h"
#include "Core/Utf.h"

#include <algorithm>

namespace aiie {

TranslationPipeline::TranslationPipeline(std::shared_ptr<ITranslationProvider> provider,
                                         std::shared_ptr<TranslationCache> cache,
                                         int requestTimeoutMs)
    : provider_(std::move(provider)), cache_(std::move(cache)), requestTimeoutMs_(requestTimeoutMs) {}

TranslationPipeline::~TranslationPipeline() {
    if (alive_) {
        *alive_ = false;
    }
    CancelAll();
}

void TranslationPipeline::SetProvider(std::shared_ptr<ITranslationProvider> provider) {
    {
        std::lock_guard lock(mutex_);
        if (provider_ && provider_ != provider) {
            provider_->CancelAll();
        }
        provider_ = std::move(provider);
    }
    Logger::Instance().Info(cat::Translation, L"pipeline provider set to {}",
                            provider_ ? provider_->Name() : std::wstring_view(L"<none>"));
}

int64_t TranslationPipeline::Submit(TranslationRequest request, TranslationCallback callback) {
    if (!provider_) {
        Logger::Instance().Warn(cat::Translation, L"[PIPELINE] rejected: no provider");
        return 0;
    }

    const int64_t id = (request.requestId > 0) ? request.requestId : nextId_.fetch_add(1);
    request.requestId = id;
    if (!request.cancellationToken) {
        request.cancellationToken = std::make_shared<std::atomic<bool>>(false);
    }

    {
        std::lock_guard lock(mutex_);
        if (request.commitAuthority == CommitAuthority::Allowed) {
            // Preempt any existing speculative work immediately
            for (auto& slot : inflightSlots_) {
                if (slot.authority == CommitAuthority::Forbidden) {
                    if (slot.cancellationToken) {
                        slot.cancellationToken->store(true);
                        Logger::Instance().Debug(cat::Translation, L"[PIPELINE] preempted speculative req=#{} for Allowed req=#{}",
                                                 slot.id, id);
                    }
                }
            }

            if (pendingAllowed_ >= kMaxAllowedPending) {
                Logger::Instance().Warn(cat::Translation,
                    L"[PIPELINE] rejected req=#{} reason=too_many_pending_allowed ({})",
                    id, pendingAllowed_);
                return 0;
            }

            latestRequested_ = id;
            ++pendingAllowed_;
            inflightSlots_.push_back({id, CommitAuthority::Allowed, request.cancellationToken});
            Logger::Instance().Debug(cat::Translation,
                L"[PIPELINE] accepted req=#{} authority=Allowed (pendingAllowed={}, pendingSpeculative={})",
                id, pendingAllowed_, pendingSpeculative_);
        } else {
            // Speculative / Forbidden request
            if (pendingSpeculative_ >= kMaxSpeculativePending) {
                // Signal physical cancellation on older speculative slots
                for (auto& slot : inflightSlots_) {
                    if (slot.authority == CommitAuthority::Forbidden) {
                        if (slot.cancellationToken) {
                            slot.cancellationToken->store(true);
                            Logger::Instance().Debug(cat::Translation, L"[PIPELINE] cancelling older speculative req=#{}", slot.id);
                        }
                    }
                }
                Logger::Instance().Debug(cat::Translation,
                    L"[PIPELINE] rejected req=#{} reason=speculative_slot_occupied ({})",
                    id, pendingSpeculative_);
                return 0;
            }

            if (pendingAllowed_ >= kMaxAllowedPending) {
                Logger::Instance().Debug(cat::Translation,
                    L"[PIPELINE] rejected req=#{} reason=allowed_capacity_full ({})",
                    id, pendingAllowed_);
                return 0;
            }

            ++pendingSpeculative_;
            inflightSlots_.push_back({id, CommitAuthority::Forbidden, request.cancellationToken});
            Logger::Instance().Debug(cat::Translation,
                L"[PIPELINE] accepted req=#{} authority=Forbidden (pendingAllowed={}, pendingSpeculative={})",
                id, pendingAllowed_, pendingSpeculative_);
        }
    }

    if (request.useCache && cache_) {
        if (const std::wstring* cached = cache_->Lookup(request)) {
            TranslationResult result;
            result.text = *cached;
            result.ok = true;
            result.requestId = id;
            result.fromCache = true;
            Finish(request, result, std::move(callback));
            return id;
        }
    }

    auto alive = alive_;
    try {
        TranslationRequest reqCopy = request;
        provider_->Translate(std::move(request),
                             [this, alive, reqCopy = std::move(reqCopy), callback = std::move(callback)](TranslationResult result) mutable {
            if (alive && alive->load()) {
                Finish(reqCopy, result, std::move(callback));
            } else if (callback) {
                callback(std::move(result));
            }
        });
    } catch (const std::exception& e) {
        TranslationResult result;
        result.ok = false;
        result.error = L"provider threw";
        result.errorDetail = utf::FromUtf8(e.what());
        Finish(request, result, std::move(callback));
    }
    return id;
}

void TranslationPipeline::CancelAll() {
    std::lock_guard lock(mutex_);
    for (auto& slot : inflightSlots_) {
        if (slot.cancellationToken) {
            slot.cancellationToken->store(true);
        }
    }
    inflightSlots_.clear();
    pendingAllowed_ = 0;
    pendingSpeculative_ = 0;
    latestRequested_ = 0;
    if (provider_) {
        provider_->CancelAll();
    }
}

ProviderStatus TranslationPipeline::Status() const {
    if (!provider_) {
        return {};
    }
    return provider_->Status();
}

size_t TranslationPipeline::PendingAllowed() const {
    std::lock_guard lock(mutex_);
    return pendingAllowed_;
}

size_t TranslationPipeline::PendingSpeculative() const {
    std::lock_guard lock(mutex_);
    return pendingSpeculative_;
}

size_t TranslationPipeline::TotalPending() const {
    std::lock_guard lock(mutex_);
    return pendingAllowed_ + pendingSpeculative_;
}

void TranslationPipeline::Finish(TranslationRequest request, TranslationResult result,
                                 TranslationCallback callback) {
    result.commitAuthority = request.commitAuthority;
    result.appendTrailingNewline = request.appendTrailingNewline;

    bool stale = false;
    {
        std::lock_guard lock(mutex_);
        auto it = std::find_if(inflightSlots_.begin(), inflightSlots_.end(),
                               [&](const InflightSlot& s) { return s.id == request.requestId; });
        if (it != inflightSlots_.end()) {
            if (it->authority == CommitAuthority::Allowed) {
                if (pendingAllowed_ > 0) {
                    --pendingAllowed_;
                }
            } else {
                if (pendingSpeculative_ > 0) {
                    --pendingSpeculative_;
                }
            }
            inflightSlots_.erase(it);
            Logger::Instance().Debug(cat::Translation,
                L"[PIPELINE] slot-released req=#{} authority={} (remaining: allowed={}, speculative={})",
                request.requestId,
                request.commitAuthority == CommitAuthority::Allowed ? L"Allowed" : L"Forbidden",
                pendingAllowed_, pendingSpeculative_);
        }
        if (request.commitAuthority == CommitAuthority::Allowed) {
            stale = (request.requestId < latestRequested_);
        }
    }

    if (stale) {
        Logger::Instance().Debug(cat::Translation, L"request #{} stale, dropping (latest=#{}), text len={}",
                                 request.requestId, latestRequested_, result.text.size());
        result.ok = false;
        result.error = L"stale";
    }

    if (result.ok && !result.text.empty() && cache_ && !result.fromCache) {
        cache_->Store(request, result.text);
    }

    if (callback) {
        callback(std::move(result));
    }
}

} // namespace aiie