#include "Core/Json.h"

#include <windows.h>

#include <charconv>
#include <cmath>
#include <cstdlib>

namespace aiie::json {

namespace {

std::wstring Utf8ToWide(const std::string& s);

class Parser {
public:
    explicit Parser(std::wstring_view text) : s_(text) {}

    Value ParseDocument() {
        Value v = ParseValue();
        SkipWs();
        if (!AtEnd()) {
            throw JsonError("Trailing characters after JSON value");
        }
        return v;
    }

private:
    std::wstring_view s_;
    size_t pos_ = 0;

    bool AtEnd() const { return pos_ >= s_.size(); }
    wchar_t Peek() const { return AtEnd() ? L'\0' : s_[pos_]; }
    wchar_t Next() { return AtEnd() ? L'\0' : s_[pos_++]; }
    void SkipWs() {
        while (!AtEnd()) {
            const wchar_t c = s_[pos_];
            if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    [[noreturn]] void Fail(const char* msg) const {
        throw JsonError(std::string(msg) + " (at offset " + std::to_string(pos_) + ")");
    }

    void Expect(wchar_t c) {
        if (Next() != c) {
            Fail("Unexpected character");
        }
    }

    Value ParseValue() {
        SkipWs();
        if (AtEnd()) {
            Fail("Unexpected end of input");
        }
        switch (Peek()) {
        case L'{': return ParseObject();
        case L'[': return ParseArray();
        case L'"': return Value(ParseString());
        case L't': ParseLiteral(L"true"); return Value(true);
        case L'f': ParseLiteral(L"false"); return Value(false);
        case L'n': ParseLiteral(L"null"); return Value();
        default: return ParseNumber();
        }
    }

    void ParseLiteral(std::wstring_view lit) {
        for (const wchar_t c : lit) {
            if (Next() != c) {
                Fail("Invalid literal");
            }
        }
    }

    Value ParseObject() {
        Value obj = Value::MakeObject();
        Expect(L'{');
        SkipWs();
        if (Peek() == L'}') {
            Next();
            return obj;
        }
        for (;;) {
            SkipWs();
            if (Peek() != L'"') {
                Fail("Expected string key");
            }
            std::wstring key = ParseString();
            SkipWs();
            Expect(L':');
            obj.set(std::move(key), ParseValue());
            SkipWs();
            const wchar_t c = Next();
            if (c == L'}') {
                break;
            }
            if (c != L',') {
                Fail("Expected ',' or '}' in object");
            }
        }
        return obj;
    }

    Value ParseArray() {
        Value arr = Value::MakeArray();
        Expect(L'[');
        SkipWs();
        if (Peek() == L']') {
            Next();
            return arr;
        }
        for (;;) {
            arr.push_back(ParseValue());
            SkipWs();
            const wchar_t c = Next();
            if (c == L']') {
                break;
            }
            if (c != L',') {
                Fail("Expected ',' or ']' in array");
            }
        }
        return arr;
    }

    std::wstring ParseString() {
        Expect(L'"');
        std::wstring out;
        for (;;) {
            if (AtEnd()) {
                Fail("Unterminated string");
            }
            const wchar_t c = Next();
            if (c == L'"') {
                break;
            }
            if (c == L'\\') {
                if (AtEnd()) {
                    Fail("Unterminated escape");
                }
                const wchar_t e = Next();
                switch (e) {
                case L'"': out.push_back(L'"'); break;
                case L'\\': out.push_back(L'\\'); break;
                case L'/': out.push_back(L'/'); break;
                case L'b': out.push_back(L'\b'); break;
                case L'f': out.push_back(L'\f'); break;
                case L'n': out.push_back(L'\n'); break;
                case L'r': out.push_back(L'\r'); break;
                case L't': out.push_back(L'\t'); break;
                case L'u': {
                    uint32_t cp = ParseHex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF && !AtEnd() && Peek() == L'\\') {
                        // Possible surrogate pair.
                        const size_t save = pos_;
                        Next();
                        if (!AtEnd() && Peek() == L'u') {
                            Next();
                            const uint32_t lo = ParseHex4();
                            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            } else {
                                pos_ = save;
                            }
                        } else {
                            pos_ = save;
                        }
                    }
                    AppendCodePoint(out, cp);
                    break;
                }
                default: Fail("Invalid escape sequence");
                }
            } else if (c < 0x20) {
                Fail("Unescaped control character in string");
            } else {
                out.push_back(c);
            }
        }
        return out;
    }

