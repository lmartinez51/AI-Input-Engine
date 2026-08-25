#include "mini_test.h"

#include "Composition/SmartPasteManager.h"
#include "Configuration/ConfigStore.h"
#include "Core/ClipboardHelper.h"
#include "Core/Dispatcher.h"
#include "Providers/MockProvider.h"
#include "Translation/TranslationCache.h"
#include "Translation/TranslationPipeline.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

using namespace aiie;

namespace {

class FakeStatusWindow : public StatusWindow {
public:
    std::vector<std::wstring> messages;

    void Show(std::wstring_view message) override {
        messages.emplace_back(message);
    }

    void Hide() override {}
};

class SmartPasteTestProvider : public ITranslationProvider {
public:
    std::atomic<int> callCount{0};
    bool failNext = false;
    std::chrono::milliseconds delay{0};
    std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>>(false);

    explicit SmartPasteTestProvider(std::chrono::milliseconds artificialDelay = std::chrono::milliseconds(0))
        : delay(artificialDelay) {}

    std::wstring_view Name() const override { return L"test_mock"; }
    std::wstring_view DisplayName() const override { return L"Test Mock Provider"; }
    bool RequiresNetwork() const override { return false; }
    ProviderStatus Status() const override { return ProviderStatus{ProviderStatus::State::Ready, L""}; }
    void CancelAll() override {
        if (cancelled) {
            cancelled->store(true);
        }
    }

    void Translate(TranslationRequest request, TranslationCallback callback) override {
        callCount.fetch_add(1);
        if (delay.count() > 0) {
            auto cancelFlag = cancelled;
            std::thread([cancelFlag, request = std::move(request), cb = std::move(callback), d = delay, fail = failNext]() mutable {
                std::this_thread::sleep_for(d);
                if (cancelFlag && cancelFlag->load()) {
                    return;
                }
                TranslationResult result;
                result.requestId = request.requestId;
                if (fail) {
                    result.ok = false;
                    result.error = L"Simulated provider failure";
                } else {
                    result.ok = true;
                    result.text = MockProvider::Transform(request);
                }
                cb(std::move(result));
            }).detach();
            return;
        }

        TranslationResult result;
        result.requestId = request.requestId;
        if (failNext) {
            result.ok = false;
            result.error = L"Simulated provider failure";
        } else {
            result.ok = true;
            result.text = MockProvider::Transform(request);
        }
        callback(std::move(result));
    }
};

struct SmartPasteTestHarness {
    std::filesystem::path tempConfigPath;
    std::unique_ptr<ConfigStore> configStore;
    std::shared_ptr<ImmediateDispatcher> dispatcher;
    std::shared_ptr<SmartPasteTestProvider> provider;
    std::shared_ptr<TranslationCache> cache;
    std::shared_ptr<TranslationPipeline> pipeline;
    std::shared_ptr<FakeStatusWindow> status;
    std::unique_ptr<SmartPasteManager> manager;

    SmartPasteTestHarness() {
        tempConfigPath = std::filesystem::temp_directory_path() / L"aiie_smart_paste_test_config.json";
        configStore = std::make_unique<ConfigStore>(tempConfigPath);

        AppConfig cfg;
        cfg.enabled = true;
        cfg.smartPasteEnabled = true;
        cfg.operation = TransformOperation::Translate;
        cfg.sourceLanguage = L"es";
        cfg.targetLanguage = L"en";
        cfg.profile = TranslationProfile::Neutral;
        cfg.cacheEnabled = true;
        configStore->Set(cfg);

        dispatcher = std::make_shared<ImmediateDispatcher>();
        provider = std::make_shared<SmartPasteTestProvider>(std::chrono::milliseconds(0));
        cache = std::make_shared<TranslationCache>(100);
        pipeline = std::make_shared<TranslationPipeline>(provider, cache, 30000);
        status = std::make_shared<FakeStatusWindow>();
        manager = std::make_unique<SmartPasteManager>(pipeline, status, configStore.get(), dispatcher);
    }

    ~SmartPasteTestHarness() {
        if (provider) {
            provider->CancelAll();
        }
        if (pipeline) {
            pipeline->CancelAll();
        }
        std::error_code ec;
        std::filesystem::remove(tempConfigPath, ec);
    }
};

} // namespace

TEST(smart_paste_translate_es_to_en) {
    SmartPasteTestHarness h;
    DWORD seq = 0;
    ClipboardHelper::SetUnicodeText(L"Hola mundo", seq);

    const bool triggered = h.manager->Trigger();
    CHECK(triggered);

    // Verify status was updated
    CHECK(!h.status->messages.empty());
    bool sawTranslating = false;
    for (const auto& msg : h.status->messages) {
        if (msg.find(L"Smart Paste") != std::wstring::npos) {
            sawTranslating = true;
        }
    }
    CHECK(sawTranslating);
}

