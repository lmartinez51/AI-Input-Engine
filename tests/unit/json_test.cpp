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

TEST(json_serialize_indent_positive) {
    // Simple object with default indent (indent = 2)
    json::Value obj = json::Value::MakeObject();
    obj.set(L"key", L"value");
    obj.set(L"num", 10);

    const std::wstring serialized2 = json::Serialize(obj, 2);
    const std::wstring expected2 = L"{\n  \"key\": \"value\",\n  \"num\": 10\n}";
    CHECK_STREQ(serialized2, expected2);

    // Verify default parameter uses indent = 2
    const std::wstring serialized_def = json::Serialize(obj);
    CHECK_STREQ(serialized_def, expected2);

    // Custom indent (indent = 4 spaces)
    const std::wstring serialized4 = json::Serialize(obj, 4);
    const std::wstring expected4 = L"{\n    \"key\": \"value\",\n    \"num\": 10\n}";
    CHECK_STREQ(serialized4, expected4);

    // Nested object and array pretty printing
    json::Value root = json::Value::MakeObject();
    json::Value arr = json::Value::MakeArray();
    arr.push_back(1);
    arr.push_back(2);
    root.set(L"arr", std::move(arr));
    root.set(L"empty_arr", json::Value::MakeArray());
    root.set(L"empty_obj", json::Value::MakeObject());

    const std::wstring nested_serialized = json::Serialize(root, 2);
    const std::wstring nested_expected = L"{\n  \"arr\": [\n    1,\n    2\n  ],\n  \"empty_arr\": [],\n  \"empty_obj\": {}\n}";
    CHECK_STREQ(nested_serialized, nested_expected);

    // Verify pretty printed JSON parses back into an equivalent Value structure
    const json::Value parsed = json::Parse(nested_serialized);
    CHECK(parsed.is_object());
    CHECK_EQ(parsed.find(L"arr")->as_array().size(), 2u);
    CHECK_EQ(parsed.find(L"arr")->as_array()[0].as_int(), 1);
    CHECK_EQ(parsed.find(L"arr")->as_array()[1].as_int(), 2);
    CHECK(parsed.find(L"empty_arr")->as_array().empty());
    CHECK(parsed.find(L"empty_obj")->as_object().empty());
}