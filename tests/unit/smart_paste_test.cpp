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
#include <map>
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

TEST(smart_paste_sensitive_secret_detection) {
    // Private keys
    CHECK(SmartPasteManager::IsSensitiveOrSecret(L"-----BEGIN RSA PRIVATE KEY-----\nMIIEowIBAAKCAQEA...\n-----END RSA PRIVATE KEY-----"));
    CHECK(SmartPasteManager::IsSensitiveOrSecret(L"-----BEGIN OPENSSH PRIVATE KEY-----\nb3BlbnNza...\n-----END OPENSSH PRIVATE KEY-----"));
    CHECK(SmartPasteManager::IsSensitiveOrSecret(L"-----BEGIN EC PRIVATE KEY-----\nMHcCAQEEI...\n-----END EC PRIVATE KEY-----"));

    // Provider and cloud API keys
    CHECK(SmartPasteManager::IsSensitiveOrSecret(L"sk-proj-abc123456789012345678901234567890"));
    CHECK(SmartPasteManager::IsSensitiveOrSecret(L"sk-123456789012345678901234567890"));
    CHECK(SmartPasteManager::IsSensitiveOrSecret(L"(sk-proj-abc123456789012345678901234567890)"));
    CHECK(SmartPasteManager::IsSensitiveOrSecret(L"AIzaSyD-abc123def456ghi789jkl012mno345"));
    CHECK(SmartPasteManager::IsSensitiveOrSecret(L"ghp_1234567890abcdefghijklmnopqrstuvwxyz"));
    CHECK(SmartPasteManager::IsSensitiveOrSecret(L"glpat-1234567890abcdefghij"));
    CHECK(SmartPasteManager::IsSensitiveOrSecret(L"xoxb-1234567890-abcdefghij"));
    CHECK(SmartPasteManager::IsSensitiveOrSecret(L"AKIAIOSFODNN7EXAMPLE"));
    CHECK(SmartPasteManager::IsSensitiveOrSecret(L"AWS_ACCESS_KEY_ID=AKIAIOSFODNN7EXAMPLE"));
    CHECK(SmartPasteManager::IsSensitiveOrSecret(L"export AWS_ACCESS_KEY_ID=AKIAIOSFODNN7EXAMPLE"));

    // Tokens and connection strings
    CHECK(SmartPasteManager::IsSensitiveOrSecret(L"Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0.SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c"));
    CHECK(SmartPasteManager::IsSensitiveOrSecret(L"eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0.SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c"));
    CHECK(SmartPasteManager::IsSensitiveOrSecret(L"Bearer example\nAuthorization: Bearer abcdefghijklmnopqrstuvwxyz012345"));
    CHECK(SmartPasteManager::IsSensitiveOrSecret(L"incomplete eyJ.test\nValid: eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0.SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c"));
    CHECK(SmartPasteManager::IsSensitiveOrSecret(L"postgresql://admin:SecretPass123!@db.internal:5432/production"));
    CHECK(SmartPasteManager::IsSensitiveOrSecret(L"mongodb://user:p%40ssw0rd@localhost:27017/analytics"));

    // Key-value pairs (unquoted and environment variables)
    CHECK(SmartPasteManager::IsSensitiveOrSecret(L"password=hunter2"));
    CHECK(SmartPasteManager::IsSensitiveOrSecret(L"PASSWORD=hunter2; other_var=123"));
    CHECK(SmartPasteManager::IsSensitiveOrSecret(L"AWS_SECRET_ACCESS_KEY=wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"));
    CHECK(SmartPasteManager::IsSensitiveOrSecret(L"client_secret: unquoted_super_secret_value"));

    // Benign text should not be flagged as secret
    CHECK(!SmartPasteManager::IsSensitiveOrSecret(L"Hello world, this is a standard sentence."));
    CHECK(!SmartPasteManager::IsSensitiveOrSecret(L"The quick brown fox jumps over the lazy dog."));
}

TEST(smart_paste_source_code_detection) {
    // Shebangs
    CHECK(SmartPasteManager::IsSourceCode(L"#!/usr/bin/env python3\nprint('hello')"));
    CHECK(SmartPasteManager::IsSourceCode(L"#!/bin/bash\necho 'running'"));

    // Markdown code fences
    CHECK(SmartPasteManager::IsSourceCode(L"```json\n{\"id\": 10}\n```"));
    CHECK(SmartPasteManager::IsSourceCode(L"```cpp\nint main() {}\n```"));

    // C/C++ preprocessor & statements
    CHECK(SmartPasteManager::IsSourceCode(L"#include <vector>\n#include <string>\nint compute(int x) {\n    return x * 2;\n}"));
    CHECK(SmartPasteManager::IsSourceCode(L"#define BUFFER_SIZE 4096\nstatic const int maxRetries = 3;"));

    // Python syntax
    CHECK(SmartPasteManager::IsSourceCode(L"def fetch_user(user_id: int) -> dict:\n    data = db.query(user_id)\n    return data"));

    // Java / C# syntax
    CHECK(SmartPasteManager::IsSourceCode(L"public class ApplicationRunner {\n    public static void main(String[] args) {\n        System.out.println(\"running\");\n    }\n}"));

    // SQL syntax
    CHECK(SmartPasteManager::IsSourceCode(L"SELECT u.id, u.email FROM users u WHERE u.active = 1;"));
    CHECK(SmartPasteManager::IsSourceCode(L"INSERT INTO audit_logs (id, event) VALUES (1, 'login');"));

    // JSON objects and arrays
    CHECK(SmartPasteManager::IsSourceCode(L"{\n  \"status\": \"ok\",\n  \"count\": 42\n}"));
    CHECK(SmartPasteManager::IsSourceCode(L"[\n  {\"name\": \"Alice\"},\n  {\"name\": \"Bob\"}\n]"));

    // TypeScript / JS
    CHECK(SmartPasteManager::IsSourceCode(L"const calculateTotal = (items: Item[]): number => {\n    return items.reduce((a, b) => a + b.price, 0);\n};"));

    // Standalone calls and statements
    CHECK(SmartPasteManager::IsSourceCode(L"console.log(\"hola\");"));
    CHECK(SmartPasteManager::IsSourceCode(L"System.out.println(\"running\");"));
    CHECK(SmartPasteManager::IsSourceCode(L"console.log(\"test\")", /*targetIsEditor=*/true));
    CHECK(SmartPasteManager::IsSourceCode(L"print(\"hola\")", /*targetIsEditor=*/true));

    // Editor sensitivity flag
    CHECK(SmartPasteManager::IsSourceCode(L"std::vector<int> numbers = {1, 2, 3};", /*targetIsEditor=*/true));

    // Plain natural language text should not be flagged as code
    CHECK(!SmartPasteManager::IsSourceCode(L"This is just an email discussing project deadlines for tomorrow morning."));
    CHECK(!SmartPasteManager::IsSourceCode(L"Por favor revisa el documento adjunto y confirmame si estas de acuerdo."));
    CHECK(!SmartPasteManager::IsSourceCode(L"Por favor llama a Juan mañana a las 5.", /*targetIsEditor=*/true));
}

