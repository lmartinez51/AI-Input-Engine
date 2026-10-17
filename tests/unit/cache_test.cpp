#include "mini_test.h"

#include "Translation/TranslationCache.h"

#include <chrono>
#include <thread>

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
    const auto hitA = cache.Lookup(caseA);
    CHECK(hitA.has_value());
    CHECK_STREQ(hitA->c_str(), L"hello-A");

    // All others MUST be isolated cache misses!
    CHECK(!cache.Lookup(caseB).has_value());
    CHECK(!cache.Lookup(caseC).has_value());
    CHECK(!cache.Lookup(caseD).has_value());
    CHECK(!cache.Lookup(caseE).has_value());

    // Store Case B, Case C, Case D, Case E with distinct results:
    cache.Store(caseB, L"hello-B");
    cache.Store(caseC, L"Hola-C");
    cache.Store(caseD, L"hey-D");
    cache.Store(caseE, L"custom-E");

    // Verify all return their own unique results without cross-pollution:
    CHECK_STREQ(cache.Lookup(caseA)->c_str(), L"hello-A");
    CHECK_STREQ(cache.Lookup(caseB)->c_str(), L"hello-B");
    CHECK_STREQ(cache.Lookup(caseC)->c_str(), L"Hola-C");
    CHECK_STREQ(cache.Lookup(caseD)->c_str(), L"hey-D");
    CHECK_STREQ(cache.Lookup(caseE)->c_str(), L"custom-E");
}

TEST(cache_evicts_oldest) {
    TranslationCache cache(2);
    const auto a = MakeRequest(L"aaa");
    const auto b = MakeRequest(L"bbb");
    const auto c = MakeRequest(L"ccc");
    cache.Store(a, L"1");
    cache.Store(b, L"2");
    cache.Store(c, L"3");
    CHECK(!cache.Lookup(a).has_value()); // evicted (oldest)
    CHECK(cache.Lookup(b).has_value());
    CHECK(cache.Lookup(c).has_value());
    CHECK_EQ(cache.Size(), 2u);
}

TEST(cache_lru_recency_refreshed_on_lookup) {
    TranslationCache cache(2);
    const auto a = MakeRequest(L"aaa");
    const auto b = MakeRequest(L"bbb");
    const auto c = MakeRequest(L"ccc");
    cache.Store(a, L"1");
    cache.Store(b, L"2");

    // Access 'a' so 'a' becomes MRU and 'b' becomes LRU
    const auto hitA = cache.Lookup(a);
    CHECK(hitA.has_value());
    CHECK_STREQ(hitA->c_str(), L"1");

    // Inserting 'c' should evict 'b' (LRU), keeping 'a' (MRU)
    cache.Store(c, L"3");
    CHECK(cache.Lookup(a).has_value());
    CHECK(!cache.Lookup(b).has_value());
    CHECK(cache.Lookup(c).has_value());
    CHECK_EQ(cache.Size(), 2u);
}

TEST(cache_configure_shrink_evicts_lru) {
    TranslationCache cache(4);
    cache.Store(MakeRequest(L"1"), L"val1");
    cache.Store(MakeRequest(L"2"), L"val2");
    cache.Store(MakeRequest(L"3"), L"val3");
    cache.Store(MakeRequest(L"4"), L"val4");
    CHECK_EQ(cache.Size(), 4u);

    // Access '1' to make it MRU
    cache.Lookup(MakeRequest(L"1"));

    // Shrink max capacity to 2 -> should evict LRU items ('2' and '3')
    cache.Configure(true, 2);
    CHECK_EQ(cache.Size(), 2u);
    CHECK(cache.Lookup(MakeRequest(L"1")).has_value());
    CHECK(!cache.Lookup(MakeRequest(L"2")).has_value());
    CHECK(!cache.Lookup(MakeRequest(L"3")).has_value());
    CHECK(cache.Lookup(MakeRequest(L"4")).has_value());
}

