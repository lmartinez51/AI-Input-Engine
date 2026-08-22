#include "mini_test.h"

#include "Configuration/AppConfig.h"
#include "Providers/ProviderFactory.h"

using namespace aiie;

TEST(provider_factory_mock_selection) {
    AppConfig cfg;
    cfg.provider = ProviderId::Mock;
    auto p = ProviderFactory::Create(cfg);
    CHECK(p != nullptr);
    CHECK_STREQ(p->Name(), L"mock");
    CHECK(!p->RequiresNetwork());
}

TEST(provider_factory_gemini_selection) {
    AppConfig cfg;
    cfg.provider = ProviderId::Gemini;
    cfg.model = L"gemini-3.6-flash";
    auto p = ProviderFactory::Create(cfg);
    CHECK(p != nullptr);
    CHECK_STREQ(p->Name(), L"gemini");
    CHECK(p->RequiresNetwork());
}

TEST(provider_factory_openai_selection) {
    AppConfig cfg;
    cfg.provider = ProviderId::OpenAI;
    cfg.model = L"gpt-5.6-luna";
    auto p = ProviderFactory::Create(cfg);
    CHECK(p != nullptr);
    CHECK_STREQ(p->Name(), L"openai");
    CHECK(p->RequiresNetwork());
}

TEST(provider_factory_no_silent_fallback) {
    // Selecting Gemini or OpenAI must never yield the Mock provider, even when no key
    // is configured (the provider itself reports the missing key later).
    {
        AppConfig cfg;
        cfg.provider = ProviderId::Gemini;
        auto p = ProviderFactory::Create(cfg);
        CHECK(p != nullptr);
        CHECK_STREQ(p->Name(), L"gemini");
    }
    {
        AppConfig cfg;
        cfg.provider = ProviderId::OpenAI;
        auto p = ProviderFactory::Create(cfg);
        CHECK(p != nullptr);
        CHECK_STREQ(p->Name(), L"openai");
    }
}

TEST(provider_name_and_parse) {
    CHECK_STREQ(ProviderName(ProviderId::Mock), L"mock");
    CHECK_STREQ(ProviderName(ProviderId::Gemini), L"gemini");
    CHECK_STREQ(ProviderName(ProviderId::OpenAI), L"openai");

    ProviderId id = ProviderId::Mock;
    CHECK(TryParseProvider(L"mock", id));
    CHECK_EQ(id, ProviderId::Mock);
    CHECK(TryParseProvider(L"gemini", id));
    CHECK_EQ(id, ProviderId::Gemini);
    CHECK(TryParseProvider(L"openai", id));
    CHECK_EQ(id, ProviderId::OpenAI);
    CHECK(!TryParseProvider(L"nope", id));
}