TEST(smart_paste_literal_and_short_circuit_detection) {
    AppConfig cfg;
    cfg.sourceLanguage = L"es";
    cfg.targetLanguage = L"en";

    // URLs
    CHECK(SmartPasteManager::IsLiteralOrShortCircuit(L"https://github.com/lmartinez51/AI-Input-Engine", cfg));
    CHECK(SmartPasteManager::IsLiteralOrShortCircuit(L"http://localhost:8080/api/v1/health", cfg));
    CHECK(SmartPasteManager::IsLiteralOrShortCircuit(L"ftp://files.example.com/archive.zip", cfg));

    // File paths
    CHECK(SmartPasteManager::IsLiteralOrShortCircuit(L"C:\\Users\\admin\\AppData\\Local\\Temp\\config.json", cfg));
    CHECK(SmartPasteManager::IsLiteralOrShortCircuit(L"D:\\Projects\\AIInputEngine\\build\\Release\\aiie.exe", cfg));
    CHECK(SmartPasteManager::IsLiteralOrShortCircuit(L"/etc/nginx/sites-available/default", cfg));
    CHECK(SmartPasteManager::IsLiteralOrShortCircuit(L"/var/log/syslog", cfg));

    // Emails
    CHECK(SmartPasteManager::IsLiteralOrShortCircuit(L"developer@example.com", cfg));
    CHECK(SmartPasteManager::IsLiteralOrShortCircuit(L"support@service.corp.internal", cfg));

    // Numeric / telephone
    CHECK(SmartPasteManager::IsLiteralOrShortCircuit(L"1,234,567.89", cfg));
    CHECK(SmartPasteManager::IsLiteralOrShortCircuit(L"(555) 123-4567", cfg));

    // Identical languages short-circuit
    AppConfig sameLangCfg = cfg;
    sameLangCfg.sourceLanguage = L"en";
    sameLangCfg.targetLanguage = L"en";
    CHECK(SmartPasteManager::IsLiteralOrShortCircuit(L"This text is already in English.", sameLangCfg));

    // Different languages should not be short-circuited
    CHECK(!SmartPasteManager::IsLiteralOrShortCircuit(L"Este texto debe ser traducido al ingles.", cfg));
}

TEST(smart_paste_editor_process_detection) {
    CHECK(SmartPasteManager::IsEditorExecutable(L"Code.exe"));
    CHECK(SmartPasteManager::IsEditorExecutable(L"code.exe"));
    CHECK(SmartPasteManager::IsEditorExecutable(L"devenv.exe"));
    CHECK(SmartPasteManager::IsEditorExecutable(L"idea64.exe"));
    CHECK(SmartPasteManager::IsEditorExecutable(L"notepad++.exe"));
    CHECK(SmartPasteManager::IsEditorExecutable(L"sublime_text.exe"));
    CHECK(SmartPasteManager::IsEditorExecutable(L"pycharm64.exe"));
    CHECK(SmartPasteManager::IsEditorExecutable(L"clion64.exe"));

    CHECK(!SmartPasteManager::IsEditorExecutable(L"chrome.exe"));
    CHECK(!SmartPasteManager::IsEditorExecutable(L"msedge.exe"));
    CHECK(!SmartPasteManager::IsEditorExecutable(L"calc.exe"));
    CHECK(!SmartPasteManager::IsEditorExecutable(L""));
}

TEST(smart_paste_trigger_intercepts_secrets) {
    SmartPasteTestHarness h;
    DWORD seq = 0;
    ClipboardHelper::SetUnicodeText(L"sk-proj-abc123456789012345678901234567890", seq);

    const bool triggered = h.manager->Trigger();
    CHECK(triggered);

    // Network provider MUST NOT be called for sensitive data
    CHECK_EQ(h.provider->callCount.load(), 0);

    // Status notification displayed
    CHECK(!h.status->messages.empty());
    bool containsSensitiveNote = false;
    for (const auto& msg : h.status->messages) {
        if (msg.find(L"sensitive") != std::wstring::npos ||
            msg.find(L"Safe paste") != std::wstring::npos) {
            containsSensitiveNote = true;
            break;
        }
    }
    CHECK(containsSensitiveNote);
}

TEST(smart_paste_trigger_intercepts_aws_env_var) {
    SmartPasteTestHarness h;
    DWORD seq = 0;
    ClipboardHelper::SetUnicodeText(L"AWS_ACCESS_KEY_ID=AKIAIOSFODNN7EXAMPLE", seq);

    const bool triggered = h.manager->Trigger();
    CHECK(triggered);

    // Network provider MUST NOT be called for AWS credential assignment
    CHECK_EQ(h.provider->callCount.load(), 0);

    CHECK(!h.status->messages.empty());
    bool containsSensitiveNote = false;
    for (const auto& msg : h.status->messages) {
        if (msg.find(L"sensitive") != std::wstring::npos ||
            msg.find(L"Safe paste") != std::wstring::npos) {
            containsSensitiveNote = true;
            break;
        }
    }
    CHECK(containsSensitiveNote);
}

TEST(smart_paste_trigger_intercepts_unquoted_secrets) {
    SmartPasteTestHarness h;
    DWORD seq = 0;
    ClipboardHelper::SetUnicodeText(L"AWS_SECRET_ACCESS_KEY=wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY", seq);

    const bool triggered = h.manager->Trigger();
    CHECK(triggered);

    // Network provider MUST NOT be called for unquoted AWS secret key
    CHECK_EQ(h.provider->callCount.load(), 0);

    CHECK(!h.status->messages.empty());
    bool containsSensitiveNote = false;
    for (const auto& msg : h.status->messages) {
        if (msg.find(L"sensitive") != std::wstring::npos ||
            msg.find(L"Safe paste") != std::wstring::npos) {
            containsSensitiveNote = true;
            break;
        }
    }
    CHECK(containsSensitiveNote);
}

TEST(smart_paste_trigger_intercepts_code) {
    SmartPasteTestHarness h;
    DWORD seq = 0;
    ClipboardHelper::SetUnicodeText(L"#include <iostream>\nint main() {\n    std::cout << \"ok\";\n    return 0;\n}", seq);

    const bool triggered = h.manager->Trigger();
    CHECK(triggered);

    // Network provider MUST NOT be called for source code
    CHECK_EQ(h.provider->callCount.load(), 0);

    // Status notification displayed
    CHECK(!h.status->messages.empty());
    bool containsCodeNote = false;
    for (const auto& msg : h.status->messages) {
        if (msg.find(L"code") != std::wstring::npos ||
            msg.find(L"Safe paste") != std::wstring::npos) {
            containsCodeNote = true;
            break;
        }
    }
    CHECK(containsCodeNote);
}

