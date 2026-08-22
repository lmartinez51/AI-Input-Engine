// openaitest: live OpenAI connectivity, multilingual translation, and rewrite validation.
//
// Reads the API key from the OPENAI_API_KEY environment variable or Windows
// Credential Manager. The key value is never printed, logged, or written anywhere.
//
// Usage:
//   aiie_openaitest                      (runs single ES->EN probe)
//   aiie_openaitest --matrix             (runs the strict minimal multilingual + rewrite live matrix)

#include "Configuration/AppConfig.h"
#include "Configuration/CredentialStore.h"
#include "Configuration/LanguageRegistry.h"
#include "Providers/OpenAIProvider.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

std::wstring EnvKey() {
    wchar_t buf[4096] = {};
    const DWORD n = GetEnvironmentVariableW(L"OPENAI_API_KEY", buf, 4096);
    if (n == 0 || n >= 4096) {
        return {};
    }
    return std::wstring(buf);
}

std::wstring StoredKey() {
    std::wstring key;
    if (aiie::CredentialStore::GetOpenAIApiKey(key)) {
        return key;
    }
    return {};
}

struct ProbeResult {
    bool ok = false;
    std::wstring text;
    std::wstring error;
    int64_t latencyMs = 0;
};

ProbeResult RunCustomProbe(aiie::OpenAIProvider& provider,
                           aiie::TransformOperation op,
                           const std::wstring& src,
                           const std::wstring& tgt,
                           aiie::TranslationProfile profile,
                           const std::wstring& text) {
    ProbeResult r;
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const auto tStart = std::chrono::steady_clock::now();

    aiie::TranslationRequest req;
    req.text = text;
    req.operation = op;
    req.sourceLanguage = src;
    req.targetLanguage = tgt;
    req.profile = profile;
    req.requestId = 1;

    provider.Translate(std::move(req), [&r, &tStart, event](aiie::TranslationResult res) {
        const auto tEnd = std::chrono::steady_clock::now();
        r.latencyMs = std::chrono::duration_cast<std::chrono::milliseconds>(tEnd - tStart).count();
        r.ok = res.ok;
        r.text = std::move(res.text);
        r.error = std::move(res.error);
        SetEvent(event);
    });

    WaitForSingleObject(event, 60000);
    CloseHandle(event);
    return r;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);

    bool runMatrix = false;
    std::wstring endpoint = aiie::DefaultEndpointForProvider(aiie::ProviderId::OpenAI);
    std::wstring model = aiie::DefaultModelForProvider(aiie::ProviderId::OpenAI);

    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg(argv[i]);
        if (arg == L"--matrix") {
            runMatrix = true;
        } else if (arg == L"--endpoint" && i + 1 < argc) {
            endpoint = argv[++i];
        } else if (arg == L"--model" && i + 1 < argc) {
            model = argv[++i];
        }
    }

    std::wstring key;
    const std::wstring env = EnvKey();
    const std::wstring stored = StoredKey();
    std::wstring keySource;
    if (!stored.empty()) {
        key = stored;
        keySource = L"Windows Credential Manager";
    } else if (!env.empty()) {
        key = env;
        keySource = L"OPENAI_API_KEY environment variable";
    }

    if (key.empty()) {
        std::wprintf(L"OpenAI API key: NOT configured (no OPENAI_API_KEY env var, no stored credential)\n");
        return 1;
    }

    std::wprintf(L"OpenAI API key: configured (source: %ls)\n", keySource.c_str());
    std::wprintf(L"Endpoint: %ls\n", endpoint.c_str());
    std::wprintf(L"Model:    %ls\n", model.c_str());

    aiie::OpenAIConfig cfg;
    cfg.endpoint = endpoint;
    cfg.model = model;
    cfg.apiKey = key;
    cfg.timeoutMs = 45000;
    cfg.maxRetries = 2;

    aiie::OpenAIProvider provider(std::move(cfg));

    if (!runMatrix) {
        std::wprintf(L"\n--- Testing single live probe (ES -> EN) ---\n");
        const auto r = RunCustomProbe(provider, aiie::TransformOperation::Translate,
                                      L"es", L"en", aiie::TranslationProfile::Neutral,
                                      L"Hola mundo, este proyecto es importante.");
        if (r.ok) {
            std::wprintf(L"OK (%lld ms): %ls\n", r.latencyMs, r.text.c_str());
            return 0;
        } else {
            std::wprintf(L"FAIL (%lld ms): %ls\n", r.latencyMs, r.error.c_str());
            return 1;
        }
    }

    std::wprintf(L"\n============================================================\n");
    std::wprintf(L"LIVE OPENAI TEST MATRIX (STRICT MINIMAL BUDGET)\n");
    std::wprintf(L"============================================================\n");

    struct TestCase {
        std::wstring name;
        aiie::TransformOperation op;
        std::wstring src;
        std::wstring tgt;
        aiie::TranslationProfile profile;
        std::wstring input;
    };

    const std::vector<TestCase> testCases = {
        // Translation tests
        {L"1. Translate ES -> EN", aiie::TransformOperation::Translate, L"es", L"en",
         aiie::TranslationProfile::Neutral, L"Hola mundo, este proyecto es importante."},
        {L"2. Translate ES -> zh-Hans", aiie::TransformOperation::Translate, L"es", L"zh-Hans",
         aiie::TranslationProfile::Neutral, L"Hola mundo."},
        {L"3. Translate zh-Hans -> ES", aiie::TransformOperation::Translate, L"zh-Hans", L"es",
         aiie::TranslationProfile::Neutral, L"你好，世界。"},
        {L"4. Translate EN -> zh-Hant", aiie::TransformOperation::Translate, L"en", L"zh-Hant",
         aiie::TranslationProfile::Neutral, L"Hello world."},

        // CorrectAndRewrite tests
        {L"5. CorrectAndRewrite Spanish Neutral", aiie::TransformOperation::CorrectAndRewrite, L"es", L"es",
         aiie::TranslationProfile::Neutral, L"Ola ayer fui al super aver si comprava leche pero no avia."},
        {L"6. CorrectAndRewrite English Professional", aiie::TransformOperation::CorrectAndRewrite, L"en", L"en",
         aiie::TranslationProfile::Professional, L"i need to finish this project before friday"},
        {L"7. CorrectAndRewrite Chinese Simplified Casual", aiie::TransformOperation::CorrectAndRewrite, L"zh-Hans", L"zh-Hans",
         aiie::TranslationProfile::Casual, L"你好世界明天你能来吗"},
    };

    int passCount = 0;
    int requestCount = 0;

    for (const auto& tc : testCases) {
        ++requestCount;
        std::wprintf(L"\n[%ls]\n", tc.name.c_str());
        std::wprintf(L"  Input:  \"%ls\"\n", tc.input.c_str());
        const auto r = RunCustomProbe(provider, tc.op, tc.src, tc.tgt, tc.profile, tc.input);
        if (r.ok) {
            ++passCount;
            std::wprintf(L"  Output: \"%ls\" (OK, %lld ms)\n", r.text.c_str(), r.latencyMs);
        } else {
            std::wprintf(L"  Output: FAIL -> %ls (%lld ms)\n", r.error.c_str(), r.latencyMs);
        }
    }

    std::wprintf(L"\n============================================================\n");
    std::wprintf(L"RESULTS: %d / %d tests passed. Total live OpenAI requests consumed: %d\n",
                 passCount, static_cast<int>(testCases.size()), requestCount);
    std::wprintf(L"============================================================\n");

    return (passCount == static_cast<int>(testCases.size())) ? 0 : 1;
}