TEST(cache_disabled) {
    TranslationCache cache(100);
    cache.Configure(false, 100);
    cache.Store(MakeRequest(L"hola"), L"hello");
    CHECK(!cache.Lookup(MakeRequest(L"hola")).has_value());
    CHECK_EQ(cache.Size(), 0u);
}

TEST(cache_clear) {
    TranslationCache cache(100);
    cache.Store(MakeRequest(L"hola"), L"hello");
    cache.Clear();
    CHECK_EQ(cache.Size(), 0u);
    CHECK(!cache.Lookup(MakeRequest(L"hola")).has_value());
}

TEST(cache_does_not_store_empty_text) {
    TranslationCache cache(100);
    cache.Store(MakeRequest(L""), L"hello");
    CHECK_EQ(cache.Size(), 0u);
}

TEST(cache_hits_and_misses_metrics) {
    TranslationCache cache(10);
    const auto req = MakeRequest(L"hola");
    CHECK_EQ(cache.Hits(), 0u);
    CHECK_EQ(cache.Misses(), 0u);

    cache.Lookup(req);
    CHECK_EQ(cache.Misses(), 1u);
    CHECK_EQ(cache.Hits(), 0u);

    cache.Store(req, L"hello");
    cache.Lookup(req);
    CHECK_EQ(cache.Hits(), 1u);
    CHECK_EQ(cache.Misses(), 1u);
}

TEST(cache_ttl_expiration_on_lookup) {
    // 1-second TTL
    TranslationCache cache(10, std::chrono::seconds(1));
    const auto req = MakeRequest(L"hola");
    cache.Store(req, L"hello");

    CHECK_EQ(cache.Size(), 1u);
    const auto hitImmediate = cache.Lookup(req);
    CHECK(hitImmediate.has_value());
    CHECK_STREQ(hitImmediate->c_str(), L"hello");

    // Sleep past the 1-second TTL
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    // Lookup should detect expiration, purge the entry in O(1), and register a miss
    const auto hitAfterExpiry = cache.Lookup(req);
    CHECK(!hitAfterExpiry.has_value());
    CHECK_EQ(cache.Size(), 0u);
    CHECK_EQ(cache.Hits(), 1u);
    CHECK_EQ(cache.Misses(), 1u);
}

TEST(cache_evicts_expired_from_back_on_store) {
    // 1-second TTL
    TranslationCache cache(10, std::chrono::seconds(1));
    const auto reqA = MakeRequest(L"primero");
    const auto reqB = MakeRequest(L"segundo");

    cache.Store(reqA, L"first");
    CHECK_EQ(cache.Size(), 1u);

    // Sleep past the 1-second TTL
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    // Storing B should trigger opportunistic eviction of expired entries from the back
    cache.Store(reqB, L"second");
    CHECK_EQ(cache.Size(), 1u);

    CHECK(!cache.Lookup(reqA).has_value());
    const auto hitB = cache.Lookup(reqB);
    CHECK(hitB.has_value());
    CHECK_STREQ(hitB->c_str(), L"second");
}

TEST(cache_ttl_disabled_when_zero_preserves_entries) {
    // TTL = 0 means disabled / infinite lifetime
    TranslationCache cache(10, std::chrono::seconds(0));
    const auto req = MakeRequest(L"infinito");
    cache.Store(req, L"infinite");

    CHECK_EQ(cache.Size(), 1u);
    CHECK(cache.Lookup(req).has_value());

    // Sleep 1100ms: with TTL=0, entry must NOT expire
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    CHECK_EQ(cache.Size(), 1u);
    CHECK(cache.Lookup(req).has_value());
}

TEST(cache_ttl_overwrite_restarts_lifetime) {
    // 1-second TTL
    TranslationCache cache(10, std::chrono::seconds(1));
    const auto req = MakeRequest(L"renovar");
    cache.Store(req, L"version 1");

    // After 600ms, overwrite entry
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    cache.Store(req, L"version 2");

    // Sleep another 600ms: total 1200ms since initial creation, but only 600ms since overwrite
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    const auto hit = cache.Lookup(req);
    CHECK(hit.has_value());
    CHECK_STREQ(hit->c_str(), L"version 2");

    // Sleep past the overwrite's 1-second TTL (500ms more -> 1100ms from overwrite)
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(!cache.Lookup(req).has_value());
}