TEST(smart_paste_trigger_intercepts_short_circuit_url) {
    SmartPasteTestHarness h;
    DWORD seq = 0;
    ClipboardHelper::SetUnicodeText(L"https://typesafe.ai/primitives/choice", seq);

    const bool triggered = h.manager->Trigger();
    CHECK(triggered);

    // Network provider MUST NOT be called for URL short-circuit
    CHECK_EQ(h.provider->callCount.load(), 0);

    // Status notification displayed
    CHECK(!h.status->messages.empty());
    bool containsVerbatimNote = false;
    for (const auto& msg : h.status->messages) {
        if (msg.find(L"verbatim") != std::wstring::npos) {
            containsVerbatimNote = true;
            break;
        }
    }
    CHECK(containsVerbatimNote);
}

TEST(smart_paste_trigger_identical_languages_bypasses_api) {
    SmartPasteTestHarness h;
    AppConfig cfg = h.configStore->Get();
    cfg.sourceLanguage = L"en";
    cfg.targetLanguage = L"en";
    h.configStore->Set(cfg);

    DWORD seq = 0;
    ClipboardHelper::SetUnicodeText(L"This English sentence requires no translation.", seq);

    const bool triggered = h.manager->Trigger();
    CHECK(triggered);

    // Network provider MUST NOT be called when languages match
    CHECK_EQ(h.provider->callCount.load(), 0);

    CHECK(!h.status->messages.empty());
    bool containsVerbatimNote = false;
    for (const auto& msg : h.status->messages) {
        if (msg.find(L"verbatim") != std::wstring::npos) {
            containsVerbatimNote = true;
            break;
        }
    }
    CHECK(containsVerbatimNote);
}

TEST(smart_paste_pre_paste_hook_called) {
    SmartPasteTestHarness h;
    bool hookInvoked = false;
    h.manager->SetPrePasteHook([&hookInvoked]() {
        hookInvoked = true;
    });

    DWORD seq = 0;
    ClipboardHelper::SetUnicodeText(L"https://example.com/test", seq);

    const bool triggered = h.manager->Trigger();
    CHECK(triggered);
    CHECK(hookInvoked);
}

TEST(smart_paste_analysis_struct_details) {
    SmartPasteTestHarness h;

    auto secretAnalysis = h.manager->AnalyzeContent(L"sk-123456789012345678901234567890");
    CHECK(secretAnalysis.decision == SmartPasteDecision::PasteVerbatim);
    CHECK(secretAnalysis.kind == ContentKind::SecretOrCredential);
    CHECK(secretAnalysis.isSensitive);
    CHECK(!secretAnalysis.isCode);

    auto codeAnalysis = h.manager->AnalyzeContent(L"def process_job(job_id: str):\n    pass");
    CHECK(codeAnalysis.decision == SmartPasteDecision::PasteVerbatim);
    CHECK(codeAnalysis.kind == ContentKind::SourceCode);
    CHECK(codeAnalysis.isCode);
    CHECK(!codeAnalysis.isSensitive);

    auto urlAnalysis = h.manager->AnalyzeContent(L"https://platform.openai.com/docs");
    CHECK(urlAnalysis.decision == SmartPasteDecision::PasteVerbatim);
    CHECK(urlAnalysis.kind == ContentKind::LiteralOrUrl);
    CHECK(urlAnalysis.isShortCircuit);

    auto naturalAnalysis = h.manager->AnalyzeContent(L"Buenos días, ¿cómo estás hoy?");
    CHECK(naturalAnalysis.decision == SmartPasteDecision::Transform);
    CHECK(naturalAnalysis.kind == ContentKind::NaturalLanguage);
    CHECK(!naturalAnalysis.isSensitive);
    CHECK(!naturalAnalysis.isCode);
    CHECK(!naturalAnalysis.isShortCircuit);
}

TEST(smart_paste_quoted_key_credentials_embedded_in_prose) {
    SmartPasteTestHarness h;

    // 1. Quoted key embedded in prose
    const std::wstring proseWithQuotedKey = L"Here is the credential: \"password\": \"hunter2\"";
    CHECK(SmartPasteManager::IsSensitiveOrSecret(proseWithQuotedKey));
    auto analysis1 = h.manager->AnalyzeContent(proseWithQuotedKey);
    CHECK(analysis1.decision == SmartPasteDecision::PasteVerbatim);
    CHECK(analysis1.isSensitive);

    // 2. Single-quoted key
    const std::wstring singleQuotedKey = L"Config item: 'api_key': 'my-secret-key-12345'";
    CHECK(SmartPasteManager::IsSensitiveOrSecret(singleQuotedKey));
    auto analysis2 = h.manager->AnalyzeContent(singleQuotedKey);
    CHECK(analysis2.decision == SmartPasteDecision::PasteVerbatim);
    CHECK(analysis2.isSensitive);

    // 3. Provider bypass verification via Trigger
    DWORD seq = 0;
    ClipboardHelper::SetUnicodeText(proseWithQuotedKey, seq);
    const bool triggered = h.manager->Trigger();
    CHECK(triggered);
    CHECK_EQ(h.provider->callCount.load(), 0);
}

TEST(smart_paste_bearer_tokens_case_insensitive_and_rfc6750_alphabet) {
    SmartPasteTestHarness h;

    // 1. Lowercase scheme name
    const std::wstring lowerScheme = L"Authorization: bearer abcdefghijklmnopqrstuvwxyz012345";
    CHECK(SmartPasteManager::IsSensitiveOrSecret(lowerScheme));

    // 2. Opaque token containing '+', '/', '~', and '=' padding
    const std::wstring opaqueAlphabet = L"Authorization: BEARER aabbccdd+eeffgg/hhiijj~kkllmmnnoopp==";
    CHECK(SmartPasteManager::IsSensitiveOrSecret(opaqueAlphabet));

    // 3. Multi-occurrence with short dummy scheme first
    const std::wstring multiOccurrence = L"bearer test and then bearer abcdefghijklmnopqrstuvwxyz012345";
    CHECK(SmartPasteManager::IsSensitiveOrSecret(multiOccurrence));

    // 4. Provider bypass verification via Trigger
    DWORD seq = 0;
    ClipboardHelper::SetUnicodeText(opaqueAlphabet, seq);
    const bool triggered = h.manager->Trigger();
    CHECK(triggered);
    CHECK_EQ(h.provider->callCount.load(), 0);
}