    uint32_t ParseHex4() {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            if (AtEnd()) {
                Fail("Truncated \\u escape");
            }
            const wchar_t c = Next();
            uint32_t d;
            if (c >= L'0' && c <= L'9') d = static_cast<uint32_t>(c - L'0');
            else if (c >= L'a' && c <= L'f') d = static_cast<uint32_t>(c - L'a' + 10);
            else if (c >= L'A' && c <= L'F') d = static_cast<uint32_t>(c - L'A' + 10);
            else Fail("Invalid hex digit in \\u escape");
            v = (v << 4) | d;
        }
        return v;
    }

    static void AppendCodePoint(std::wstring& out, uint32_t cp) {
        if (cp <= 0xFFFF) {
            out.push_back(static_cast<wchar_t>(cp));
        } else {
            cp -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
        }
    }

    Value ParseNumber() {
        const size_t start = pos_;
        bool negative = false;
        if (Peek() == L'-') {
            negative = true;
            Next();
        }
        bool any = false;
        bool is_double = false;
        while (!AtEnd()) {
            const wchar_t c = Peek();
            if (c >= L'0' && c <= L'9') {
                any = true;
                Next();
            } else if (c == L'.' || c == L'e' || c == L'E' || c == L'+' || c == L'-') {
                is_double = true;
                Next();
            } else {
                break;
            }
        }
        if (!any) {
            Fail("Invalid number");
        }
        // Strict validation of the scanned token (RFC 8259 number grammar):
        //   - no leading zeros ("01")
        //   - '.' and exponent parts need digits on both sides
        //   - '+'/'-' only immediately after the exponent marker
        const std::wstring token(s_.substr(start, pos_ - start));
        const wchar_t* p = token.c_str();
        if (*p == L'-') {
            ++p;
        }
        if (*p == L'\0' || (*p == L'0' && p[1] >= L'0' && p[1] <= L'9')) {
            Fail("Invalid number");
        }
        bool haveInt = false;
        while (*p >= L'0' && *p <= L'9') {
            haveInt = true;
            ++p;
        }
        if (*p == L'.') {
            ++p;
            bool haveFrac = false;
            while (*p >= L'0' && *p <= L'9') {
                haveFrac = true;
                ++p;
            }
            if (!haveInt || !haveFrac) {
                Fail("Invalid number");
            }
        }
        if (*p == L'e' || *p == L'E') {
            ++p;
            if (*p == L'+' || *p == L'-') {
                ++p;
            }
            bool haveExp = false;
            while (*p >= L'0' && *p <= L'9') {
                haveExp = true;
                ++p;
            }
            if (!haveExp) {
                Fail("Invalid number");
            }
        }
        if (*p != L'\0') {
            Fail("Invalid number");
        }
        std::wstring text(s_.substr(start, pos_ - start));
        double value;
        if (is_double) {
            const std::string narrow = Utf16ToUtf8ForNumber(text);
            const char* end = nullptr;
            value = std::strtod(narrow.c_str(), const_cast<char**>(&end));
        } else {
            int64_t iv = 0;
            const std::string narrow = Utf16ToUtf8ForNumber(text);
            const auto [ptr, ec] = std::from_chars(narrow.data(), narrow.data() + narrow.size(), iv);
            if (ec == std::errc()) {
                return Value(iv);
            }
            value = static_cast<double>(iv);
        }
        return Value(negative ? -std::fabs(value) : value);
    }

