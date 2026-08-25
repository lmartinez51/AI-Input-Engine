#include "mini_test.h"

#include "Core/Json.h"
#include "Core/Logging.h"
#include "Providers/GeminiProvider.h"

using namespace aiie;

TEST(gemini_build_request_body) {
    TranslationRequest req;
    req.text = L"Hola mundo";
    req.operation = TransformOperation::Translate;
    req.sourceLanguage = L"es";
    req.targetLanguage = L"en";
    req.profile = TranslationProfile::Neutral;

    const std::wstring body = GeminiProvider::BuildRequestBody(req, L"gemini-3.6-flash");
    const json::Value root = json::Parse(body);

    CHECK(root.is_object());
    CHECK(root.find(L"contents") != nullptr);
    CHECK(root.find(L"systemInstruction") != nullptr);
    CHECK(root.find(L"generationConfig") != nullptr);

    const json::Value& content = root.find(L"contents")->as_array().front();
    CHECK_STREQ(content.get_string(L"role"), L"user");
    CHECK_STREQ(content.find(L"parts")->as_array().front().get_string(L"text"), L"Hola mundo");

    const std::wstring sysText = root.find(L"systemInstruction")->find(L"parts")
                                     ->as_array().front().get_string(L"text");
    CHECK(sysText.find(L"Spanish") != std::wstring::npos);
    CHECK(sysText.find(L"English") != std::wstring::npos);
}

TEST(gemini_build_request_body_chinese) {
    TranslationRequest req;
    req.text = L"Hola mundo";
    req.operation = TransformOperation::Translate;
    req.sourceLanguage = L"es";
    req.targetLanguage = L"zh-Hans";
    req.profile = TranslationProfile::Neutral;

    const std::wstring body = GeminiProvider::BuildRequestBody(req, L"gemini-3.6-flash");
    const json::Value root = json::Parse(body);

    const std::wstring sysText = root.find(L"systemInstruction")->find(L"parts")
                                     ->as_array().front().get_string(L"text");
    CHECK(sysText.find(L"Spanish") != std::wstring::npos);
    CHECK(sysText.find(L"Chinese (Simplified)") != std::wstring::npos);
}

TEST(gemini_build_request_body_correct_and_rewrite) {
    TranslationRequest req;
    req.text = L"Ola ayer fui al super aver si comprava leche pero no avia.";
    req.operation = TransformOperation::CorrectAndRewrite;
    req.sourceLanguage = L"es";
    req.targetLanguage = L"es";
    req.profile = TranslationProfile::Neutral;

    const std::wstring body = GeminiProvider::BuildRequestBody(req, L"gemini-3.6-flash");
    const json::Value root = json::Parse(body);

    const std::wstring sysText = root.find(L"systemInstruction")->find(L"parts")
                                     ->as_array().front().get_string(L"text");
    CHECK(sysText.find(L"text transformation engine") != std::wstring::npos);
    CHECK(sysText.find(L"Do NOT translate") != std::wstring::npos);
    CHECK(sysText.find(L"Spanish") != std::wstring::npos);
}

TEST(gemini_build_request_body_includes_context) {
    TranslationRequest req;
    req.text = L"y mañana sigo";
    req.operation = TransformOperation::Translate;
    req.sourceLanguage = L"es";
    req.targetLanguage = L"en";
    req.context = L"Necesito terminar el proyecto.";
    const json::Value root = json::Parse(GeminiProvider::BuildRequestBody(req, L"gemini-3.6-flash"));
    const std::wstring userText = root.find(L"contents")->as_array().front()
                                      .find(L"parts")->as_array().front().get_string(L"text");
    CHECK(userText.find(L"Necesito terminar el proyecto") != std::wstring::npos);
    CHECK(userText.find(L"y mañana sigo") != std::wstring::npos);
}

TEST(gemini_parse_response_success) {
    const std::wstring body = L"{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"Hello world\"}]}}]}";
    std::wstring text, detected, error;
    CHECK(GeminiProvider::ParseResponseBody(body, text, detected, error));
    CHECK_STREQ(text, L"Hello world");
}

TEST(gemini_parse_response_multiple_parts) {
    const std::wstring body =
        L"{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"Hello \"},{\"text\":\"world\"}]}}]}";
    std::wstring text, detected, error;
    CHECK(GeminiProvider::ParseResponseBody(body, text, detected, error));
    CHECK_STREQ(text, L"Hello world");
}

TEST(gemini_parse_response_no_candidates) {
    const std::wstring body = L"{\"promptFeedback\":{\"blockReason\":\"SAFETY\"}}";
    std::wstring text, detected, error;
    CHECK(!GeminiProvider::ParseResponseBody(body, text, detected, error));
    CHECK(!error.empty());
}

TEST(gemini_parse_response_malformed) {
    std::wstring text, detected, error;
    CHECK(!GeminiProvider::ParseResponseBody(L"{nope", text, detected, error));
    CHECK(!GeminiProvider::ParseResponseBody(L"{}", text, detected, error));
    CHECK(!GeminiProvider::ParseResponseBody(L"{\"candidates\":[]}", text, detected, error));
    CHECK(!GeminiProvider::ParseResponseBody(L"{\"candidates\":[{}]}", text, detected, error));
}

TEST(gemini_parse_response_empty_text) {
    const std::wstring body = L"{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"\"}]}}]}";
    std::wstring text, detected, error;
    CHECK(!GeminiProvider::ParseResponseBody(body, text, detected, error));
}

TEST(gemini_error_message_never_contains_key) {
    const std::wstring key = L"AIzaSySecret12345";
    const std::wstring url = L"https://generativelanguage.googleapis.com/v1beta/models/x:generateContent?key=" + key;
    const std::wstring redacted = Logger::Redact(url, key);
    CHECK(redacted.find(key) == std::wstring::npos);
    CHECK(redacted.find(L"***") != std::wstring::npos);
}

TEST(gemini_status) {
    GeminiConfig gc;
    gc.apiKey = L"";
    GeminiProvider provider(gc);
    const ProviderStatus st = provider.Status();
    CHECK(st.state == ProviderStatus::State::Ready || st.state == ProviderStatus::State::Error);
    CHECK(provider.RequiresNetwork());
}