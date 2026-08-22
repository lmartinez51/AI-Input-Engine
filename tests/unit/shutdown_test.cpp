#include "mini_test.h"

#include "Composition/InputEngine.h"
#include "Providers/GeminiProvider.h"
#include "Providers/MockProvider.h"
#include "Translation/TranslationCache.h"
#include "Translation/TranslationPipeline.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace aiie;

namespace {
void WaitFor(std::function<bool()> pred, int timeoutMs = 10000) {
    for (int i = 0; i < timeoutMs / 10 && !pred(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
} // namespace

TEST(gemini_provider_clean_shutdown_no_key) {
    // Empty key: requests fail fast without network. Verifies the worker
    // thread lifecycle: queue drains, callbacks fire, destruction joins.
    GeminiConfig gc;
    auto provider = std::make_shared<GeminiProvider>(gc);
    std::atomic<int> callbacks{0};
    TranslationRequest req;
    req.text = L"hola";
    req.sourceLanguage = L"es";
    req.targetLanguage = L"en";
    for (int i = 0; i < 3; ++i) {
        provider->Translate(req, [&](TranslationResult r) {
            CHECK(!r.ok);
            callbacks.fetch_add(1);
        });
    }
    WaitFor([&] { return callbacks.load() >= 3; });
    CHECK_EQ(callbacks.load(), 3);
    provider->CancelAll();
    provider.reset(); // joins the worker thread; must return promptly
    CHECK(true);
}

TEST(gemini_provider_cancel_and_destroy_while_queued) {
    GeminiConfig gc;
    auto provider = std::make_shared<GeminiProvider>(gc);
    std::atomic<int> callbacks{0};
    TranslationRequest req;
    req.text = L"hola";
    req.sourceLanguage = L"es";
    req.targetLanguage = L"en";
    for (int i = 0; i < 5; ++i) {
        provider->Translate(req, [&](TranslationResult r) { callbacks.fetch_add(1); (void)r; });
    }
    provider->CancelAll();
    provider.reset(); // must not hang even with queued work
    CHECK(true);
}

TEST(pipeline_teardown_with_inflight_request) {
    // The provider is destroyed while a request is in flight. The shared
    // cancellation flag keeps the worker safe and destruction must not hang.
    auto cache = std::make_shared<TranslationCache>(100);
    auto provider = std::make_shared<MockProvider>(std::chrono::milliseconds(200));
    auto pipeline = std::make_shared<TranslationPipeline>(provider, cache, 30000);
    std::atomic<int> callbacks{0};
    TranslationRequest req;
    req.text = L"hola";
    req.sourceLanguage = L"es";
    req.targetLanguage = L"en";
    pipeline->Submit(req, [&](TranslationResult r) {
        callbacks.fetch_add(1);
        (void)r;
    });
    pipeline.reset();
    provider.reset();
    WaitFor([&] { return callbacks.load() >= 1; });
    CHECK_EQ(callbacks.load(), 1);
}

TEST(engine_teardown_with_inflight_translation) {
    // Engine destroyed while a translation is in flight: teardown must be
    // prompt and must not deadlock (recursive mutex + debouncer stop).
    auto dispatcher = std::make_shared<ImmediateDispatcher>();
    struct FakeBackend : IInputBackend {
        void CommitReplacement(std::wstring, const std::wstring&) override {}
        void OnBufferStarted() override {}
        void OnBufferChanged(std::wstring_view) override {}
        void OnBufferCommitted() override {}
        void ShowStatus(std::wstring_view) override {}
        void HideStatus() override {}
    };
    auto backend = std::make_shared<FakeBackend>();
    auto provider = std::make_shared<MockProvider>(std::chrono::milliseconds(150));
    auto cache = std::make_shared<TranslationCache>(100);
    auto pipeline = std::make_shared<TranslationPipeline>(provider, cache, 30000);
    auto engine = std::make_shared<InputEngine>(dispatcher, backend, pipeline, nullptr);
    AppConfig cfg;
    cfg.enabled = true;
    cfg.sourceLanguage = L"es";
    cfg.targetLanguage = L"en";
    cfg.debounceMs = 1;
    cfg.minChars = 2;
    engine->ApplyConfig(cfg);
    for (const wchar_t c : std::wstring(L"hola")) {
        engine->HandleCharacter(c);
    }
    // Translation is in flight; destroy the engine (and the stack) immediately.
    engine.reset();
    pipeline.reset();
    provider.reset();
    CHECK(true);
}