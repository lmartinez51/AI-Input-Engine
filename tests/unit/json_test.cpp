#include "mini_test.h"

#include "Core/Json.h"
#include "Core/Utf.h"

using namespace aiie;

TEST(json_parse_basic) {
    const json::Value v = json::Parse(LR"({"a": 1, "b": "two", "c": true, "d": null, "e": [1,2,3]})");
    CHECK(v.is_object());
    CHECK_EQ(v.get_int(L"a"), 1);
    CHECK_STREQ(v.get_string(L"b"), L"two");
    CHECK(v.get_bool(L"c"));
    CHECK(v.find(L"d")->is_null());
    CHECK(v.find(L"e")->is_array());
    CHECK_EQ(v.find(L"e")->as_array().size(), 3u);
}

TEST(json_parse_unicode_escapes) {
    const json::Value v = json::Parse(LR"({"s": "\u00e1\u00f1o\u4e16\u754c"})");
    CHECK_STREQ(v.get_string(L"s"), L"\u00e1\u00f1o\u4e16\u754c");
}

TEST(json_parse_surrogate_pair) {
    // "😀" = U+1F600 = \ud83d\ude00
    const json::Value v = json::Parse(LR"({"emoji": "\ud83d\ude00"})");
    CHECK_STREQ(v.get_string(L"emoji"), L"\U0001F600");
    CHECK(aiie::utf::IsWellFormedUtf16(v.get_string(L"emoji")));
}

TEST(json_parse_numbers) {
    const json::Value v = json::Parse(LR"({"i": -42, "f": 3.5, "e": 1e3})");
    CHECK_EQ(v.get_int(L"i"), -42);
    CHECK_EQ(v.get_number(L"f"), 3.5);
    CHECK_EQ(v.get_number(L"e"), 1000.0);
}

TEST(json_parse_nested) {
    const json::Value v = json::Parse(
        LR"({"candidates":[{"content":{"parts":[{"text":"hello"}]}}],"usage":{"totalTokens":5}})");
    const json::Value& cand = v.find(L"candidates")->as_array().front();
    CHECK_STREQ(cand.find(L"content")->find(L"parts")->as_array().front().get_string(L"text"), L"hello");
}

TEST(json_parse_errors) {
    CHECK_THROWS(json::Parse(L""));
    CHECK_THROWS(json::Parse(L"{"));
    CHECK_THROWS(json::Parse(L"[1,2,]"));
    CHECK_THROWS(json::Parse(L"{\"a\":}"));
    CHECK_THROWS(json::Parse(L"tru"));
    CHECK_THROWS(json::Parse(L"{\"a\" \"b\"}"));
    CHECK_THROWS(json::Parse(L"01"));
}

TEST(json_roundtrip) {
    json::Value v = json::Value::MakeObject();
    v.set(L"str", L"hello \"world\"\nline2");
    v.set(L"num", 42);
    v.set(L"flag", true);
    v.set(L"nul", nullptr);
    json::Value arr = json::Value::MakeArray();
    arr.push_back(json::Value(L"a"));
    arr.push_back(json::Value(1.5));
    v.set(L"arr", std::move(arr));

    const std::wstring serialized = json::Serialize(v, -1);
    const json::Value back = json::Parse(serialized);
    CHECK_STREQ(back.get_string(L"str"), L"hello \"world\"\nline2");
    CHECK_EQ(back.get_int(L"num"), 42);
    CHECK(back.get_bool(L"flag"));
    CHECK(back.find(L"nul")->is_null());
    CHECK_EQ(back.find(L"arr")->as_array().size(), 2u);
}

TEST(json_roundtrip_unicode) {
    const json::Value v = json::Parse(L"{\"s\": \"\u00e1\u00f1o \U0001F600\"}");
    const std::wstring serialized = json::Serialize(v, -1);
    const json::Value back = json::Parse(serialized);
    CHECK_STREQ(back.get_string(L"s"), L"\u00e1\u00f1o \U0001F600");
}

TEST(json_escape_string) {
    CHECK_STREQ(json::EscapeString(L"a\"b\\c\nd"), L"\"a\\\"b\\\\c\\nd\"");
}

TEST(json_missing_keys_have_defaults) {
    const json::Value v = json::Parse(L"{}");
    CHECK_STREQ(v.get_string(L"nope", L"def"), L"def");
    CHECK_EQ(v.get_int(L"nope", 7), 7);
    CHECK(v.get_bool(L"nope", true));
}