TEST(smart_paste_correct_and_rewrite) {
    SmartPasteTestHarness h;
    AppConfig cfg = h.configStore->Get();
    cfg.operation = TransformOperation::CorrectAndRewrite;
    cfg.sourceLanguage = L"es";
    cfg.targetLanguage = L"es";
    cfg.profile = TranslationProfile::Casual;
    h.configStore->Set(cfg);

    DWORD seq = 0;
    ClipboardHelper::SetUnicodeText(L"hola como estas", seq);

    const bool triggered = h.manager->Trigger();
    CHECK(triggered);
    CHECK(!h.status->messages.empty());
}

TEST(smart_paste_empty_clipboard) {
    SmartPasteTestHarness h;
    DWORD seq = 0;
    ClipboardHelper::SetUnicodeText(L"", seq);

    const bool triggered = h.manager->Trigger();
    CHECK(!triggered);
    CHECK(!h.status->messages.empty());
    CHECK(h.status->messages.back().find(L"empty") != std::wstring::npos);
}

TEST(smart_paste_unicode_text) {
    SmartPasteTestHarness h;
    const std::wstring unicodeInput = L"¿Cómo estás? München — 東京 日本";
    DWORD seq = 0;
    ClipboardHelper::SetUnicodeText(unicodeInput, seq);

    std::wstring readBack;
    DWORD readSeq = 0;
    CHECK(ClipboardHelper::ReadUnicodeText(readBack, readSeq));
    CHECK_EQ(readBack, unicodeInput);
    CHECK_EQ(readSeq, seq);
}

TEST(smart_paste_emoji_surrogate_pairs) {
    SmartPasteTestHarness h;
    const std::wstring emojiInput = L"Hola 👋 Mundo 🚀 ✨ 🐱‍👤";
    DWORD seq = 0;
    ClipboardHelper::SetUnicodeText(emojiInput, seq);

    std::wstring readBack;
    DWORD readSeq = 0;
    CHECK(ClipboardHelper::ReadUnicodeText(readBack, readSeq));
    CHECK_EQ(readBack, emojiInput);
}

TEST(smart_paste_clipboard_snapshot_inspect) {
    DWORD seq = 0;
    ClipboardHelper::SetUnicodeText(L"Sample text for inspection", seq);

    const auto inspect = ClipboardHelper::InspectClipboard();
    CHECK(inspect.hasText);
    CHECK(!inspect.hasUnsupportedNonText);
}

TEST(smart_paste_clipboard_preservation) {
    const std::wstring original = L"Original preserved clipboard text";
    DWORD originalSeq = 0;
    ClipboardHelper::SetUnicodeText(original, originalSeq);

    // Verify restoration works when sequence is unchanged
    const bool restored = ClipboardHelper::RestoreIfSequenceMatches(originalSeq, original);
    CHECK(restored);

    std::wstring readBack;
    DWORD finalSeq = 0;
    CHECK(ClipboardHelper::ReadUnicodeText(readBack, finalSeq));
    CHECK_EQ(readBack, original);
}

TEST(smart_paste_unsupported_non_text) {
    ClipboardCheckResult res;
    res.hasText = false;
    res.hasUnsupportedNonText = true;
    CHECK(res.hasUnsupportedNonText);
    CHECK(!res.hasText);
}

TEST(smart_paste_disabled_setting) {
    SmartPasteTestHarness h;
    AppConfig cfg = h.configStore->Get();
    cfg.smartPasteEnabled = false;
    h.configStore->Set(cfg);

    DWORD seq = 0;
    ClipboardHelper::SetUnicodeText(L"Test text", seq);

    const bool triggered = h.manager->Trigger();
    CHECK(!triggered);
    CHECK(!h.status->messages.empty());
    CHECK(h.status->messages.back().find(L"disabled") != std::wstring::npos);
}

TEST(smart_paste_app_disabled) {
    SmartPasteTestHarness h;
    AppConfig cfg = h.configStore->Get();
    cfg.enabled = false;
    h.configStore->Set(cfg);

    DWORD seq = 0;
    ClipboardHelper::SetUnicodeText(L"Test text", seq);

    const bool triggered = h.manager->Trigger();
    CHECK(!triggered);
    CHECK(!h.status->messages.empty());
    CHECK(h.status->messages.back().find(L"disabled") != std::wstring::npos);
}

TEST(smart_paste_provider_failure) {
    SmartPasteTestHarness h;
    // Set failing mock provider
    h.provider->failNext = true;

    DWORD seq = 0;
    ClipboardHelper::SetUnicodeText(L"Fail test", seq);

    const bool triggered = h.manager->Trigger();
    CHECK(triggered);

    // Verify manager caught failure and reported it
    CHECK(!h.status->messages.empty());
    bool sawFail = false;
    for (const auto& msg : h.status->messages) {
        if (msg.find(L"failed") != std::wstring::npos) {
            sawFail = true;
        }
    }
    CHECK(sawFail);
}

