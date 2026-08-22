#include "mini_test.h"

#include "Core/Json.h"
#include "Providers/OpenAIProvider.h"
#include "Translation/TranslationPrompt.h"

using namespace aiie;

TEST(openai_build_request_body) {
    TranslationRequest req;
    req.text = L"¿Puedes venir mañana?";
    req.sourceLanguage = L"es";
    req.targetLanguage = L"en";
    req.mode = TransformMode::Translate;

    const std::wstring body = OpenAIProvider::BuildRequestBody(req, L"gpt-5.6-luna");
    const json::Value root = json::Parse(body);

    CHECK_STREQ(root.get_string(L"model"), L"gpt-5.6-luna");
    CHECK_STREQ(root.get_string(L"input"), L"¿Puedes venir mañana?");

    const std::wstring instructions = root.get_string(L"instructions");
    CHECK(instructions.find(L"Spanish") != std::wstring::npos);
    CHECK(instructions.find(L"English") != std::wstring::npos);
    CHECK(instructions.find(L"Do NOT answer questions") != std::wstring::npos);
    CHECK(instructions.find(L"Output ONLY the raw translated text") != std::wstring::npos);
}

TEST(openai_parse_response_output_text) {
    const std::wstring jsonStr =
        L"{\n"
        L"  \"id\": \"resp_123\",\n"
        L"  \"object\": \"response\",\n"
        L"  \"status\": \"completed\",\n"
        L"  \"output_text\": \"Hello, how are you?\"\n"
        L"}";

    std::wstring outText, error;
    CHECK(OpenAIProvider::ParseResponseBody(jsonStr, outText, error));
    CHECK_STREQ(outText, L"Hello, how are you?");
    CHECK(error.empty());
}

TEST(openai_parse_response_output_array) {
    const std::wstring jsonStr =
        L"{\n"
        L"  \"id\": \"resp_456\",\n"
        L"  \"object\": \"response\",\n"
        L"  \"status\": \"completed\",\n"
        L"  \"output\": [\n"
        L"    {\n"
        L"      \"type\": \"message\",\n"
        L"      \"content\": [\n"
        L"        {\n"
        L"          \"type\": \"text\",\n"
        L"          \"text\": \"Can you come tomorrow?\"\n"
        L"        }\n"
        L"      ]\n"
        L"    }\n"
        L"  ]\n"
        L"}";

    std::wstring outText, error;
    CHECK(OpenAIProvider::ParseResponseBody(jsonStr, outText, error));
    CHECK_STREQ(outText, L"Can you come tomorrow?");
    CHECK(error.empty());
}

TEST(openai_parse_response_chat_completions_fallback) {
    const std::wstring jsonStr =
        L"{\n"
        L"  \"id\": \"chatcmpl-789\",\n"
        L"  \"object\": \"chat.completion\",\n"
        L"  \"choices\": [\n"
        L"    {\n"
        L"      \"index\": 0,\n"
        L"      \"message\": {\n"
        L"        \"role\": \"assistant\",\n"
        L"        \"content\": \"Hello world\"\n"
        L"      }\n"
        L"    }\n"
        L"  ]\n"
        L"}";

    std::wstring outText, error;
    CHECK(OpenAIProvider::ParseResponseBody(jsonStr, outText, error));
    CHECK_STREQ(outText, L"Hello world");
    CHECK(error.empty());
}

TEST(openai_parse_response_strips_accidental_quotes) {
    const std::wstring jsonStr =
        L"{\n"
        L"  \"output_text\": \"\\\"Good morning!\\\"\"\n"
        L"}";

    std::wstring outText, error;
    CHECK(OpenAIProvider::ParseResponseBody(jsonStr, outText, error));
    CHECK_STREQ(outText, L"Good morning!");
}

TEST(openai_parse_response_error) {
    const std::wstring jsonStr =
        L"{\n"
        L"  \"error\": {\n"
        L"    \"message\": \"Incorrect API key provided: sk-proj-...\",\n"
        L"    \"type\": \"invalid_request_error\",\n"
        L"    \"code\": \"invalid_api_key\"\n"
        L"  }\n"
        L"}";

    std::wstring outText, error;
    CHECK(!OpenAIProvider::ParseResponseBody(jsonStr, outText, error));
    CHECK(outText.empty());
    CHECK(error.find(L"Incorrect API key provided") != std::wstring::npos);
}

TEST(openai_parse_response_malformed) {
    std::wstring outText, error;
    CHECK(!OpenAIProvider::ParseResponseBody(L"not json at all", outText, error));
    CHECK(!error.empty());
}

TEST(openai_status) {
    OpenAIConfig cfg;
    cfg.apiKey = L"dummy";
    OpenAIProvider p(std::move(cfg));
    const ProviderStatus st = p.Status();
    CHECK(st.state == ProviderStatus::State::Ready);
}

