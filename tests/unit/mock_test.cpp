#include "mini_test.h"

#include "Providers/MockProvider.h"

using namespace aiie;

namespace {
TranslationRequest MakeReq(std::wstring text, TransformOperation op, std::wstring src, std::wstring tgt,
                           TranslationProfile profile = TranslationProfile::Neutral,
                           std::wstring custom = L"") {
    TranslationRequest r;
    r.text = std::move(text);
    r.operation = op;
    r.sourceLanguage = std::move(src);
    r.targetLanguage = std::move(tgt);
    r.profile = profile;
    r.customInstructions = std::move(custom);
    return r;
}
} // namespace

// 1. Translate ES -> EN
TEST(mock_translate_es_to_en) {
    CHECK_STREQ(MockProvider::TranslateText(L"hola mundo", L"es", L"en"), L"hello world");
    CHECK_STREQ(MockProvider::TranslateText(L"necesito terminar", L"es", L"en"), L"I need to finish");
    CHECK_STREQ(MockProvider::TranslateText(L"Necesito terminar este proyecto antes del viernes.", L"es", L"en"),
                L"I need to finish this project before the Friday.");
}

// 2. Translate EN -> ES
TEST(mock_translate_en_to_es) {
    CHECK_STREQ(MockProvider::TranslateText(L"hello world", L"en", L"es"), L"hola mundo");
    CHECK_STREQ(MockProvider::TranslateText(L"this project", L"en", L"es"), L"este proyecto");
}

// 3. Translate ES -> zh-Hans
TEST(mock_translate_es_to_zh_hans) {
    CHECK_STREQ(MockProvider::TranslateText(L"hola mundo", L"es", L"zh-Hans"), L"你好，世界。");
    CHECK_STREQ(MockProvider::TranslateText(L"hola", L"es", L"zh-Hans"), L"你好。");
    CHECK_STREQ(MockProvider::TranslateText(L"este proyecto", L"es", L"zh-Hans"), L"这个项目很重要。");
}

// 4. Translate zh-Hans -> ES
TEST(mock_translate_zh_hans_to_es) {
    CHECK_STREQ(MockProvider::TranslateText(L"你好世界", L"zh-Hans", L"es"), L"Hola, mundo.");
    CHECK_STREQ(MockProvider::TranslateText(L"你好", L"zh-Hans", L"es"), L"Hola.");
}

// 5. Translate EN -> zh-Hant
TEST(mock_translate_en_to_zh_hant) {
    CHECK_STREQ(MockProvider::TranslateText(L"hello world", L"en", L"zh-Hant"), L"你好，世界。");
    CHECK_STREQ(MockProvider::TranslateText(L"hello", L"en", L"zh-Hant"), L"你好。");
}

// 6. Translate zh-Hant -> EN
TEST(mock_translate_zh_hant_to_en) {
    CHECK_STREQ(MockProvider::TranslateText(L"你好世界", L"zh-Hant", L"en"), L"Hello, world.");
    CHECK_STREQ(MockProvider::TranslateText(L"你好", L"zh-Hant", L"en"), L"Hello.");
}

// 7. Translate ES -> zh-Hant
TEST(mock_translate_es_to_zh_hant) {
    CHECK_STREQ(MockProvider::TranslateText(L"hola mundo", L"es", L"zh-Hant"), L"你好，世界。");
    CHECK_STREQ(MockProvider::TranslateText(L"este proyecto", L"es", L"zh-Hant"), L"這個項目很重要。");
}

// 8. Translate zh-Hant -> ES
TEST(mock_translate_zh_hant_to_es) {
    CHECK_STREQ(MockProvider::TranslateText(L"你好世界", L"zh-Hant", L"es"), L"Hola, mundo.");
}

// 9. CorrectAndRewrite ES Neutral
TEST(mock_rewrite_es_neutral) {
    const auto req = MakeReq(L"Ola ayer fui al super aver si comprava leche pero no avia.",
                             TransformOperation::CorrectAndRewrite, L"es", L"es", TranslationProfile::Neutral);
    CHECK_STREQ(MockProvider::Transform(req), L"Hola, ayer fui al súper a ver si compraba leche, pero no había.");

    const auto req2 = MakeReq(L"ola como estas", TransformOperation::CorrectAndRewrite, L"es", L"es", TranslationProfile::Neutral);
    CHECK_STREQ(MockProvider::Transform(req2), L"Hola, ¿cómo estás?");
}

// 10. CorrectAndRewrite ES Professional
TEST(mock_rewrite_es_professional) {
    const auto req = MakeReq(L"Que onda bro, ya llegaste?",
                             TransformOperation::CorrectAndRewrite, L"es", L"es", TranslationProfile::Professional);
    CHECK_STREQ(MockProvider::Transform(req), L"Hola, ¿ya has llegado?");
}

// 11. CorrectAndRewrite ES Casual
TEST(mock_rewrite_es_casual) {
    const auto req = MakeReq(L"ola como estas",
                             TransformOperation::CorrectAndRewrite, L"es", L"es", TranslationProfile::Casual);
    CHECK_STREQ(MockProvider::Transform(req), L"Hola, ¿cómo estás?");
}