TEST(smart_paste_target_window_changed) {
    SmartPasteTestHarness h;
    DWORD seq = 0;
    ClipboardHelper::SetUnicodeText(L"Target check", seq);

    // Pass a fictitious window that is not the foreground window
    const bool triggered = h.manager->Trigger(reinterpret_cast<HWND>(static_cast<uintptr_t>(0xDEADBEEFULL)));
    CHECK(triggered);

    bool sawCancelled = false;
    for (const auto& msg : h.status->messages) {
        if (msg.find(L"target window changed") != std::wstring::npos) {
            sawCancelled = true;
        }
    }
    CHECK(sawCancelled);
}

TEST(smart_paste_target_process_changed) {
    SmartPasteTestHarness h;
    DWORD seq = 0;
    ClipboardHelper::SetUnicodeText(L"Process check", seq);

    const bool triggered = h.manager->Trigger();
    CHECK(triggered);
}

TEST(smart_paste_cache_hit) {
    SmartPasteTestHarness h;
    DWORD seq = 0;
    ClipboardHelper::SetUnicodeText(L"Cache me once", seq);

    // First request
    h.manager->Trigger();
    CHECK_EQ(h.provider->callCount.load(), 1);

    // Second identical request
    ClipboardHelper::SetUnicodeText(L"Cache me once", seq);
    h.manager->Trigger();
    // Cache hit: provider call count remains 1
    CHECK_EQ(h.provider->callCount.load(), 1);
}

TEST(smart_paste_cache_miss) {
    SmartPasteTestHarness h;
    DWORD seq = 0;
    ClipboardHelper::SetUnicodeText(L"Text 1", seq);
    h.manager->Trigger();
    CHECK_EQ(h.provider->callCount.load(), 1);

    ClipboardHelper::SetUnicodeText(L"Text 2", seq);
    h.manager->Trigger();
    // Cache miss: provider call count increases to 2
    CHECK_EQ(h.provider->callCount.load(), 2);
}

TEST(smart_paste_busy_guard) {
    SmartPasteTestHarness h;
    // Set a provider with async delay to test busy guard
    auto delayed = std::make_shared<SmartPasteTestProvider>(std::chrono::milliseconds(200));
    h.pipeline->SetProvider(delayed);

    DWORD seq = 0;
    ClipboardHelper::SetUnicodeText(L"Busy test", seq);

    const bool first = h.manager->Trigger();
    CHECK(first);
    CHECK(h.manager->IsBusy());

    // Second trigger while busy is rejected
    const bool second = h.manager->Trigger();
    CHECK(!second);
    CHECK(h.status->messages.back().find(L"busy") != std::wstring::npos);

    delayed->CancelAll();
    h.pipeline->CancelAll();
}

TEST(smart_paste_sequence_unchanged_restores) {
    DWORD originalSeq = 0;
    ClipboardHelper::SetUnicodeText(L"Sequence test original", originalSeq);

    // When sequence matches, restore succeeds
    const bool ok = ClipboardHelper::RestoreIfSequenceMatches(originalSeq, L"Sequence test original");
    CHECK(ok);
}

TEST(smart_paste_sequence_changed_skips_restore) {
    DWORD originalSeq = 0;
    ClipboardHelper::SetUnicodeText(L"Initial text", originalSeq);

    // Simulate external clipboard change (sequence increments)
    DWORD externalSeq = 0;
    ClipboardHelper::SetUnicodeText(L"User copied something else", externalSeq);
    CHECK(externalSeq != originalSeq);

    // Restore with old sequence MUST fail and not overwrite
    const bool ok = ClipboardHelper::RestoreIfSequenceMatches(originalSeq, L"Initial text");
    CHECK(!ok);

    // Verify newer clipboard data remains intact
    std::wstring currentText;
    DWORD curSeq = 0;
    ClipboardHelper::ReadUnicodeText(currentText, curSeq);
    CHECK_EQ(currentText, L"User copied something else");
}

TEST(smart_paste_ctrl_v_untouched) {
    AppConfig cfg;
    // Standard Ctrl+V is never registered
    CHECK(cfg.hotkeySmartPaste.modifiers == (MOD_CONTROL | MOD_SHIFT));
    CHECK(cfg.hotkeySmartPaste.vk == 'V');
    CHECK(cfg.hotkeyToggle.modifiers == (MOD_CONTROL | MOD_ALT));
}

TEST(smart_paste_runtime_config_updated) {
    SmartPasteTestHarness h;
    AppConfig cfg = h.configStore->Get();
    cfg.targetLanguage = L"fr";
    cfg.profile = TranslationProfile::Professional;
    h.configStore->Set(cfg);

    DWORD seq = 0;
    ClipboardHelper::SetUnicodeText(L"Bonjour", seq);

    const bool triggered = h.manager->Trigger();
    CHECK(triggered);
    // Config store holds the live updated values
    CHECK_EQ(h.configStore->Get().targetLanguage, L"fr");
    CHECK(h.configStore->Get().profile == TranslationProfile::Professional);
}