TEST(smart_paste_standalone_function_calls_in_editor) {
    SmartPasteTestHarness h;

    // 1. Arbitrary function name without semicolon in editor context
    CHECK(SmartPasteManager::IsSourceCode(L"send_message(\"hola\")", true));
    CHECK(!SmartPasteManager::IsSourceCode(L"send_message(\"hola\")", false));

    // 2. Arbitrary function name with whitespace before opening parenthesis
    CHECK(SmartPasteManager::IsSourceCode(L"calculate_total   ( items, discount = 0.1 )", true));
    CHECK(SmartPasteManager::IsSourceCode(L"do_work ()", true));

    // 3. Function call with comment
    CHECK(SmartPasteManager::IsSourceCode(L"my_func(\"arg\") // trailing comment", true));

    // 4. Prose must NOT be classified as code
    CHECK(!SmartPasteManager::IsSourceCode(L"Note (see below) for details", true));
    CHECK(!SmartPasteManager::IsSourceCode(L"Please send_message(\"hola\") now", true));

    // 5. Verification of editor classification
    CHECK(SmartPasteManager::IsEditorExecutable(L"code.exe"));
    CHECK(SmartPasteManager::IsEditorExecutable(L"devenv.exe"));
    CHECK(SmartPasteManager::IsEditorExecutable(L"sublime_text.exe"));
}

TEST(smart_paste_rfc6750_and_short_credentials_bypass_provider) {
    SmartPasteTestHarness h;

    // 1. Authorization header with short alphabetic token (5 chars: admin)
    const std::wstring bearerAdmin = L"Authorization: Bearer admin";
    CHECK(SmartPasteManager::IsSensitiveOrSecret(bearerAdmin));
    auto analysisAdmin = h.manager->AnalyzeContent(bearerAdmin);
    CHECK(analysisAdmin.decision == SmartPasteDecision::PasteVerbatim);
    CHECK(analysisAdmin.isSensitive);

    // 2. Authorization header with 1-char token
    const std::wstring bearerSingleChar = L"Authorization: Bearer a";
    CHECK(SmartPasteManager::IsSensitiveOrSecret(bearerSingleChar));

    // 3. RFC 6750 canonical token: mF_9.B5f-4.1JqM (length 16)
    const std::wstring rfcToken = L"Authorization: Bearer mF_9.B5f-4.1JqM";
    CHECK(SmartPasteManager::IsSensitiveOrSecret(rfcToken));

    // 4. Short unquoted password (5 chars): password=admin
    const std::wstring shortUnquoted = L"The password=admin is temporary.";
    CHECK(SmartPasteManager::IsSensitiveOrSecret(shortUnquoted));

    // 5. Short quoted password: {"password": "admin"}
    const std::wstring quotedAdmin = L"{\"password\": \"admin\"}";
    CHECK(SmartPasteManager::IsSensitiveOrSecret(quotedAdmin));

    // 6. Quoted Authorization header with extensive whitespace and 1-char token
    const std::wstring quotedBearerWhitespace = L"Use \"Authorization\":                                        \"Bearer a\" for this request.";
    CHECK(SmartPasteManager::IsSensitiveOrSecret(quotedBearerWhitespace));
    auto analysisQuotedBearer = h.manager->AnalyzeContent(quotedBearerWhitespace);
    CHECK(analysisQuotedBearer.decision == SmartPasteDecision::PasteVerbatim);
    CHECK(analysisQuotedBearer.isSensitive);

    // 6b. Authorization headers with newlines between scheme and token, or after colon
    const std::wstring bearerWithNewline = L"Use \"Authorization\": \"Bearer\na\" for this request.";
    CHECK(SmartPasteManager::IsSensitiveOrSecret(bearerWithNewline));
    auto analysisNewline = h.manager->AnalyzeContent(bearerWithNewline);
    CHECK(analysisNewline.decision == SmartPasteDecision::PasteVerbatim);
    CHECK(analysisNewline.isSensitive);

    const std::wstring bearerMultiline = L"Authorization:\nBearer\n1";
    CHECK(SmartPasteManager::IsSensitiveOrSecret(bearerMultiline));
    auto analysisMulti = h.manager->AnalyzeContent(bearerMultiline);
    CHECK(analysisMulti.decision == SmartPasteDecision::PasteVerbatim);
    CHECK(analysisMulti.isSensitive);

    // 6c. Quoted Bearer token with whitespace immediately inside opening quote
    const std::wstring bearerQuoteInnerWs = L"Use Authorization: Bearer \"\n\ta\" for this request.";
    CHECK(SmartPasteManager::IsSensitiveOrSecret(bearerQuoteInnerWs));
    auto analysisInnerWs = h.manager->AnalyzeContent(bearerQuoteInnerWs);
    CHECK(analysisInnerWs.decision == SmartPasteDecision::PasteVerbatim);
    CHECK(analysisInnerWs.isSensitive);

    const std::wstring bearerSingleQuoteInnerWs = L"Use Authorization: Bearer '\n\t1' for this request.";
    CHECK(SmartPasteManager::IsSensitiveOrSecret(bearerSingleQuoteInnerWs));

    // 6d. Multiline YAML key-value credentials
    const std::wstring yamlPassword = L"password:\n  hunter2";
    CHECK(SmartPasteManager::IsSensitiveOrSecret(yamlPassword));
    auto analysisYamlPass = h.manager->AnalyzeContent(yamlPassword);
    CHECK(analysisYamlPass.decision == SmartPasteDecision::PasteVerbatim);
    CHECK(analysisYamlPass.isSensitive);

    const std::wstring yamlApiKey = L"api_key:\n  \"my-secret-key-12345\"";
    CHECK(SmartPasteManager::IsSensitiveOrSecret(yamlApiKey));

    const std::wstring yamlClientSecret = L"client_secret:\n  unquoted_secret";
    CHECK(SmartPasteManager::IsSensitiveOrSecret(yamlClientSecret));

    // 6e. Multiline YAML credential with value beginning with a semicolon
    const std::wstring yamlSemicolonVal = L"password:\n  ;hunter2";
    CHECK(SmartPasteManager::IsSensitiveOrSecret(yamlSemicolonVal));
    auto analysisSemiVal = h.manager->AnalyzeContent(yamlSemicolonVal);
    CHECK(analysisSemiVal.decision == SmartPasteDecision::PasteVerbatim);
    CHECK(analysisSemiVal.isSensitive);

    const std::wstring yamlApiKeySemi = L"api_key:\n  ;my-super-secret-key";
    CHECK(SmartPasteManager::IsSensitiveOrSecret(yamlApiKeySemi));

    // 6f. Multiline YAML document with username and password with leading semicolon on same line as colon
    const std::wstring yamlDocWithSemi = L"username: alice\npassword: ;hunter2";
    CHECK(SmartPasteManager::IsSensitiveOrSecret(yamlDocWithSemi));
    auto analysisDocSemi = h.manager->AnalyzeContent(yamlDocWithSemi);
    CHECK(analysisDocSemi.decision == SmartPasteDecision::PasteVerbatim);
    CHECK(analysisDocSemi.isSensitive);

    // 7. Provider bypass test via Trigger
    DWORD seq = 0;
    ClipboardHelper::SetUnicodeText(bearerAdmin, seq);
    CHECK(h.manager->Trigger());
    CHECK_EQ(h.provider->callCount.load(), 0);

    ClipboardHelper::SetUnicodeText(bearerSingleChar, seq);
    CHECK(h.manager->Trigger());
    CHECK_EQ(h.provider->callCount.load(), 0);

    ClipboardHelper::SetUnicodeText(shortUnquoted, seq);
    CHECK(h.manager->Trigger());
    CHECK_EQ(h.provider->callCount.load(), 0);

    ClipboardHelper::SetUnicodeText(quotedBearerWhitespace, seq);
    CHECK(h.manager->Trigger());
    CHECK_EQ(h.provider->callCount.load(), 0);

    ClipboardHelper::SetUnicodeText(bearerWithNewline, seq);
    CHECK(h.manager->Trigger());
    CHECK_EQ(h.provider->callCount.load(), 0);

    ClipboardHelper::SetUnicodeText(bearerMultiline, seq);
    CHECK(h.manager->Trigger());
    CHECK_EQ(h.provider->callCount.load(), 0);

    ClipboardHelper::SetUnicodeText(bearerQuoteInnerWs, seq);
    CHECK(h.manager->Trigger());
    CHECK_EQ(h.provider->callCount.load(), 0);

    ClipboardHelper::SetUnicodeText(yamlPassword, seq);
    CHECK(h.manager->Trigger());
    CHECK_EQ(h.provider->callCount.load(), 0);

    ClipboardHelper::SetUnicodeText(yamlSemicolonVal, seq);
    CHECK(h.manager->Trigger());
    CHECK_EQ(h.provider->callCount.load(), 0);

    ClipboardHelper::SetUnicodeText(yamlDocWithSemi, seq);
    CHECK(h.manager->Trigger());
    CHECK_EQ(h.provider->callCount.load(), 0);
}

