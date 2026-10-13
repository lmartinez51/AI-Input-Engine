#include "mini_test.h"

#include "Core/Dispatcher.h"
#include "Providers/MockProvider.h"
#include "Translation/TranslationCache.h"
#include "Translation/TranslationPipeline.h"

#include <atomic>
#include <thread>

using namespace aiie;

namespace {
class FakeProvider : public ITranslationProvider {
public:
    explicit FakeProvider(std::chrono::milliseconds delay) : delay_(delay) {}

    std::wstring_view Name() const override { return L"fake"; }
    std::wstring_view DisplayName() const override { return L"Fake"; }
    bool RequiresNetwork() const override { return false; }
    ProviderStatus Status() const override { return {ProviderStatus::State::Ready, L""}; }
    void CancelAll() override { cancelled_->store(true); }

    void Translate(TranslationRequest request, TranslationCallback callback) override {
        {
            std::lock_guard lock(mutex_);
            ++requests_;
            lastText_ = request.text;
            lastId_ = request.requestId;
        }
        // Workers capture only the shared cancellation flag, never `this`: a
        // worker may still be sleeping when the provider is destroyed.
        const auto cancelled = cancelled_;
        const auto delay = delay_;
        std::thread([cancelled, delay, request = std::move(request),
                     callback = std::move(callback)]() mutable {
            std::this_thread::sleep_for(delay);
            TranslationResult r;
            r.requestId = request.requestId;
            if (cancelled->load()) {
                r.ok = false;
                r.error = L"cancelled";
            } else {
                r.ok = true;
                r.text = L"[" + request.text + L"]";
            }
            callback(std::move(r));
        }).detach();
    }

    std::chrono::milliseconds delay_;
    std::shared_ptr<std::atomic<bool>> cancelled_ = std::make_shared<std::atomic<bool>>(false);
    std::mutex mutex_;
    int requests_ = 0;
    std::wstring lastText_;
    int64_t lastId_ = 0;
};
} // namespace