    static std::string Utf16ToUtf8ForNumber(std::wstring_view w) {
        std::string out;
        out.reserve(w.size());
        for (const wchar_t c : w) {
            out.push_back(static_cast<char>(c & 0xFF));
        }
        return out;
    }
};

void WriteIndent(std::wstring& out, int indent, int depth) {
    out.append(static_cast<size_t>(indent) * static_cast<size_t>(depth), L' ');
}

void SerializeValue(std::wstring& out, const Value& v, int indent, int depth) {
    switch (v.type()) {
    case Value::Type::Null:
        out.append(L"null");
        break;
    case Value::Type::Bool:
        out.append(v.as_bool() ? L"true" : L"false");
        break;
    case Value::Type::Number: {
        const double d = v.as_number();
        if (d == static_cast<int64_t>(d) && std::fabs(d) < 1e15) {
            out.append(std::to_wstring(static_cast<int64_t>(d)));
        } else {
            char buf[32] = {};
            std::snprintf(buf, sizeof(buf), "%.17g", d);
            out.append(Utf8ToWide(buf));
        }
        break;
    }
    case Value::Type::String:
        out.append(EscapeString(v.as_string()));
        break;
    case Value::Type::Array: {
        if (v.as_array().empty()) {
            out.append(L"[]");
            break;
        }
        out.push_back(L'[');
        bool first = true;
        for (const Value& item : v.as_array()) {
            if (!first) {
                out.push_back(L',');
            }
            first = false;
            if (indent >= 0) {
                out.push_back(L'\n');
                WriteIndent(out, indent, depth + 1);
            }
            SerializeValue(out, item, indent, depth + 1);
        }
        if (indent >= 0) {
            out.push_back(L'\n');
            WriteIndent(out, indent, depth);
        }
        out.push_back(L']');
        break;
    }
    case Value::Type::Object: {
        if (v.as_object().empty()) {
            out.append(L"{}");
            break;
        }
        out.push_back(L'{');
        bool first = true;
        for (const auto& [key, item] : v.as_object()) {
            if (!first) {
                out.push_back(L',');
            }
            first = false;
            if (indent >= 0) {
                out.push_back(L'\n');
                WriteIndent(out, indent, depth + 1);
            }
            out.append(EscapeString(key));
            out.append(L":");
            if (indent >= 0) {
                out.push_back(L' ');
            }
            SerializeValue(out, item, indent, depth + 1);
        }
        if (indent >= 0) {
            out.push_back(L'\n');
            WriteIndent(out, indent, depth);
        }
        out.push_back(L'}');
        break;
    }
    }
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

} // namespace

Value Parse(std::wstring_view text) {
    return Parser(text).ParseDocument();
}

std::wstring Serialize(const Value& v, int indent) {
    std::wstring out;
    SerializeValue(out, v, indent, 0);
    return out;
}

std::wstring EscapeString(std::wstring_view s) {
    std::wstring out;
    out.reserve(s.size() + 2);
    out.push_back(L'"');
    for (const wchar_t c : s) {
        switch (c) {
        case L'"': out.append(L"\\\""); break;
        case L'\\': out.append(L"\\\\"); break;
        case L'\n': out.append(L"\\n"); break;
        case L'\r': out.append(L"\\r"); break;
        case L'\t': out.append(L"\\t"); break;
        case L'\b': out.append(L"\\b"); break;
        case L'\f': out.append(L"\\f"); break;
        default:
            if (c < 0x20) {
                wchar_t buf[8];
                swprintf_s(buf, L"\\u%04x", static_cast<unsigned>(c));
                out.append(buf);
            } else {
                out.push_back(c);
            }
        }
    }
    out.push_back(L'"');
    return out;
}

} // namespace aiie::json