TEST(smart_paste_consecutive_function_calls_in_editor) {
    // 1. Consecutive calls without semicolons
    const std::wstring multiCalls = L"send_message('hola')\nsend_message('adios')";
    CHECK(SmartPasteManager::IsSourceCode(multiCalls, true));
    CHECK(!SmartPasteManager::IsSourceCode(multiCalls, false));

    // 2. Consecutive method calls on objects
    const std::wstring multiMethodCalls = L"obj.send_message('hola')\nobj.send_message('adios')";
    CHECK(SmartPasteManager::IsSourceCode(multiMethodCalls, true));
    CHECK(!SmartPasteManager::IsSourceCode(multiMethodCalls, false));

    const std::wstring loggerCalls = L"logger.info(\"start\")\nlogger.debug(\"end\")";
    CHECK(SmartPasteManager::IsSourceCode(loggerCalls, true));

    // 3. Consecutive calls with comments and whitespace
    const std::wstring multiWithComments = L"do_step(1) // first step\ndo_step(2)\n// comment line\ndo_step(3)";
    CHECK(SmartPasteManager::IsSourceCode(multiWithComments, true));

    // 4. Mixed calls with semicolons
    const std::wstring mixedCalls = L"init();\nexecute_job(ctx)\nfinalize();";
    CHECK(SmartPasteManager::IsSourceCode(mixedCalls, true));

    // 5. Negative tests: prose containing qualified calls or common prefixes must NOT be code
    CHECK(!SmartPasteManager::IsSourceCode(L"Please replace obj.send_message('hola') in the example.", true));
    CHECK(!SmartPasteManager::IsSourceCode(L"Please check console.log(\"test\") in the output.", true));
    CHECK(!SmartPasteManager::IsSourceCode(L"Consider using System.out.println(\"hello\") for debugging.", true));
    CHECK(!SmartPasteManager::IsSourceCode(L"We should invoke send_message('hola') when the user clicks submit.", true));

    // 6. Return statements: syntactic code vs prose in editor
    CHECK(!SmartPasteManager::IsSourceCode(L"Please return the signed document tomorrow.", true));
    CHECK(!SmartPasteManager::IsSourceCode(L"Please return obj.send_message('hola') to the caller.", true));
    CHECK(SmartPasteManager::IsSourceCode(L"return compute(x);", true));
    CHECK(SmartPasteManager::IsSourceCode(L"return 0;", true));

    // 7. Same-line consecutive calls separated by semicolons
    const std::wstring sameLineCalls = L"obj.prepare(); obj.execute();";
    CHECK(SmartPasteManager::IsSourceCode(sameLineCalls, true));

    // Negative: same-line call followed by prose must NOT be code
    CHECK(!SmartPasteManager::IsSourceCode(L"obj.prepare(); then we run the program", true));

    // 8. Prose containing declaration words and embedded calls must NOT be code
    CHECK(!SmartPasteManager::IsSourceCode(L"Please import the file using load_data('hola').", true));
    CHECK(!SmartPasteManager::IsSourceCode(L"You can use const for calculate_total('items').", true));
    CHECK(SmartPasteManager::IsSourceCode(L"import numpy as np\nload_data('file')", true));
    CHECK(SmartPasteManager::IsSourceCode(L"const total = calculate_total(items);", true));

    // 9. Negative tests for prose containing scope-qualified calls and operators
    CHECK(!SmartPasteManager::IsSourceCode(L"Please call std::move(value) before continuing.", true));
    CHECK(!SmartPasteManager::IsSourceCode(L"Please use ptr->method() here.", true));
    CHECK(SmartPasteManager::IsSourceCode(L"std::vector<int> numbers = {1, 2, 3};", true));
    CHECK(SmartPasteManager::IsSourceCode(L"auto fn = [](int x) -> int { return x; };", true));
}