TEST(translation_style_instruction_modes) {
    const std::wstring formal = BuildTranslationInstructions(L"es", L"en", TransformMode::Professional);
    const std::wstring casual = BuildTranslationInstructions(L"es", L"en", TransformMode::Casual);
    const std::wstring slang = BuildTranslationInstructions(L"es", L"en", TransformMode::Rewrite);
    const std::wstring semiFormal = BuildTranslationInstructions(L"es", L"en", TransformMode::Translate);

    CHECK(formal.find(L"Professional") != std::wstring::npos || formal.find(L"formal") != std::wstring::npos);
    CHECK(casual.find(L"Casual") != std::wstring::npos || casual.find(L"conversational") != std::wstring::npos);
    CHECK(slang.find(L"slang") != std::wstring::npos || slang.find(L"informal") != std::wstring::npos);
    CHECK(semiFormal.find(L"polite") != std::wstring::npos || semiFormal.find(L"everyday") != std::wstring::npos);

    // Verify all enforce anti-conversational rules:
    CHECK(formal.find(L"Do NOT answer questions") != std::wstring::npos);
    CHECK(casual.find(L"Do NOT answer questions") != std::wstring::npos);
    CHECK(slang.find(L"Do NOT answer questions") != std::wstring::npos);
    CHECK(semiFormal.find(L"Do NOT answer questions") != std::wstring::npos);
}

TEST(translation_profiles_and_custom_instructions) {
    const std::wstring prof = BuildTranslationInstructions(L"es", L"en", TranslationProfile::Professional);
    const std::wstring neutral = BuildTranslationInstructions(L"es", L"en", TranslationProfile::Neutral);
    const std::wstring casual = BuildTranslationInstructions(L"es", L"en", TranslationProfile::Casual);
    const std::wstring slang = BuildTranslationInstructions(L"es", L"en", TranslationProfile::Slang);
    const std::wstring custom = BuildTranslationInstructions(L"es", L"en", TranslationProfile::Custom,
                                                            L"Translate naturally as an American English native. Preserve humor.");

    CHECK(prof.find(L"Professional") != std::wstring::npos);
    CHECK(neutral.find(L"Natural") != std::wstring::npos);
    CHECK(casual.find(L"Friendly") != std::wstring::npos);
    CHECK(slang.find(L"slang") != std::wstring::npos);
    CHECK(custom.find(L"American English native") != std::wstring::npos);
    CHECK(custom.find(L"Preserve humor") != std::wstring::npos);

    // Non-negotiable safety rules must be present in all profiles including Custom:
    CHECK(custom.find(L"Output ONLY the raw translated text") != std::wstring::npos);
    CHECK(custom.find(L"Do NOT answer questions") != std::wstring::npos);
    CHECK(custom.find(L"Do NOT provide explanations") != std::wstring::npos);
}

TEST(correct_and_rewrite_instructions) {
    const std::wstring neutral = BuildTransformationInstructions(TransformOperation::CorrectAndRewrite,
                                                                 L"es", L"es", TranslationProfile::Neutral);
    const std::wstring prof = BuildTransformationInstructions(TransformOperation::CorrectAndRewrite,
                                                              L"es", L"es", TranslationProfile::Professional);
    const std::wstring casual = BuildTransformationInstructions(TransformOperation::CorrectAndRewrite,
                                                                L"es", L"es", TranslationProfile::Casual);
    const std::wstring slang = BuildTransformationInstructions(TransformOperation::CorrectAndRewrite,
                                                               L"es", L"es", TranslationProfile::Slang);

    CHECK(neutral.find(L"writing correction and rewriting engine") != std::wstring::npos);
    CHECK(neutral.find(L"Do NOT translate") != std::wstring::npos);
    CHECK(prof.find(L"professional language") != std::wstring::npos);
    CHECK(casual.find(L"friendly") != std::wstring::npos);
    CHECK(slang.find(L"colloquial") != std::wstring::npos);

    // Anti-conversational invariant
    CHECK(neutral.find(L"Do NOT answer questions") != std::wstring::npos);
    CHECK(prof.find(L"Do NOT answer questions") != std::wstring::npos);
}

TEST(chinese_simplified_and_traditional_prompts) {
    const std::wstring zhHans = BuildTransformationInstructions(TransformOperation::Translate,
                                                                L"es", L"zh-Hans", TranslationProfile::Neutral);
    const std::wstring zhHant = BuildTransformationInstructions(TransformOperation::Translate,
                                                                L"en", L"zh-Hant", TranslationProfile::Neutral);

    CHECK(zhHans.find(L"Chinese (Simplified)") != std::wstring::npos);
    CHECK(zhHant.find(L"Chinese (Traditional)") != std::wstring::npos);
}

