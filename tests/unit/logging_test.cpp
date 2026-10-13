#include "mini_test.h"

#include "Core/Logging.h"

using namespace aiie;

TEST(logger_redact_empty_or_short_secret) {
    // Secrets under 4 characters or empty should not redact anything.
    CHECK_STREQ(Logger::Redact(L"https://api.openai.com/v1/chat?key=123", L""), L"https://api.openai.com/v1/chat?key=123");
    CHECK_STREQ(Logger::Redact(L"https://api.openai.com/v1/chat?key=123", L"a"), L"https://api.openai.com/v1/chat?key=123");
    CHECK_STREQ(Logger::Redact(L"https://api.openai.com/v1/chat?key=123", L"ab"), L"https://api.openai.com/v1/chat?key=123");
    CHECK_STREQ(Logger::Redact(L"https://api.openai.com/v1/chat?key=123", L"abc"), L"https://api.openai.com/v1/chat?key=123");
}

TEST(logger_redact_empty_text) {
    CHECK_STREQ(Logger::Redact(L"", L"secret_key_12345"), L"");
}

TEST(logger_redact_no_match) {
    CHECK_STREQ(Logger::Redact(L"Hello world with no secret", L"secret_key_12345"), L"Hello world with no secret");
}

TEST(logger_redact_exact_match) {
    CHECK_STREQ(Logger::Redact(L"secret_key_12345", L"secret_key_12345"), L"***");
}

TEST(logger_redact_single_occurrence) {
    CHECK_STREQ(Logger::Redact(L"Bearer secret_key_12345 in header", L"secret_key_12345"), L"Bearer *** in header");
    CHECK_STREQ(Logger::Redact(L"secret_key_12345 at start", L"secret_key_12345"), L"*** at start");
    CHECK_STREQ(Logger::Redact(L"at end secret_key_12345", L"secret_key_12345"), L"at end ***");
}

TEST(logger_redact_multiple_occurrences) {
    CHECK_STREQ(
        Logger::Redact(L"Key1: secret_key_12345, Key2: secret_key_12345", L"secret_key_12345"),
        L"Key1: ***, Key2: ***"
    );
}

TEST(logger_redact_consecutive_occurrences) {
    CHECK_STREQ(
        Logger::Redact(L"secret_key_12345secret_key_12345", L"secret_key_12345"),
        L"******"
    );
}

TEST(logger_redact_unicode_text_and_secret) {
    CHECK_STREQ(
        Logger::Redact(L"User 密钥_12345 logged in", L"密钥_12345"),
        L"User *** logged in"
    );
}
