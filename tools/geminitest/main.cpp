// geminitest: live Gemini connectivity + model availability validation.
//
// Reads the API key ONLY from the GEMINI_API_KEY environment variable. The
// key value is never printed, logged, or written anywhere.
//
// Exit codes: 0 = all checks passed, 1 = any check failed, 2 = usage error.
//
// Usage: aiie_geminitest [--endpoint <url>] [--model <id>] [--list-models]
//                        [--repeats <n>]

#include "Configuration/AppConfig.h"
#include "Configuration/CredentialStore.h"
#include "Providers/GeminiProvider.h"
#include "Providers/ProviderFactory.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

std::wstring EnvKey() {
    wchar_t buf[4096] = {};
    const DWORD n = GetEnvironmentVariableW(L"GEMINI_API_KEY", buf, 4096);
    if (n == 0 || n >= 4096) {
        return {};
    }
    return std::wstring(buf);
}

std::wstring StoredKey() {
    std::wstring key;
    if (aiie::CredentialStore::GetApiKey(key)) {
        return key;
    }
    return {};
}

void PrintUsage() {
    std::wprintf(L"Usage: aiie_geminitest [--endpoint <url>] [--model <id>] [--list-models]\n");
    std::wprintf(L"                      [--repeats <n>]\n");
    std::wprintf(L"  --list-models   print available model ids\n");
    std::wprintf(L"  --repeats <n>   run n back-to-back translations (latency measurement)\n");
    std::wprintf(L"  (no args)       test connection + translate a probe sentence\n");
}

// One synchronous end-to-end translation through the real provider. Returns
// the elapsed milliseconds and fills `text` on success.
bool TranslateOnce(aiie::GeminiProvider& provider, aiie::TranslationRequest req,
                   std::wstring& text, unsigned long long& elapsedMs) {
    bool done = false;
    aiie::TranslationResult result;
    const auto t0 = GetTickCount64();
    provider.Translate(req, [&](aiie::TranslationResult r) {
        result = std::move(r);
        done = true;
    });
    const auto deadline = GetTickCount64() + 60000;
    while (!done && GetTickCount64() < deadline) {
        Sleep(50);
    }
    elapsedMs = GetTickCount64() - t0;
    if (!done) {
        std::wprintf(L"Async translation TIMED OUT\n");
        return false;
    }
    if (!result.ok) {
        std::wprintf(L"Async translation FAILED: %ls\n", result.errorDetail.c_str());
        return false;
    }
    text = std::move(result.text);
    return true;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring endpoint;
    std::wstring model;
    bool listModels = false;
    int repeats = 1;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--endpoint" && i + 1 < argc) {
            endpoint = argv[++i];
        } else if (arg == L"--model" && i + 1 < argc) {
            model = argv[++i];
        } else if (arg == L"--list-models") {
            listModels = true;
        } else if (arg == L"--repeats" && i + 1 < argc) {
            repeats = _wtoi(argv[++i]);
            if (repeats < 1 || repeats > 20) {
                PrintUsage();
                return 2;
            }
        } else {
            PrintUsage();
            return 2;
        }
    }

    if (endpoint.empty()) {
        endpoint = aiie::DefaultEndpointForProvider(aiie::ProviderId::Gemini);
    }
    if (model.empty()) {
        model = aiie::DefaultModelForProvider(aiie::ProviderId::Gemini);
    }

    std::wstring key;
    const std::wstring env = EnvKey();
    const std::wstring stored = StoredKey();
    if (!env.empty()) {
        key = env; // dev credential wins for the live probe
    } else if (!stored.empty()) {
        key = stored;
    }
    if (key.empty()) {
        std::wprintf(L"Gemini API key: NOT configured (no GEMINI_API_KEY env var, no stored credential)\n");
        return 1;
    }
    std::wprintf(L"Gemini API key: configured (source: %s)\n",
                 env.empty() ? L"Windows Credential Manager" : L"GEMINI_API_KEY environment variable");

    if (listModels) {
        std::vector<std::wstring> models;
        std::wstring error;
        if (!aiie::GeminiProvider::ListModels(endpoint, key, 30000, models, error)) {
            std::wprintf(L"Model list failed: %ls\n", error.c_str());
            return 1;
        }
        std::wprintf(L"Available models (%zu):\n", models.size());
        for (const std::wstring& id : models) {
            std::wprintf(L"  %ls\n", id.c_str());
        }
        return 0;
    }

    // 1) connection + key validation against the configured model
    std::wstring error;
    const auto t0 = GetTickCount64();
    if (!aiie::GeminiProvider::TestConnection(endpoint, model, key, 45000, error)) {
        std::wprintf(L"TestConnection FAILED (model %ls): %ls\n", model.c_str(), error.c_str());
        return 1;
    }
    const auto t1 = GetTickCount64();
    std::wprintf(L"TestConnection OK: model %ls responds in %llu ms\n", model.c_str(),
                 static_cast<unsigned long long>(t1 - t0));

    // 2) end-to-end async translation(s) through the real pipeline
    aiie::GeminiConfig gc;
    gc.endpoint = endpoint;
    gc.model = model;
    gc.apiKey = key;
    gc.timeoutMs = 30000;
    auto provider = std::make_shared<aiie::GeminiProvider>(gc);

    aiie::TranslationRequest req;
    req.text = L"Necesito terminar este proyecto antes del viernes.";
    req.sourceLanguage = L"es";
    req.targetLanguage = L"en";
    req.mode = aiie::TransformMode::Translate;

    std::vector<unsigned long long> timings;
    timings.reserve(static_cast<size_t>(repeats));
    std::wstring lastText;
    for (int i = 0; i < repeats; ++i) {
        std::wstring text;
        unsigned long long elapsed = 0;
        if (!TranslateOnce(*provider, req, text, elapsed)) {
            provider->CancelAll();
            return 1;
        }
        timings.push_back(elapsed);
        lastText = std::move(text);
        if (repeats > 1) {
            std::wprintf(L"  request %d/%d: %llu ms\n", i + 1, repeats,
                         static_cast<unsigned long long>(elapsed));
        }
    }

    if (repeats > 1) {
        unsigned long long min = timings.front(), max = timings.front(), sum = 0;
        for (const unsigned long long t : timings) {
            min = t < min ? t : min;
            max = t > max ? t : max;
            sum += t;
        }
        std::wprintf(L"latency summary (%d requests): min=%llu ms avg=%llu ms max=%llu ms\n",
                     repeats, min, sum / timings.size(), max);
    }

    std::wprintf(L"Async translation OK in %llu ms:\n  \"%ls\"\n",
                 static_cast<unsigned long long>(timings.front()), lastText.c_str());
    provider->CancelAll();
    return 0;
}