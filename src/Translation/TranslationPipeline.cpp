#include "Translation/TranslationPipeline.h"

#include "Core/Logging.h"
#include "Core/Utf.h"

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
        Logger::Instance().Warn(cat::Translation, L"submit rejected: no provider");
        return 0;
    }

    const int64_t id = nextId_.fetch_add(1);
    request.requestId = id;

    {
        std::lock_guard lock(mutex_);
        if (pending_ >= kMaxPending) {
            Logger::Instance().Warn(cat::Translation, L"submit rejected: too many pending ({}), dropping request",
                                    pending_);
            return 0;
        }
        latestRequested_ = id;
        ++pending_;
    }

    Logger::Instance().Debug(cat::Translation, L"request #{} submitted (text len={})", id, request.text.size());

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
        provider_->Translate(std::move(request),
                             [this, alive, request, callback = std::move(callback)](TranslationResult result) mutable {
            if (alive && alive->load()) {
                Finish(request, result, std::move(callback));
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
    latestRequested_ = 0;
    pending_ = 0;
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

void TranslationPipeline::Finish(TranslationRequest request, TranslationResult result,
                                 TranslationCallback callback) {
    const bool stale = [&] {
        std::lock_guard lock(mutex_);
        --pending_;
        return request.requestId < latestRequested_;
    }();

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