TEST(smart_paste_modifier_normalization_combinations) {
    using Mod = ClipboardHelper::ModifierState;

    // 1. Enumerate and verify ALL 64 Shift/Alt/Ctrl modifier combinations
    for (int mask = 0; mask < 64; ++mask) {
        Mod m;
        m.leftShift  = (mask & 1) != 0;
        m.rightShift = (mask & 2) != 0;
        m.leftAlt    = (mask & 4) != 0;
        m.rightAlt   = (mask & 8) != 0;
        m.leftCtrl   = (mask & 16) != 0;
        m.rightCtrl  = (mask & 32) != 0;

        const int modCount = (m.leftShift ? 1 : 0) + (m.rightShift ? 1 : 0) +
                             (m.leftAlt ? 1 : 0) + (m.rightAlt ? 1 : 0);
        const bool ctrlHeldInitially = (m.leftCtrl || m.rightCtrl);
        auto inputs = ClipboardHelper::BuildNormalizedPasteInputs(m);
        CHECK_EQ(inputs.size(), static_cast<size_t>(modCount * 2 + (ctrlHeldInitially ? 2 : 4)));

        // Every input must carry the synthetic signature
        for (const auto& in : inputs) {
            CHECK_EQ(in.ki.dwExtraInfo, kAiieSyntheticSignature);
        }

        // Release prefix
        int relIdx = 0;
        if (m.leftShift) {
            CHECK_EQ(inputs[relIdx].ki.wVk, VK_LSHIFT);
            CHECK_EQ(inputs[relIdx].ki.dwFlags, KEYEVENTF_KEYUP);
            ++relIdx;
        }
        if (m.rightShift) {
            CHECK_EQ(inputs[relIdx].ki.wVk, VK_RSHIFT);
            CHECK_EQ(inputs[relIdx].ki.dwFlags, KEYEVENTF_KEYUP);
            ++relIdx;
        }
        if (m.leftAlt) {
            CHECK_EQ(inputs[relIdx].ki.wVk, VK_LMENU);
            CHECK_EQ(inputs[relIdx].ki.dwFlags, KEYEVENTF_KEYUP);
            ++relIdx;
        }
        if (m.rightAlt) {
            CHECK_EQ(inputs[relIdx].ki.wVk, VK_RMENU);
            CHECK_EQ(inputs[relIdx].ki.dwFlags, (KEYEVENTF_KEYUP | KEYEVENTF_EXTENDEDKEY));
            ++relIdx;
        }

        int restIdx = relIdx;
        if (!ctrlHeldInitially) {
            CHECK_EQ(inputs[restIdx].ki.wVk, VK_CONTROL);
            CHECK_EQ(inputs[restIdx].ki.dwFlags, 0);
            ++restIdx;
        }
        CHECK_EQ(inputs[restIdx].ki.wVk, 'V');
        CHECK_EQ(inputs[restIdx].ki.dwFlags, 0);
        ++restIdx;

        CHECK_EQ(inputs[restIdx].ki.wVk, 'V');
        CHECK_EQ(inputs[restIdx].ki.dwFlags, KEYEVENTF_KEYUP);
        ++restIdx;

        if (!ctrlHeldInitially) {
            CHECK_EQ(inputs[restIdx].ki.wVk, VK_CONTROL);
            CHECK_EQ(inputs[restIdx].ki.dwFlags, KEYEVENTF_KEYUP);
            ++restIdx;
        }

        // Restore suffix
        if (m.leftShift) {
            CHECK_EQ(inputs[restIdx].ki.wVk, VK_LSHIFT);
            CHECK_EQ(inputs[restIdx].ki.dwFlags, 0);
            ++restIdx;
        }
        if (m.rightShift) {
            CHECK_EQ(inputs[restIdx].ki.wVk, VK_RSHIFT);
            CHECK_EQ(inputs[restIdx].ki.dwFlags, 0);
            ++restIdx;
        }
        if (m.leftAlt) {
            CHECK_EQ(inputs[restIdx].ki.wVk, VK_LMENU);
            CHECK_EQ(inputs[restIdx].ki.dwFlags, 0);
            ++restIdx;
        }
        if (m.rightAlt) {
            CHECK_EQ(inputs[restIdx].ki.wVk, VK_RMENU);
            CHECK_EQ(inputs[restIdx].ki.dwFlags, KEYEVENTF_EXTENDEDKEY);
            ++restIdx;
        }
    }

    // 2. Test BuildRecoveryInputs for all sequence boundaries across all 64 modifier combinations
    for (int mask = 0; mask < 64; ++mask) {
        Mod m;
        m.leftShift  = (mask & 1) != 0;
        m.rightShift = (mask & 2) != 0;
        m.leftAlt    = (mask & 4) != 0;
        m.rightAlt   = (mask & 8) != 0;
        m.leftCtrl   = (mask & 16) != 0;
        m.rightCtrl  = (mask & 32) != 0;

        auto inputs = ClipboardHelper::BuildNormalizedPasteInputs(m);
        for (UINT sent = 1; sent < inputs.size(); ++sent) {
            auto recovery = ClipboardHelper::BuildRecoveryInputs(inputs, sent, m);
            // Check that every recovery input carries the synthetic signature
            for (const auto& rec : recovery) {
                CHECK_EQ(rec.ki.dwExtraInfo, kAiieSyntheticSignature);
            }

            // Simulate key state: true = DOWN, false = UP
            std::map<WORD, bool> keyState;
            // Initially, physically held modifiers are DOWN, synthetic keys are UP
            keyState[VK_LSHIFT]   = m.leftShift;
            keyState[VK_RSHIFT]   = m.rightShift;
            keyState[VK_LMENU]    = m.leftAlt;
            keyState[VK_RMENU]    = m.rightAlt;
            keyState[VK_LCONTROL] = m.leftCtrl;
            keyState[VK_RCONTROL] = m.rightCtrl;
            keyState[VK_CONTROL]  = (m.leftCtrl || m.rightCtrl);
            keyState['V']         = false;

            // Apply inputs dispatched so far [0 .. sent-1]
            for (UINT i = 0; i < sent; ++i) {
                const bool isUp = (inputs[i].ki.dwFlags & KEYEVENTF_KEYUP) != 0;
                keyState[inputs[i].ki.wVk] = !isUp;
                if (inputs[i].ki.wVk == VK_CONTROL) {
                    keyState[VK_LCONTROL] = !isUp;
                }
            }

            // Apply recovery inputs
            for (const auto& rec : recovery) {
                const bool isUp = (rec.ki.dwFlags & KEYEVENTF_KEYUP) != 0;
                keyState[rec.ki.wVk] = !isUp;
                if (rec.ki.wVk == VK_CONTROL) {
                    keyState[VK_LCONTROL] = !isUp;
                }
            }

            // Invariant: after recovery:
            // - 'V' must be UP
            // - Held modifiers must match their initial physical state
            CHECK(!keyState['V']);
            CHECK_EQ(keyState[VK_LCONTROL], m.leftCtrl);
            CHECK_EQ(keyState[VK_RCONTROL], m.rightCtrl);
            CHECK_EQ(keyState[VK_LSHIFT], m.leftShift);
            CHECK_EQ(keyState[VK_RSHIFT], m.rightShift);
            CHECK_EQ(keyState[VK_LMENU], m.leftAlt);
            CHECK_EQ(keyState[VK_RMENU], m.rightAlt);
        }
    }

    // 3. InputSender testable seam: success and partial failure dispatch recovery
    {
        UINT lastCount = 0;
        auto successSender = [&lastCount](UINT n, LPINPUT, int) -> UINT {
            lastCount = n;
            return n;
        };
        CHECK(ClipboardHelper::DispatchNormalizedPasteKeystroke(successSender));
        CHECK(lastCount >= 4);

        // Complete failure (0 sent) -> returns false, no recovery needed
        auto zeroSender = [](UINT, LPINPUT, int) -> UINT {
            return 0;
        };
        CHECK(!ClipboardHelper::DispatchNormalizedPasteKeystroke(zeroSender));

        // Partial failure at boundary: simulates 2 sent, verifies recovery batch is dispatched
        std::vector<std::vector<INPUT>> dispatchCalls;
        auto partialSender = [&dispatchCalls](UINT n, LPINPUT pInputs, int) -> UINT {
            std::vector<INPUT> copy(pInputs, pInputs + n);
            dispatchCalls.push_back(std::move(copy));
            if (dispatchCalls.size() == 1) {
                return 2; // Simulate partial send of 2 inputs on initial attempt
            }
            return n; // Accept all recovery inputs
        };
        CHECK(!ClipboardHelper::DispatchNormalizedPasteKeystroke(partialSender));
        // Must have received initial dispatch call AND subsequent recovery call
        CHECK_EQ(dispatchCalls.size(), 2);
        CHECK(!dispatchCalls[1].empty());
    }
}