// 12. CorrectAndRewrite ES Slang
TEST(mock_rewrite_es_slang) {
    const auto req = MakeReq(L"Que onda bro, ya llegaste?",
                             TransformOperation::CorrectAndRewrite, L"es", L"es", TranslationProfile::Slang);
    CHECK_STREQ(MockProvider::Transform(req), L"¿Qué onda, bro? ¿Ya llegaste?");
}

// 13. CorrectAndRewrite EN Neutral
TEST(mock_rewrite_en_neutral) {
    const auto req = MakeReq(L"hello how are you",
                             TransformOperation::CorrectAndRewrite, L"en", L"en", TranslationProfile::Neutral);
    CHECK_STREQ(MockProvider::Transform(req), L"Hello, how are you?");

    const auto req2 = MakeReq(L"i need to finish this project before friday",
                              TransformOperation::CorrectAndRewrite, L"en", L"en", TranslationProfile::Neutral);
    CHECK_STREQ(MockProvider::Transform(req2), L"I need to finish this project before Friday.");
}

// 14. CorrectAndRewrite EN Professional
TEST(mock_rewrite_en_professional) {
    const auto req = MakeReq(L"i need to finish this project before friday",
                             TransformOperation::CorrectAndRewrite, L"en", L"en", TranslationProfile::Professional);
    CHECK_STREQ(MockProvider::Transform(req), L"I must complete this project prior to Friday.");
}

// 15. CorrectAndRewrite EN Casual
TEST(mock_rewrite_en_casual) {
    const auto req = MakeReq(L"hello how are you",
                             TransformOperation::CorrectAndRewrite, L"en", L"en", TranslationProfile::Casual);
    CHECK_STREQ(MockProvider::Transform(req), L"Hello, how are you?");
}

// 16. CorrectAndRewrite zh-Hans Neutral
TEST(mock_rewrite_zh_hans_neutral) {
    const auto req = MakeReq(L"你好世界",
                             TransformOperation::CorrectAndRewrite, L"zh-Hans", L"zh-Hans", TranslationProfile::Neutral);
    CHECK_STREQ(MockProvider::Transform(req), L"你好，世界。");
}

// 17. CorrectAndRewrite zh-Hant Neutral
TEST(mock_rewrite_zh_hant_neutral) {
    const auto req = MakeReq(L"你好世界",
                             TransformOperation::CorrectAndRewrite, L"zh-Hant", L"zh-Hant", TranslationProfile::Neutral);
    CHECK_STREQ(MockProvider::Transform(req), L"你好，世界。");
}

// 18. Question preservation (do not answer questions)
TEST(mock_preserves_questions_never_answers) {
    const auto reqSpanish = MakeReq(L"puedes venir mañana?",
                                    TransformOperation::CorrectAndRewrite, L"es", L"es", TranslationProfile::Neutral);
    CHECK_STREQ(MockProvider::Transform(reqSpanish), L"¿Puedes venir mañana?");

    const auto reqEnglish = MakeReq(L"can you come tomorrow?",
                                    TransformOperation::CorrectAndRewrite, L"en", L"en", TranslationProfile::Neutral);
    CHECK_STREQ(MockProvider::Transform(reqEnglish), L"Can you come tomorrow?");

    const auto reqChinese = MakeReq(L"明天你能来吗",
                                    TransformOperation::CorrectAndRewrite, L"zh-Hans", L"zh-Hans", TranslationProfile::Neutral);
    CHECK_STREQ(MockProvider::Transform(reqChinese), L"明天你能来吗？");
}

// 19. Protected URL/technical token preservation
TEST(mock_preserves_protected_tokens) {
    const auto res = MockProvider::TranslateText(L"visita https://github.com/aiie o escribe a user@test.com #ai",
                                                 L"es", L"en");
    CHECK(res.find(L"https://github.com/aiie") != std::wstring::npos);
    CHECK(res.find(L"user@test.com") != std::wstring::npos);
    CHECK(res.find(L"#ai") != std::wstring::npos);
}

// 20. Async and Cancellation support
TEST(mock_async_and_cancellation) {
    MockProvider provider(std::chrono::milliseconds(20));
    TranslationRequest req = MakeReq(L"hola mundo", TransformOperation::Translate, L"es", L"en");

    std::atomic<bool> done{false};
    TranslationResult result;
    provider.Translate(req, [&](TranslationResult r) {
        result = std::move(r);
        done.store(true);
    });
    for (int i = 0; i < 100 && !done.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(done.load());
    CHECK(result.ok);
    CHECK_STREQ(result.text, L"hello world");

    MockProvider cancelProvider(std::chrono::milliseconds(200));
    done.store(false);
    cancelProvider.Translate(req, [&](TranslationResult r) {
        result = std::move(r);
        done.store(true);
    });
    cancelProvider.CancelAll();
    for (int i = 0; i < 100 && !done.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(done.load());
    CHECK(!result.ok);
    CHECK_STREQ(result.error, L"cancelled");
}