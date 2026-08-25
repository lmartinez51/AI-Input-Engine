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

TEST(openai_build_request_body_includes_context) {
    TranslationRequest req;
    req.text = L"¿Puedes venir mañana?";
    req.sourceLanguage = L"es";
    req.targetLanguage = L"en";
    req.context = L"Ayer no pude ir a la oficina.";
    req.mode = TransformMode::Translate;

    const std::wstring body = OpenAIProvider::BuildRequestBody(req, L"gpt-5.6-luna");
    const json::Value root = json::Parse(body);

    const std::wstring input = root.get_string(L"input");
    CHECK(input.find(L"Previous context (for reference only):") != std::wstring::npos);
    CHECK(input.find(L"Ayer no pude ir a la oficina.") != std::wstring::npos);
    CHECK(input.find(L"¿Puedes venir mañana?") != std::wstring::npos);
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

    CHECK(prof.find(L"Professional") != std::wstring::npos || prof.find(L"Polished") != std::wstring::npos);
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

    CHECK(neutral.find(L"text transformation engine") != std::wstring::npos);
    CHECK(neutral.find(L"Do NOT translate") != std::wstring::npos);
    CHECK(prof.find(L"professional language") != std::wstring::npos || prof.find(L"Polished") != std::wstring::npos);
    CHECK(casual.find(L"Friendly") != std::wstring::npos || casual.find(L"conversational") != std::wstring::npos);
    CHECK(slang.find(L"colloquial") != std::wstring::npos || slang.find(L"informal") != std::wstring::npos);

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

TEST(prompt_semantic_and_cultural_capabilities) {
    const std::wstring slang = BuildTranslationInstructions(L"es", L"en", TranslationProfile::Slang);
    const std::wstring casual = BuildTranslationInstructions(L"es", L"en", TranslationProfile::Casual);
    const std::wstring neutral = BuildTranslationInstructions(L"es", L"en", TranslationProfile::Neutral);

    // Slang recognizes Mexican Spanish & regional expressions:
    CHECK(slang.find(L"Mexican Spanish") != std::wstring::npos);
    CHECK(slang.find(L"me cayó el veinte") != std::wstring::npos);
    CHECK(slang.find(L"a huevo") != std::wstring::npos);
    CHECK(slang.find(L"clean source -> clean target") != std::wstring::npos);

    // All profiles enforce core semantic principles & native target flow:
    CHECK(slang.find(L"intended meaning, tone, and social context") != std::wstring::npos);
    CHECK(casual.find(L"intended meaning, tone, and social context") != std::wstring::npos);
    CHECK(neutral.find(L"intended meaning, tone, and social context") != std::wstring::npos);

    // Anti-hallucination and context usage bounds:
    CHECK(slang.find(L"Never invent facts, relationships, background events") != std::wstring::npos);
    CHECK(slang.find(L"Context Usage:") != std::wstring::npos);
    CHECK(slang.find(L"Never translate, reproduce, or output the previous context") != std::wstring::npos);
}

TEST(prompt_format_user_payload_helper) {
    TranslationRequest req;
    req.text = L"Me cayó el veinte.";
    req.operation = TransformOperation::Translate;
    CHECK_STREQ(FormatUserPayload(req), L"Me cayó el veinte.");

    req.context = L"Estuve pensando en lo que dijiste.";
    const std::wstring withCtx = FormatUserPayload(req);
    CHECK(withCtx.find(L"Previous context (for reference only):") != std::wstring::npos);
    CHECK(withCtx.find(L"Estuve pensando en lo que dijiste.") != std::wstring::npos);
    CHECK(withCtx.find(L"Current text to translate:\nMe cayó el veinte.") != std::wstring::npos);

    req.operation = TransformOperation::CorrectAndRewrite;
    const std::wstring withCtxRewrite = FormatUserPayload(req);
    CHECK(withCtxRewrite.find(L"Current text to correct and rewrite:\nMe cayó el veinte.") != std::wstring::npos);
}

TEST(openai_streaming_request_body) {
    TranslationRequest req;
    req.text = L"Hola mundo";
    req.sourceLanguage = L"es";
    req.targetLanguage = L"en";

    const std::wstring body = OpenAIProvider::BuildRequestBody(req, L"gpt-5.6-luna", true);
    const json::Value root = json::Parse(body);

    CHECK_STREQ(root.get_string(L"model"), L"gpt-5.6-luna");
    CHECK_EQ(root.get_bool(L"stream", false), true);
    CHECK_STREQ(root.get_string(L"input"), L"Hola mundo");
}

TEST(openai_non_streaming_request_body) {
    TranslationRequest req;
    req.text = L"Hola mundo";
    req.sourceLanguage = L"es";
    req.targetLanguage = L"en";

    const std::wstring body = OpenAIProvider::BuildRequestBody(req, L"gpt-5.6-luna", false);
    const json::Value root = json::Parse(body);

    CHECK_STREQ(root.get_string(L"model"), L"gpt-5.6-luna");
    CHECK(root.find(L"stream") == nullptr);
}

TEST(openai_reasoning_effort_serialization) {
    TranslationRequest req;
    req.text = L"Texto de prueba";
    req.sourceLanguage = L"es";
    req.targetLanguage = L"en";

    // 1. None
    const std::wstring bodyNone = OpenAIProvider::BuildRequestBody(req, L"gpt-5.6-luna", true, L"none");
    const json::Value rootNone = json::Parse(bodyNone);
    const json::Value* rObjNone = rootNone.find(L"reasoning");
    CHECK(rObjNone != nullptr && rObjNone->is_object());
    CHECK_STREQ(rObjNone->get_string(L"effort"), L"none");

    // 2. Low
    const std::wstring bodyLow = OpenAIProvider::BuildRequestBody(req, L"gpt-5.6-luna", true, L"low");
    const json::Value rootLow = json::Parse(bodyLow);
    const json::Value* rObjLow = rootLow.find(L"reasoning");
    CHECK(rObjLow != nullptr && rObjLow->is_object());
    CHECK_STREQ(rObjLow->get_string(L"effort"), L"low");

    // 3. Medium
    const std::wstring bodyMed = OpenAIProvider::BuildRequestBody(req, L"gpt-5.6-luna", true, L"medium");
    const json::Value rootMed = json::Parse(bodyMed);
    const json::Value* rObjMed = rootMed.find(L"reasoning");
    CHECK(rObjMed != nullptr && rObjMed->is_object());
    CHECK_STREQ(rObjMed->get_string(L"effort"), L"medium");
}

TEST(openai_sse_chunk_parser_basic) {
    std::string sseBuffer;
    std::vector<SseEvent> events;

    // First chunk delivers incomplete event line:
    const std::string chunk1 = "event: response.output_text.delta\ndata: {\"delta\":\"Hel";
    OpenAIProvider::ParseSseChunk(chunk1, sseBuffer, events);
    CHECK(events.empty());

    // Second chunk finishes the line and starts another:
    const std::string chunk2 = "lo\"}\n\nevent: response.output_text.delta\ndata: {\"delta\":\" world\"}\n\n";
    OpenAIProvider::ParseSseChunk(chunk2, sseBuffer, events);
    CHECK_EQ(events.size(), 2);
    CHECK_STREQ(events[0].eventType, L"response.output_text.delta");
    CHECK(events[0].data.find(L"Hello") != std::wstring::npos);
    CHECK_STREQ(events[1].eventType, L"response.output_text.delta");
    CHECK(events[1].data.find(L"world") != std::wstring::npos);
}

TEST(openai_sse_delta_text_accumulation) {
    std::wstring accum, done, err;
    bool completed = false;

    SseEvent ev1{L"response.output_text.delta", L"{\"delta\":\"Hello\"}"};
    CHECK(OpenAIProvider::ProcessSseEvent(ev1, accum, done, completed, err));
    CHECK_STREQ(accum, L"Hello");
    CHECK_EQ(completed, false);

    SseEvent ev2{L"response.output_text.delta", L"{\"delta\":\" world!\"}"};
    CHECK(OpenAIProvider::ProcessSseEvent(ev2, accum, done, completed, err));
    CHECK_STREQ(accum, L"Hello world!");
    CHECK_EQ(completed, false);
}

TEST(openai_sse_output_text_done) {
    std::wstring accum = L"Hello world!";
    std::wstring done, err;
    bool completed = false;

    SseEvent evDone{L"response.output_text.done", L"{\"text\":\"Hello world!\"}"};
    CHECK(OpenAIProvider::ProcessSseEvent(evDone, accum, done, completed, err));
    CHECK_STREQ(done, L"Hello world!");
    CHECK_EQ(completed, false);
}

TEST(openai_sse_response_completed) {
    std::wstring accum = L"Hello world!";
    std::wstring done, err;
    bool completed = false;

    const std::wstring completedPayload =
        L"{\"response\":{\"id\":\"resp_1\",\"status\":\"completed\",\"output_text\":\"Hello world!\"}}";
    SseEvent evComp{L"response.completed", completedPayload};
    CHECK(OpenAIProvider::ProcessSseEvent(evComp, accum, done, completed, err));
    CHECK_EQ(completed, true);
    CHECK_STREQ(done, L"Hello world!");
    CHECK(err.empty());
}

TEST(openai_sse_error_event) {
    std::wstring accum = L"Partial text";
    std::wstring done, err;
    bool completed = false;

    SseEvent evErr{L"error", L"{\"error\":{\"message\":\"Rate limit exceeded\",\"code\":\"rate_limit\"}}"};
    CHECK(!OpenAIProvider::ProcessSseEvent(evErr, accum, done, completed, err));
    CHECK_EQ(completed, false);
    CHECK(err.find(L"Rate limit exceeded") != std::wstring::npos);
}

TEST(openai_sse_malformed_event) {
    std::wstring accum, done, err;
    bool completed = false;

    SseEvent evNoise{L"heartbeat", L"ping: 12345"};
    CHECK(OpenAIProvider::ProcessSseEvent(evNoise, accum, done, completed, err));
    CHECK_EQ(completed, false);
    CHECK(err.empty());
}

TEST(openai_request_plain_text_only) {
    TranslationRequest req;
    req.text = L"Only pure text input";
    req.sourceLanguage = L"en";
    req.targetLanguage = L"es";

    const std::wstring body = OpenAIProvider::BuildRequestBody(req, L"gpt-5.6-luna", true);
    const json::Value root = json::Parse(body);

    // Verify "input" is a direct string and NOT an array or multimodal object
    const json::Value* inputVal = root.find(L"input");
    CHECK(inputVal != nullptr);
    CHECK(inputVal->is_string());
    CHECK_STREQ(inputVal->as_string(), L"Only pure text input");

    // Verify NO vision or multimodal keys exist anywhere in request
    CHECK(root.find(L"image") == nullptr);
    CHECK(root.find(L"image_url") == nullptr);
    CHECK(root.find(L"input_image") == nullptr);
    CHECK(root.find(L"messages") == nullptr);
}

TEST(openai_request_builder_omits_unsupported_reasoning_for_gpt4o) {
    TranslationRequest req;
    req.text = L"Hello";
    req.sourceLanguage = L"en";
    req.targetLanguage = L"es";

    ModelParameters params;
    params.reasoningEffort = L"low";
    params.temperature = 0.8;
    params.topP = 0.95;
    params.topK = 50;

    const std::wstring body = OpenAIProvider::BuildRequestBody(req, L"gpt-4o", true, L"low", &params);
    const json::Value root = json::Parse(body);

    CHECK_STREQ(root.get_string(L"model"), L"gpt-4o");
    // gpt-4o does NOT support reasoning: reasoning object must be completely omitted
    CHECK(root.find(L"reasoning") == nullptr);
    // top_k must be omitted for OpenAI
    CHECK(root.find(L"top_k") == nullptr);
    // temperature and top_p must be serialized
    CHECK(root.find(L"temperature") != nullptr);
    CHECK_EQ(root.get_number(L"temperature"), 0.8);
    CHECK(root.find(L"top_p") != nullptr);
    CHECK_EQ(root.get_number(L"top_p"), 0.95);
}

TEST(openai_request_builder_omits_unsupported_temperature_for_o3mini) {
    TranslationRequest req;
    req.text = L"Hello";
    req.sourceLanguage = L"en";
    req.targetLanguage = L"es";

    ModelParameters params;
    params.reasoningEffort = L"medium";
    params.temperature = 0.5;
    params.topP = 0.9;
    params.maxOutputTokens = 4096;

    const std::wstring body = OpenAIProvider::BuildRequestBody(req, L"o3-mini", true, L"medium", &params);
    const json::Value root = json::Parse(body);

    CHECK_STREQ(root.get_string(L"model"), L"o3-mini");
    // o3-mini supports reasoning
    const json::Value* rObj = root.find(L"reasoning");
    CHECK(rObj != nullptr);
    CHECK_STREQ(rObj->get_string(L"effort"), L"medium");
    // temperature and top_p are NOT supported for o3-mini
    CHECK(root.find(L"temperature") == nullptr);
    CHECK(root.find(L"top_p") == nullptr);
    // max_output_tokens is supported
    CHECK(root.find(L"max_output_tokens") != nullptr);
    CHECK_EQ(root.get_number(L"max_output_tokens"), 4096.0);
}

TEST(openai_request_builder_sanitizes_reasoning_temp_conflict) {
    TranslationRequest req;
    req.text = L"Hello";
    req.sourceLanguage = L"en";
    req.targetLanguage = L"es";

    ModelParameters params;
    params.reasoningEffort = L"low";
    params.temperature = 0.7;

    const std::wstring body = OpenAIProvider::BuildRequestBody(req, L"gpt-5.6-luna", true, L"low", &params);
    const json::Value root = json::Parse(body);

    // When reasoning is active (non-none), temperature override is omitted
    const json::Value* rObj = root.find(L"reasoning");
    CHECK(rObj != nullptr);
    CHECK_STREQ(rObj->get_string(L"effort"), L"low");
    CHECK(root.find(L"temperature") == nullptr);
}

TEST(openai_request_builder_supports_gpt_5_4_family) {
    TranslationRequest req;
    req.text = L"Hello";
    req.sourceLanguage = L"en";
    req.targetLanguage = L"es";

    ModelParameters params;
    params.reasoningEffort = L"none";
    params.temperature = 0.4;
    params.topP = 0.9;

    const std::wstring body54 = OpenAIProvider::BuildRequestBody(req, L"gpt-5.4", true, L"none", &params);
    const json::Value root54 = json::Parse(body54);
    CHECK_STREQ(root54.get_string(L"model"), L"gpt-5.4");
    CHECK(root54.find(L"reasoning") != nullptr);
    CHECK_EQ(root54.get_number(L"temperature"), 0.4);

    const std::wstring body54m = OpenAIProvider::BuildRequestBody(req, L"gpt-5.4-mini", true, L"none", &params);
    const json::Value root54m = json::Parse(body54m);
    CHECK_STREQ(root54m.get_string(L"model"), L"gpt-5.4-mini");
    CHECK(root54m.find(L"reasoning") != nullptr);
    CHECK_EQ(root54m.get_number(L"temperature"), 0.4);
}