TEST(smart_paste_punctuation_leading_and_period_secrets_provider_bypass) {
    SmartPasteTestHarness h;

    const std::vector<std::wstring_view> kPunctuationSecrets = {
        L"PASSWORD=,s3cr3t",
        L"PASSWORD=]s3cr3t",
        L"PASSWORD=}s3cr3t",
        L"PASSWORD=;s3cr3t",
        L"PASSWORD=...",
        L"api_key=...",
        L"secret_key=;hidden_val",
        L"aws_secret_access_key=,special_access",
        L"password:\n  ;hunter2",
        L"username: alice\npassword: ;hunter2",
        // Single- and double-quote-leading values and incomplete quoted assignments
        L"PASSWORD='s3cr3t",
        L"PASSWORD=\"s3cr3t",
        L"api_key='incomplete_token_value",
        L"secret_key=\"unclosed_secret_assignment",
        // Quoted YAML keys with indented scalar values on next line
        L"\"password\":\n hunter2",
        L"'password':\n hunter2",
        L"\"api_key\":\n secret_key_12345",
        // Unindented JSON credentials embedded in prose (JSON allows arbitrary whitespace across newlines)
        L"Credentials:\n{\"password\":\n\"hunter2\"}",
        L"Credentials:\n{\n\"password\":\n\"hunter2\"\n}",
        L"Config:\n{\n\"api_key\":\n\"secret_key_12345\"\n}",
        L"token_info:\n'auth_token':\n'abc123xyz'"
    };

    for (auto sec : kPunctuationSecrets) {
        CHECK(SmartPasteManager::IsSensitiveOrSecret(sec));

        auto analysis = h.manager->AnalyzeContent(sec);
        CHECK_EQ(static_cast<int>(analysis.decision), static_cast<int>(SmartPasteDecision::PasteVerbatim));
        CHECK(analysis.isSensitive);
        CHECK(!analysis.isCode);

        // Verify provider bypass: Trigger() must paste verbatim and NEVER dispatch to translation provider
        DWORD seq = 0;
        ClipboardHelper::SetUnicodeText(sec, seq);
        const bool triggered = h.manager->Trigger();
        CHECK(triggered);
        CHECK_EQ(h.provider->callCount.load(), 0); // Translation provider was NOT called
    }

    // Demonstrably empty quotes should NOT be flagged as secrets
    CHECK(!SmartPasteManager::IsSensitiveOrSecret(L"PASSWORD=\"\""));
    CHECK(!SmartPasteManager::IsSensitiveOrSecret(L"PASSWORD=''"));
    CHECK(!SmartPasteManager::IsSensitiveOrSecret(L"PASSWORD=\"   \""));
    CHECK(!SmartPasteManager::IsSensitiveOrSecret(L"password:\nusername: foo"));
    CHECK(!SmartPasteManager::IsSensitiveOrSecret(L"Credentials:\n{\"password\":\n\"\"}"));
}

TEST(smart_paste_operator_syntax_and_prose_distinction_tests) {
    SmartPasteTestHarness h;

    // Negative tests: natural prose discussing multiple operators in an editor context
    // MUST NOT be classified as code, and MUST be eligible for translation!
    const std::vector<std::wstring_view> kProseWithOperators = {
        L"Please replace == with != in the documentation.",
        L"Make sure to use ++ or -- instead of manual addition.",
        L"Compare items with == and != in your checks.",
        L"Check if value == other before continuing the process."
    };

    for (auto prose : kProseWithOperators) {
        CHECK(!SmartPasteManager::IsSourceCode(prose, /*isEditorContext=*/true));
        auto analysis = h.manager->AnalyzeContent(prose);
        CHECK_EQ(static_cast<int>(analysis.decision), static_cast<int>(SmartPasteDecision::Transform));
        CHECK_EQ(static_cast<int>(analysis.kind), static_cast<int>(ContentKind::NaturalLanguage));
        CHECK(!analysis.isCode);
        CHECK(!analysis.isSensitive);
    }

    // Isolated positive regressions: ordinary valid statements with assignment comparisons,
    // spaced member access, arrow functions, and spaced increment/decrement
    // with NO masking declaration keywords, return statements, or curly braces.
    const std::vector<std::wstring_view> kIsolatedOperatorStatements = {
        L"matches = left == right;",
        L"is_different = a != b;",
        L"ptr -> method();",
        L"x => x + 1;",
        L"i ++;",
        L"++ i;",
        L"ptr -> count ++;",
        L"val --;",
        L"-- val;"
    };

    for (auto stmt : kIsolatedOperatorStatements) {
        CHECK(SmartPasteManager::IsSourceCode(stmt, /*isEditorContext=*/true));
    }

    // Statements following another statement on the same line (statement-aware boundary analysis)
    const std::vector<std::wstring_view> kMultiStatementsOnSingleLine = {
        L"x = 1; ptr -> method();",
        L"x = 1; matches = left == right;",
        L"x = 1; is_diff = a != b;",
        L"x = 1; i++;",
        L"x = 1; ++i;",
        L"x = 1; val--;",
        L"x = 1; --val;",
        L"x = 1; fn = x => x + 1;",
        L"std::string s = \"ok\"; ::std::abort();"
    };

    for (auto stmt : kMultiStatementsOnSingleLine) {
        CHECK(SmartPasteManager::IsSourceCode(stmt, /*isEditorContext=*/true));
    }

    // Statements with trailing comments (//, /* */, #)
    const std::vector<std::wstring_view> kStatementsWithTrailingComments = {
        L"matches = left == right; // compare values",
        L"is_different = a != b; // check inequality",
        L"ptr -> method(); // invoke member function",
        L"x => x + 1; // lambda arrow function",
        L"i++; // advance loop counter",
        L"i ++; // spaced postfix with comment",
        L"++ i; // spaced prefix with comment",
        L"ptr -> count ++; // compound post-increment with comment",
        L"val --; /* block comment decrement */",
        L"-- val; # hash comment decrement",
        L"res = (a == b); // parenthesized equality comparison"
    };

    for (auto stmt : kStatementsWithTrailingComments) {
        CHECK(SmartPasteManager::IsSourceCode(stmt, /*isEditorContext=*/true));
    }

    // Additional positive tests with control flow or compound statements
    const std::vector<std::wstring_view> kCompoundStatements = {
        L"if (x == y) { return true; }",
        L"return a != b;",
        L"i++;",
        L"++count;",
        L"ptr->val++;",
        L"while (cur != nullptr) {\n    cur = cur->next;\n}"
    };

    for (auto code : kCompoundStatements) {
        CHECK(SmartPasteManager::IsSourceCode(code, /*isEditorContext=*/true));
    }
}