TEST(cache_lookup_does_not_extend_ttl) {
    // 1-second TTL
    TranslationCache cache(10, std::chrono::seconds(1));
    const auto req = MakeRequest(L"consulta");
    cache.Store(req, L"result");

    // Access at 600ms (refreshes LRU recency for capacity, but must NOT extend TTL)
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    CHECK(cache.Lookup(req).has_value());

    // Sleep another 500ms (total 1100ms since creation) -> entry must expire
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(!cache.Lookup(req).has_value());
}

TEST(cache_payload_size_limit_rejection) {
    // Max text length: 50 characters
    TranslationCache cache(10, std::chrono::seconds(1800), 50);

    const auto smallReq = MakeRequest(L"Corta frase de prueba.");
    const std::wstring hugeText(200, L'x');
    const auto hugeReq = MakeRequest(hugeText);

    cache.Store(smallReq, L"Short test phrase.");
    CHECK_EQ(cache.Size(), 1u);

    // Huge request must be rejected from cache storage to protect RAM footprint
    cache.Store(hugeReq, L"Huge translation");
    CHECK_EQ(cache.Size(), 1u);
    CHECK(!cache.Lookup(hugeReq).has_value());

    // Short request with huge result must also be rejected on insertion
    const auto reqWithHugeResult = MakeRequest(L"Otra frase corta.");
    cache.Store(reqWithHugeResult, hugeText);
    CHECK_EQ(cache.Size(), 1u);
    CHECK(!cache.Lookup(reqWithHugeResult).has_value());

    // If an existing entry is updated with an oversized result, it must be rejected and purged
    cache.Store(smallReq, hugeText);
    CHECK_EQ(cache.Size(), 0u);
    CHECK(!cache.Lookup(smallReq).has_value());
}

TEST(cache_payload_size_unlimited_when_zero) {
    // Max text length 0 means unlimited
    TranslationCache cache(10, std::chrono::seconds(1800), 0);

    const std::wstring hugeText(200, L'y');
    const auto hugeReq = MakeRequest(hugeText);

    cache.Store(hugeReq, L"Unlimited huge");
    CHECK_EQ(cache.Size(), 1u);
    CHECK(cache.Lookup(hugeReq).has_value());
}

TEST(cache_configure_shrink_payload_limit_purges_oversized) {
    // Start with max text length 200
    TranslationCache cache(10, std::chrono::seconds(1800), 200);

    const auto reqCompliant = MakeRequest(L"Frase corta."); // ~12 chars
    const auto reqOversizedText = MakeRequest(std::wstring(80, L'a')); // 80 chars
    const auto reqOversizedResult = MakeRequest(L"Frase con resultado largo."); // ~26 chars

    cache.Store(reqCompliant, L"Short result.");
    cache.Store(reqOversizedText, L"Short translation.");
    cache.Store(reqOversizedResult, std::wstring(90, L'b')); // 90 chars result

    CHECK_EQ(cache.Size(), 3u);
    CHECK(cache.Lookup(reqCompliant).has_value());
    CHECK(cache.Lookup(reqOversizedText).has_value());
    CHECK(cache.Lookup(reqOversizedResult).has_value());

    // Lower payload limit to 50: both oversized request (80 > 50) and oversized result (90 > 50) must be purged
    cache.Configure(true, 10, std::chrono::seconds(1800), 50);

    CHECK_EQ(cache.Size(), 1u);
    CHECK(cache.Lookup(reqCompliant).has_value());
    CHECK(!cache.Lookup(reqOversizedText).has_value());
    CHECK(!cache.Lookup(reqOversizedResult).has_value());
}