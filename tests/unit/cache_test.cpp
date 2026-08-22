#include "mini_test.h"

#include "Translation/TranslationCache.h"

using namespace aiie;

namespace {
TranslationRequest MakeRequest(std::wstring text,
                               ProviderId prov = ProviderId::OpenAI,
                               std::wstring model = L"gpt-5.6-luna",
                               TransformOperation op = TransformOperation::Translate,
                               std::wstring src = L"es",
                               std::wstring tgt = L"en",
                               TranslationProfile profile = TranslationProfile::Neutral,
                               std::wstring custom = L"") {
    TranslationRequest r;
    r.text = std::move(text);
    r.providerId = prov;
    r.model = std::move(model);
    r.operation = op;
    r.sourceLanguage = std::move(src);
    r.targetLanguage = std::move(tgt);
    r.profile = profile;
    r.customInstructions = std::move(custom);
    return r;
}
} // namespace

TEST(cache_isolation_matrix) {
    TranslationCache cache(100);

    // Case A: OpenAI + Translate + ES->EN + Neutral + "hola"
    const auto caseA = MakeRequest(L"hola", ProviderId::OpenAI, L"gpt-5.6-luna",
                                   TransformOperation::Translate, L"es", L"en", TranslationProfile::Neutral);

    // Case B: Gemini + Translate + ES->EN + Neutral + "hola"
    const auto caseB = MakeRequest(L"hola", ProviderId::Gemini, L"gemini-3.6-flash",
                                   TransformOperation::Translate, L"es", L"en", TranslationProfile::Neutral);

    // Case C: OpenAI + CorrectAndRewrite + ES->ES + Neutral + "hola"
    const auto caseC = MakeRequest(L"hola", ProviderId::OpenAI, L"gpt-5.6-luna",
                                   TransformOperation::CorrectAndRewrite, L"es", L"es", TranslationProfile::Neutral);

    // Case D: OpenAI + Translate + ES->EN + Casual + "hola"
    const auto caseD = MakeRequest(L"hola", ProviderId::OpenAI, L"gpt-5.6-luna",
                                   TransformOperation::Translate, L"es", L"en", TranslationProfile::Casual);

    // Case E: OpenAI + Translate + ES->EN + Neutral + custom prompt
    const auto caseE = MakeRequest(L"hola", ProviderId::OpenAI, L"gpt-5.6-luna",
                                   TransformOperation::Translate, L"es", L"en", TranslationProfile::Neutral, L"custom tone");

    cache.Store(caseA, L"hello-A");

    // Must be a hit for exact Case A
    const std::wstring* hitA = cache.Lookup(caseA);
    CHECK(hitA != nullptr);
    CHECK_STREQ(*hitA, L"hello-A");

    // All others MUST be isolated cache misses!
    CHECK(cache.Lookup(caseB) == nullptr);
    CHECK(cache.Lookup(caseC) == nullptr);
    CHECK(cache.Lookup(caseD) == nullptr);
    CHECK(cache.Lookup(caseE) == nullptr);

    // Store Case B, Case C, Case D, Case E with distinct results:
    cache.Store(caseB, L"hello-B");
    cache.Store(caseC, L"Hola-C");
    cache.Store(caseD, L"hey-D");
    cache.Store(caseE, L"custom-E");

    // Verify all return their own unique results without cross-pollution:
    CHECK_STREQ(*cache.Lookup(caseA), L"hello-A");
    CHECK_STREQ(*cache.Lookup(caseB), L"hello-B");
    CHECK_STREQ(*cache.Lookup(caseC), L"Hola-C");
    CHECK_STREQ(*cache.Lookup(caseD), L"hey-D");
    CHECK_STREQ(*cache.Lookup(caseE), L"custom-E");
}

TEST(cache_evicts_oldest) {
    TranslationCache cache(2);
    const auto a = MakeRequest(L"aaa");
    const auto b = MakeRequest(L"bbb");
    const auto c = MakeRequest(L"ccc");
    cache.Store(a, L"1");
    cache.Store(b, L"2");
    cache.Store(c, L"3");
    CHECK(cache.Lookup(a) == nullptr); // evicted (oldest)
    CHECK(cache.Lookup(b) != nullptr);
    CHECK(cache.Lookup(c) != nullptr);
    CHECK_EQ(cache.Size(), 2u);
}

TEST(cache_disabled) {
    TranslationCache cache(100);
    cache.Configure(false, 100);
    cache.Store(MakeRequest(L"hola"), L"hello");
    CHECK(cache.Lookup(MakeRequest(L"hola")) == nullptr);
    CHECK_EQ(cache.Size(), 0u);
}

TEST(cache_clear) {
    TranslationCache cache(100);
    cache.Store(MakeRequest(L"hola"), L"hello");
    cache.Clear();
    CHECK_EQ(cache.Size(), 0u);
    CHECK(cache.Lookup(MakeRequest(L"hola")) == nullptr);
}

TEST(cache_does_not_store_empty_text) {
    TranslationCache cache(100);
    cache.Store(MakeRequest(L""), L"hello");
    CHECK_EQ(cache.Size(), 0u);
}