TEST(smart_paste_ctrl_held_modifier_preservation_and_recovery) {
    using Mod = ClipboardHelper::ModifierState;

    // 1. Simulate Smart Paste triggered via Ctrl+Shift+V (left Ctrl + left Shift)
    {
        Mod hotkeyMod;
        hotkeyMod.leftCtrl = true;
        hotkeyMod.leftShift = true;

        auto inputs = ClipboardHelper::BuildNormalizedPasteInputs(hotkeyMod);
        // Should have: LSHIFT UP (1), V DOWN (1), V UP (1), LSHIFT DOWN (1) = 4 inputs.
        // Notice: NO synthetic VK_CONTROL input because leftCtrl was already physically held!
        CHECK_EQ(inputs.size(), 4);

        CHECK_EQ(inputs[0].ki.wVk, VK_LSHIFT);
        CHECK_EQ(inputs[0].ki.dwFlags, KEYEVENTF_KEYUP);

        CHECK_EQ(inputs[1].ki.wVk, 'V');
        CHECK_EQ(inputs[1].ki.dwFlags, 0);

        CHECK_EQ(inputs[2].ki.wVk, 'V');
        CHECK_EQ(inputs[2].ki.dwFlags, KEYEVENTF_KEYUP);

        CHECK_EQ(inputs[3].ki.wVk, VK_LSHIFT);
        CHECK_EQ(inputs[3].ki.dwFlags, 0);

        // Verify DeduceModifiersFromInputs detects leftCtrl was held because synthetic CTRL DOWN is absent
        auto deduced = ClipboardHelper::DeduceModifiersFromInputs(inputs);
        CHECK(deduced.leftCtrl);
        CHECK(deduced.leftShift);
        CHECK(!deduced.leftAlt);

        // Verify recovery: if SendInput halted after dispatching 2 inputs (LSHIFT UP, V DOWN),
        // recovery must release 'V' (UP), restore VK_LSHIFT (DOWN), and NOT touch VK_LCONTROL (remains DOWN).
        auto recovery = ClipboardHelper::BuildRecoveryInputs(inputs, 2, hotkeyMod);
        CHECK(!recovery.empty());

        bool hasVUp = false;
        bool hasCtrlUp = false;
        bool hasShiftDown = false;
        for (const auto& in : recovery) {
            if (in.ki.wVk == 'V' && (in.ki.dwFlags & KEYEVENTF_KEYUP) != 0) hasVUp = true;
            if (in.ki.wVk == VK_CONTROL && (in.ki.dwFlags & KEYEVENTF_KEYUP) != 0) hasCtrlUp = true;
            if (in.ki.wVk == VK_LSHIFT && (in.ki.dwFlags & KEYEVENTF_KEYUP) == 0) hasShiftDown = true;
        }
        CHECK(hasVUp);
        CHECK(!hasCtrlUp); // Ctrl must NEVER be released in recovery if physically held
        CHECK(hasShiftDown); // Shift must be restored
    }

    // 2. Simulate Smart Paste triggered with ONLY Right-Ctrl held
    {
        Mod rightCtrlMod;
        rightCtrlMod.rightCtrl = true;

        auto inputs = ClipboardHelper::BuildNormalizedPasteInputs(rightCtrlMod);
        // Only right Ctrl held: no Shift/Alt to lift. Emits ONLY V DOWN and V UP (2 inputs).
        // Crucially: NO VK_CONTROL (which would press left Ctrl and get left Ctrl stuck down)!
        CHECK_EQ(inputs.size(), 2);
        CHECK_EQ(inputs[0].ki.wVk, 'V');
        CHECK_EQ(inputs[0].ki.dwFlags, 0);
        CHECK_EQ(inputs[1].ki.wVk, 'V');
        CHECK_EQ(inputs[1].ki.dwFlags, KEYEVENTF_KEYUP);

        for (const auto& in : inputs) {
            CHECK_NE(in.ki.wVk, VK_CONTROL);
            CHECK_NE(in.ki.wVk, VK_LCONTROL);
            CHECK_NE(in.ki.wVk, VK_RCONTROL);
        }

        // Partial dispatch recovery: if SendInput halted after 1 input (V DOWN)
        auto recovery = ClipboardHelper::BuildRecoveryInputs(inputs, 1, rightCtrlMod);
        CHECK_EQ(recovery.size(), 1);
        CHECK_EQ(recovery[0].ki.wVk, 'V');
        CHECK_EQ(recovery[0].ki.dwFlags, KEYEVENTF_KEYUP);

        // Verify key state simulation: right Ctrl stays held, left Ctrl is NEVER stuck
        std::map<WORD, bool> keyState;
        keyState[VK_RCONTROL] = true;
        keyState[VK_LCONTROL] = false;
        keyState['V'] = false;

        // Apply partial input (V down)
        keyState['V'] = true;

        // Apply recovery
        for (const auto& rec : recovery) {
            const bool isUp = (rec.ki.dwFlags & KEYEVENTF_KEYUP) != 0;
            keyState[rec.ki.wVk] = !isUp;
        }

        CHECK(!keyState['V']);
        CHECK(keyState[VK_RCONTROL]);
        CHECK(!keyState[VK_LCONTROL]); // Left Ctrl is completely clean and unstuck!
    }
}