TEST(pipeline_assigns_ids_and_delivers) {
    auto provider = std::make_shared<FakeProvider>(std::chrono::milliseconds(10));
    auto cache = std::make_shared<TranslationCache>(100);
    TranslationPipeline pipeline(provider, cache, 30000);

    std::atomic<bool> done{false};
    TranslationResult got;
    TranslationRequest req;
    req.text = L"hola";
    req.sourceLanguage = L"es";
    req.targetLanguage = L"en";
    const int64_t id = pipeline.Submit(req, [&](TranslationResult r) {
        got = std::move(r);
        done.store(true);
    });
    CHECK_GT(id, 0);
    for (int i = 0; i < 200 && !done.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(done.load());
    CHECK(got.ok);
    CHECK_EQ(got.requestId, id);
    CHECK_STREQ(got.text, L"[hola]");
}

TEST(pipeline_latest_wins_discards_stale) {
    auto provider = std::make_shared<FakeProvider>(std::chrono::milliseconds(40));
    auto cache = std::make_shared<TranslationCache>(100);
    TranslationPipeline pipeline(provider, cache, 30000);

    std::atomic<int> staleDelivered{0};
    std::atomic<bool> latestDone{false};
    std::atomic<bool> bothDone{false};
    TranslationResult latest;
    TranslationRequest a;
    a.text = L"aaa";
    a.sourceLanguage = L"es";
    a.targetLanguage = L"en";
    a.commitAuthority = CommitAuthority::Allowed;
    TranslationRequest b;
    b.text = L"bbbb";
    b.sourceLanguage = L"es";
    b.targetLanguage = L"en";
    b.commitAuthority = CommitAuthority::Allowed;

    const int64_t idA = pipeline.Submit(a, [&](TranslationResult r) {
        if (!r.ok && r.error == L"stale") {
            staleDelivered.fetch_add(1);
        }
        if (staleDelivered.load() > 0 && latestDone.load()) {
            bothDone.store(true);
        }
    });
    const int64_t idB = pipeline.Submit(b, [&](TranslationResult r) {
        latest = std::move(r);
        latestDone.store(true);
        if (staleDelivered.load() > 0) {
            bothDone.store(true);
        }
    });
    CHECK(idB > idA);
    for (int i = 0; i < 200 && !bothDone.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(latestDone.load());
    CHECK(latest.ok);
    CHECK_STREQ(latest.text, L"[bbbb]");
    // Request A must have been marked stale (its callback fires with ok=false).
    CHECK_GT(staleDelivered.load(), 0);
}

TEST(pipeline_caches_results) {
    auto provider = std::make_shared<FakeProvider>(std::chrono::milliseconds(5));
    auto cache = std::make_shared<TranslationCache>(100);
    TranslationPipeline pipeline(provider, cache, 30000);

    TranslationRequest req;
    req.text = L"hola";
    req.sourceLanguage = L"es";
    req.targetLanguage = L"en";

    std::atomic<int> okCount{0};
    for (int i = 0; i < 3; ++i) {
        std::atomic<bool> done{false};
        pipeline.Submit(req, [&](TranslationResult r) {
            if (r.ok) {
                okCount.fetch_add(1);
            }
            done.store(true);
        });
        for (int k = 0; k < 200 && !done.load(); ++k) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    CHECK_EQ(okCount.load(), 3);
    CHECK_GT(cache->Hits(), 0u); // at least one cache hit
    // Provider only ran once (first request) — cached afterwards.
    CHECK_LE(provider->requests_, 2);
}

TEST(pipeline_cancel_all) {
    auto provider = std::make_shared<FakeProvider>(std::chrono::milliseconds(100));
    auto cache = std::make_shared<TranslationCache>(100);
    TranslationPipeline pipeline(provider, cache, 30000);
    TranslationRequest req;
    req.text = L"hola";
    req.sourceLanguage = L"es";
    req.targetLanguage = L"en";
    std::atomic<bool> done{false};
    TranslationResult got;
    pipeline.Submit(req, [&](TranslationResult r) {
        got = std::move(r);
        done.store(true);
    });
    pipeline.CancelAll();
    CHECK(provider->cancelled_->load());
    // The in-flight worker must be allowed to finish (it fires the cancelled
    // callback); never destroy the provider while a worker is still sleeping.
    for (int i = 0; i < 200 && !done.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(done.load());
    CHECK(!got.ok);
    CHECK_STREQ(got.error, L"cancelled");
}

TEST(pipeline_rejects_without_provider) {
    auto cache = std::make_shared<TranslationCache>(100);
    TranslationPipeline pipeline(nullptr, cache, 30000);
    TranslationRequest req;
    req.text = L"hola";
    CHECK_EQ(pipeline.Submit(req, [](TranslationResult) {}), 0);
}

namespace {
class ThrowingProvider : public ITranslationProvider {
public:
    std::wstring_view Name() const override { return L"throwing"; }
    std::wstring_view DisplayName() const override { return L"Throwing"; }
    bool RequiresNetwork() const override { return false; }
    ProviderStatus Status() const override { return {ProviderStatus::State::Ready, L""}; }
    void CancelAll() override {}

    void Translate(TranslationRequest request, TranslationCallback callback) override {
        (void)request;
        (void)callback;
        throw std::runtime_error("simulated provider exception");
    }
};
} // namespace

TEST(pipeline_handles_provider_exception) {
    auto provider = std::make_shared<ThrowingProvider>();
    auto cache = std::make_shared<TranslationCache>(100);
    TranslationPipeline pipeline(provider, cache, 30000);

    TranslationRequest req;
    req.text = L"hola";
    req.sourceLanguage = L"es";
    req.targetLanguage = L"en";

    bool callbackCalled = false;
    TranslationResult got;
    const int64_t id = pipeline.Submit(req, [&](TranslationResult r) {
        got = std::move(r);
        callbackCalled = true;
    });

    CHECK_GT(id, 0);
    CHECK(callbackCalled);
    CHECK(!got.ok);
    CHECK_STREQ(got.error, L"provider threw");
    CHECK_STREQ(got.errorDetail, L"simulated provider exception");
    CHECK_EQ(pipeline.TotalPending(), 0u);
}