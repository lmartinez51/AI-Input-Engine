#include "Composition/SmartPasteManager.h"

#include "Core/Json.h"
#include "Core/Logging.h"

#include <cwctype>
#include <format>
#include <vector>

namespace aiie {

SmartPasteManager::SmartPasteManager(std::shared_ptr<TranslationPipeline> pipeline,
                                     std::shared_ptr<StatusWindow> status,
                                     ConfigStore* configStore,
                                     std::shared_ptr<IDispatcher> dispatcher)
    : pipeline_(std::move(pipeline)),
      status_(std::move(status)),
      configStore_(configStore),
      dispatcher_(std::move(dispatcher)),
      alive_(std::make_shared<std::atomic<bool>>(true)) {}

SmartPasteManager::~SmartPasteManager() {
    if (alive_) {
        alive_->store(false);
    }
}

void SmartPasteManager::SetPrePasteHook(std::function<void()> hook) {
    prePasteHook_ = std::move(hook);
}

std::wstring SmartPasteManager::GetProcessExecutableName(DWORD processId) {
    if (processId == 0) {
        return {};
    }
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!hProcess) {
        return {};
    }
    wchar_t exePath[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    if (QueryFullProcessImageNameW(hProcess, 0, exePath, &size)) {
        CloseHandle(hProcess);
        std::wstring path(exePath, size);
        const size_t lastSlash = path.find_last_of(L"\\/");
        if (lastSlash != std::wstring::npos) {
            return path.substr(lastSlash + 1);
        }
        return path;
    }
    CloseHandle(hProcess);
    return {};
}

namespace {

inline wchar_t ToLowerAscii(wchar_t c) {
    if (c >= L'A' && c <= L'Z') return c + (L'a' - L'A');
    if (c > 127) return static_cast<wchar_t>(std::towlower(c));
    return c;
}

inline bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (ToLowerAscii(a[i]) != ToLowerAscii(b[i])) return false;
    }
    return true;
}

} // namespace

bool SmartPasteManager::IsEditorExecutable(std::wstring_view exeName) {
    if (exeName.empty()) {
        return false;
    }
    static const std::vector<std::wstring_view> kEditorExes = {
        L"code.exe", L"devenv.exe", L"idea64.exe", L"clion64.exe", L"pycharm64.exe",
        L"webstorm64.exe", L"rider64.exe", L"goland64.exe", L"sublime_text.exe",
        L"notepad++.exe", L"windowsterminal.exe", L"cmd.exe", L"powershell.exe",
        L"pwsh.exe", L"alacritty.exe", L"kitty.exe", L"wezterm-gui.exe", L"neovide.exe"
    };
    for (auto editor : kEditorExes) {
        if (exeName.size() == editor.size()) {
            bool match = true;
            for (size_t i = 0; i < exeName.size(); ++i) {
                wchar_t ca = exeName[i];
                wchar_t cb = editor[i];
                if (ca != cb && static_cast<wchar_t>(std::towlower(ca)) != cb) {
                    match = false;
                    break;
                }
            }
            if (match) {
                return true;
            }
        }
    }
    return false;
}

bool SmartPasteManager::IsEditorProcess(DWORD processId, HWND hwnd) {
    std::wstring exeName = GetProcessExecutableName(processId);
    if (!exeName.empty() && IsEditorExecutable(exeName)) {
        return true;
    }

    if (hwnd) {
        wchar_t className[128] = {};
        if (GetClassNameW(hwnd, className, 128) > 0) {
            std::wstring cls(className);
            if (cls.starts_with(L"Scintilla") ||
                (cls.find(L"Chrome_WidgetWin_1") != std::wstring::npos && exeName.find(L"code") != std::wstring::npos)) {
                return true;
            }
        }
    }
    return false;
}

bool SmartPasteManager::IsSensitiveOrSecret(std::wstring_view text) {
    if (text.empty()) {
        return false;
    }

    // 1. Private keys
    if (text.find(L"-----BEGIN ") != std::wstring_view::npos &&
        (text.find(L"PRIVATE KEY-----") != std::wstring_view::npos ||
         text.find(L"KEY BLOCK-----") != std::wstring_view::npos)) {
        return true;
    }

    // Helper: checks if character is a valid boundary before/after a credential token
    auto isTokenBoundaryBefore = [](std::wstring_view str, size_t pos) {
        if (pos == 0) return true;
        const wchar_t prev = str[pos - 1];
        return !iswalnum(prev) && prev != L'_' && prev != L'-';
    };

    auto isTokenBoundaryAfter = [](std::wstring_view str, size_t endPos) {
        if (endPos >= str.size()) return true;
        const wchar_t next = str[endPos];
        return !iswalnum(next) && next != L'_' && next != L'-';
    };

    // Helper: checks if text contains a specific token/prefix followed by alphanumeric chars
    auto hasPrefixedToken = [&](std::wstring_view str, std::wstring_view prefix, size_t minTotalLen) {
        size_t pos = 0;
        while ((pos = str.find(prefix, pos)) != std::wstring_view::npos) {
            if (isTokenBoundaryBefore(str, pos)) {
                size_t end = pos + prefix.size();
                while (end < str.size() && (iswalnum(str[end]) || str[end] == L'_' || str[end] == L'-')) {
                    ++end;
                }
                if (end - pos >= minTotalLen && isTokenBoundaryAfter(str, end)) {
                    return true;
                }
            }
            pos += prefix.size();
        }
        return false;
    };

    // 2. OpenAI API keys: sk-, sk-proj-, sk-org-
    if (hasPrefixedToken(text, L"sk-", 24) ||
        hasPrefixedToken(text, L"sk-proj-", 28) ||
        hasPrefixedToken(text, L"sk-org-", 28)) {
        return true;
    }

    // 3. Google Gemini / Cloud API keys: AIzaSy
    if (hasPrefixedToken(text, L"AIzaSy", 30)) {
        return true;
    }

    // 4. GitHub tokens: ghp_, gho_, ghu_, ghs_, ghr_ (length 40)
    if (hasPrefixedToken(text, L"ghp_", 40) ||
        hasPrefixedToken(text, L"gho_", 40) ||
        hasPrefixedToken(text, L"ghu_", 40) ||
        hasPrefixedToken(text, L"ghs_", 40) ||
        hasPrefixedToken(text, L"ghr_", 40)) {
        return true;
    }

    // 5. GitLab tokens: glpat- (min length 26)
    if (hasPrefixedToken(text, L"glpat-", 26)) {
        return true;
    }

    // 6. Slack tokens: xoxb-, xoxp-, xoxa-, xoxr- (min length 20)
    if (hasPrefixedToken(text, L"xoxb-", 20) ||
        hasPrefixedToken(text, L"xoxp-", 20) ||
        hasPrefixedToken(text, L"xoxa-", 20) ||
        hasPrefixedToken(text, L"xoxr-", 20)) {
        return true;
    }

    // 7. AWS Access Key ID: AKIA[0-9A-Z]{16}
    {
        size_t pos = 0;
        while ((pos = text.find(L"AKIA", pos)) != std::wstring_view::npos) {
            if (isTokenBoundaryBefore(text, pos)) {
                if (pos + 20 <= text.size()) {
                    bool allAlnumUpper = true;
                    for (size_t i = pos + 4; i < pos + 20; ++i) {
                        wchar_t ch = text[i];
                        if (!((ch >= L'0' && ch <= L'9') || (ch >= L'A' && ch <= L'Z'))) {
                            allAlnumUpper = false;
                            break;
                        }
                    }
                    if (allAlnumUpper && isTokenBoundaryAfter(text, pos + 20)) {
                        return true;
                    }
                }
            }
            pos += 4;
        }
    }

    wchar_t stackBuffer[1024];
    std::wstring dynamicBuffer;
    std::wstring_view lowerText;

    if (text.size() <= 1024) {
        for (size_t i = 0; i < text.size(); ++i) {
            stackBuffer[i] = ToLowerAscii(text[i]);
        }
        lowerText = std::wstring_view(stackBuffer, text.size());
    } else {
        dynamicBuffer.reserve(text.size());
        for (wchar_t c : text) {
            dynamicBuffer.push_back(ToLowerAscii(c));
        }
        lowerText = dynamicBuffer;
    }

    // 8. Authorization header structural parser (handles arbitrary whitespace, quotes, and 1-char tokens)
    // Examples:
    // - Authorization: Bearer admin
    // - "Authorization": "Bearer a"
    // - Use "Authorization":                      "Bearer a" for this request.
    // - 'authorization' : 'bearer 1'
    {
        size_t authPos = 0;
        while ((authPos = lowerText.find(L"authorization", authPos)) != std::wstring::npos) {
            bool isQuotedAuth = false;
            wchar_t quoteChar = 0;
            if (authPos > 0 && (lowerText[authPos - 1] == L'"' || lowerText[authPos - 1] == L'\'')) {
                isQuotedAuth = true;
                quoteChar = lowerText[authPos - 1];
            }
            if (authPos == 0 || isQuotedAuth || (!iswalnum(lowerText[authPos - 1]) && lowerText[authPos - 1] != L'_' && lowerText[authPos - 1] != L'-')) {
                size_t afterAuth = authPos + 13;
                if (isQuotedAuth) {
                    if (afterAuth < lowerText.size() && lowerText[afterAuth] == quoteChar) {
                        ++afterAuth;
                    }
                } else if (afterAuth < lowerText.size() && (lowerText[afterAuth] == L'"' || lowerText[afterAuth] == L'\'')) {
                    ++afterAuth;
                }
                while (afterAuth < lowerText.size() && iswspace(lowerText[afterAuth])) {
                    ++afterAuth;
                }
                if (afterAuth < lowerText.size() && (lowerText[afterAuth] == L':' || lowerText[afterAuth] == L'=')) {
                    ++afterAuth;
                    // Skip arbitrary whitespace / newlines
                    while (afterAuth < lowerText.size() && iswspace(lowerText[afterAuth])) {
                        ++afterAuth;
                    }
                    // Optional opening quote for header value
                    if (afterAuth < lowerText.size() && (lowerText[afterAuth] == L'"' || lowerText[afterAuth] == L'\'')) {
                        ++afterAuth;
                    }
                    while (afterAuth < lowerText.size() && iswspace(lowerText[afterAuth])) {
                        ++afterAuth;
                    }
                    // Optional scheme, e.g. "bearer"
                    if (afterAuth + 6 <= lowerText.size() && lowerText.substr(afterAuth, 6) == L"bearer") {
                        afterAuth += 6;
                        while (afterAuth < lowerText.size() && iswspace(lowerText[afterAuth])) {
                            ++afterAuth;
                        }
                    }
                    // Optional quote for token itself (and whitespace inside token quote, e.g. Bearer "\n\ta")
                    if (afterAuth < lowerText.size() && (lowerText[afterAuth] == L'"' || lowerText[afterAuth] == L'\'')) {
                        ++afterAuth;
                        while (afterAuth < lowerText.size() && iswspace(lowerText[afterAuth])) {
                            ++afterAuth;
                        }
                    }
                    // Nonempty token
                    size_t tokEnd = afterAuth;
                    while (tokEnd < text.size()) {
                        wchar_t ch = text[tokEnd];
                        if (iswalnum(ch) || ch == L'_' || ch == L'-' || ch == L'.' ||
                            ch == L'=' || ch == L'+' || ch == L'/' || ch == L'~') {
                            ++tokEnd;
                        } else {
                            break;
                        }
                    }
                    if (tokEnd > afterAuth) {
                        return true;
                    }
                }
            }
            authPos += 13;
        }
    }

    // 8b. Bearer tokens: case-insensitive scheme matching, complete RFC 6750 token alphabet
    // Token alphabet: ALPHA / DIGIT / "-" / "." / "_" / "~" / "+" / "/" and optional "=" padding
    {
        size_t pos = 0;
        while ((pos = lowerText.find(L"bearer", pos)) != std::wstring::npos) {
            if (isTokenBoundaryBefore(lowerText, pos)) {
                size_t afterScheme = pos + 6;
                if (afterScheme < lowerText.size() && iswspace(lowerText[afterScheme])) {
                    while (afterScheme < lowerText.size() && iswspace(lowerText[afterScheme])) {
                        ++afterScheme;
                    }
                    if (afterScheme < lowerText.size() && (lowerText[afterScheme] == L'"' || lowerText[afterScheme] == L'\'')) {
                        ++afterScheme;
                        while (afterScheme < lowerText.size() && iswspace(lowerText[afterScheme])) {
                            ++afterScheme;
                        }
                    }
                    size_t tokenStart = afterScheme;
                    size_t end = tokenStart;
                    bool hasSpecialOrDigit = false;
                    while (end < text.size()) {
                        wchar_t ch = text[end];
                        if (iswalnum(ch) || ch == L'_' || ch == L'-' || ch == L'.' ||
                            ch == L'=' || ch == L'+' || ch == L'/' || ch == L'~') {
                            if (iswdigit(ch) || ch == L'_' || ch == L'-' || ch == L'.' ||
                                ch == L'=' || ch == L'+' || ch == L'/' || ch == L'~') {
                                hasSpecialOrDigit = true;
                            }
                            ++end;
                        } else {
                            break;
                        }
                    }
                    const size_t tokenLen = end - tokenStart;
                    if (tokenLen > 0) {
                        bool inAuthContext = false;
                        size_t beforePos = pos;
                        while (beforePos > 0 && (iswspace(lowerText[beforePos - 1]) ||
                                                 lowerText[beforePos - 1] == L'"' ||
                                                 lowerText[beforePos - 1] == L'\'')) {
                            --beforePos;
                        }
                        if (beforePos > 0 && (lowerText[beforePos - 1] == L':' || lowerText[beforePos - 1] == L'=')) {
                            inAuthContext = true;
                        }

                        if (inAuthContext ||
                            (tokenLen >= 6 && hasSpecialOrDigit) ||
                            tokenLen >= 8) {
                            return true;
                        }
                    }
                }
            }
            pos += 6;
        }
    }

    // 9. JWT: iterate all candidate occurrences of eyJ
    {
        size_t pos = 0;
        while ((pos = text.find(L"eyJ", pos)) != std::wstring_view::npos) {
            if (pos == 0 || !iswalnum(text[pos - 1])) {
                size_t dotCount = 0;
                size_t end = pos;
                while (end < text.size() && (iswalnum(text[end]) || text[end] == L'_' || text[end] == L'-' || text[end] == L'=' || text[end] == L'.')) {
                    if (text[end] == L'.') {
                        ++dotCount;
                    }
                    ++end;
                }
                if (dotCount >= 2 && (end - pos) >= 30) {
                    return true;
                }
            }
            pos += 3;
        }
    }

    // 10. Database connection strings with credentials
    static const std::vector<std::wstring_view> kConnPrefixes = {
        L"postgres://", L"postgresql://", L"mysql://", L"mongodb://", L"mongodb+srv://", L"redis://"
    };
    for (auto cp : kConnPrefixes) {
        size_t pos = text.find(cp);
        if (pos != std::wstring_view::npos) {
            size_t atPos = text.find(L'@', pos);
            size_t colonPos = text.find(L':', pos + cp.size());
            if (colonPos != std::wstring_view::npos && atPos != std::wstring_view::npos && colonPos < atPos) {
                return true;
            }
        }
    }

    // 11. Key-value secret definitions (case-insensitive, supporting quoted/unquoted keys and values)
    static const std::vector<std::wstring_view> kPassPrefixes = {
        L"password", L"passwd", L"api_key", L"apikey", L"secret_key", L"private_key",
        L"auth_token", L"access_token", L"client_secret", L"secret_access_key",
        L"aws_secret_access_key", L"aws_session_token", L"encryption_key"
    };

    for (auto pp : kPassPrefixes) {
        size_t pPos = 0;
        while ((pPos = lowerText.find(pp, pPos)) != std::wstring::npos) {
            // Check boundary before: must be boundary or quote
            bool isQuotedKey = false;
            wchar_t quoteChar = 0;
            if (pPos > 0 && (lowerText[pPos - 1] == L'"' || lowerText[pPos - 1] == L'\'')) {
                isQuotedKey = true;
                quoteChar = lowerText[pPos - 1];
            }
            if (pPos == 0 || isQuotedKey || (!iswalnum(lowerText[pPos - 1]) && lowerText[pPos - 1] != L'_' && lowerText[pPos - 1] != L'-')) {
                size_t after = pPos + pp.size();
                // Consume matching closing quote if key was quoted, or single/double quote
                if (isQuotedKey) {
                    if (after < lowerText.size() && lowerText[after] == quoteChar) {
                        ++after;
                    }
                } else if (after < lowerText.size() && (lowerText[after] == L'"' || lowerText[after] == L'\'')) {
                    ++after;
                }

                while (after < lowerText.size() && iswspace(lowerText[after])) {
                    ++after;
                }
                if (after < lowerText.size() && (lowerText[after] == L'=' || lowerText[after] == L':')) {
                    const wchar_t sep = lowerText[after];
                    ++after;

                    size_t newlineCount = 0;
                    size_t lastNewlinePos = std::wstring::npos;
                    while (after < lowerText.size() && iswspace(lowerText[after])) {
                        if (lowerText[after] == L'\n') {
                            ++newlineCount;
                            lastNewlinePos = after;
                        }
                        ++after;
                    }

                    if (after < lowerText.size()) {
                        bool validPlacement = true;
                        if (newlineCount > 0) {
                            if (sep == L':') {
                                // For JSON-style credential pairs across newlines:
                                // if the key is quoted (e.g. "password":\n"hunter2") or the value is quoted (e.g. password:\n"hunter2"),
                                // JSON syntax allows unindented placement. Only unquoted YAML scalars require indentation!
                                const bool isJsonStyle = isQuotedKey || (lowerText[after] == L'"' || lowerText[after] == L'\'');
                                if (!isJsonStyle) {
                                    size_t keyLineStart = pPos;
                                    while (keyLineStart > 0 && lowerText[keyLineStart - 1] != L'\n') {
                                        --keyLineStart;
                                    }
                                    size_t keyIndentPos = keyLineStart;
                                    while (keyIndentPos < pPos && (lowerText[keyIndentPos] == L' ' || lowerText[keyIndentPos] == L'\t')) {
                                        ++keyIndentPos;
                                    }
                                    size_t keyIndent = keyIndentPos - keyLineStart;
                                    size_t valIndent = after - (lastNewlinePos + 1);
                                    if (valIndent <= keyIndent) {
                                        validPlacement = false;
                                    }
                                }
                            } else {
                                // For '=', newline immediately after '=' without continuation means empty value,
                                // unless value on the next line is quoted: e.g. PASSWORD=\n"hunter2"
                                if (lowerText[after] != L'"' && lowerText[after] != L'\'') {
                                    validPlacement = false;
                                }
                            }
                        }

                        if (validPlacement) {
                            if (lowerText[after] == L'"' || lowerText[after] == L'\'') {
                                wchar_t valQuote = lowerText[after];
                                size_t closing = lowerText.find(valQuote, after + 1);
                                if (closing != std::wstring::npos) {
                                    // Check if there is any non-whitespace content inside the quotes
                                    bool hasContentInside = false;
                                    for (size_t i = after + 1; i < closing; ++i) {
                                        if (!iswspace(lowerText[i])) {
                                            hasContentInside = true;
                                            break;
                                        }
                                    }
                                    if (hasContentInside) {
                                        return true;
                                    }
                                    // Demonstrably empty quoted string: "" or '' with only whitespace
                                } else {
                                    // Incomplete quoted assignment or raw value beginning with quote (e.g. PASSWORD='s3cr3t or PASSWORD="s3cr3t)
                                    // Conservatively treat as sensitive if there is any non-whitespace character after the opening quote
                                    bool hasContentAfterQuote = false;
                                    size_t qScan = after + 1;
                                    while (qScan < lowerText.size() && lowerText[qScan] != L'\r' && lowerText[qScan] != L'\n') {
                                        if (!iswspace(lowerText[qScan])) {
                                            hasContentAfterQuote = true;
                                            break;
                                        }
                                        ++qScan;
                                    }
                                    if (hasContentAfterQuote) {
                                        return true;
                                    }
                                    // A lone quote at the end of the line is demonstrably empty
                                }
                            } else {
                                // Nonempty unquoted credential value (including punctuation-leading values like
                                // ',s3cr3t', ']secret', '}secret', ';secret', or values consisting
                                // entirely of periods like '...') following recognized credential keys
                                // is conservatively treated as sensitive to prevent cloud leakage.
                                return true;
                            }
                        }
                    }
                }
            }
            pPos += pp.size();
        }
    }

    return false;
}

bool SmartPasteManager::IsSourceCode(std::wstring_view text, bool isEditorContext) {
    if (text.empty()) {
        return false;
    }

    size_t start = 0;
    while (start < text.size() && iswspace(text[start])) {
        ++start;
    }
    std::wstring_view trimmed = text.substr(start);
    if (trimmed.empty()) {
        return false;
    }

    // 1. Shebang
    if (trimmed.starts_with(L"#!")) {
        return true;
    }

    // 2. Markdown code block
    if (trimmed.starts_with(L"```")) {
        return true;
    }

    // 3. HTML/XML documents
    if (trimmed.starts_with(L"<!DOCTYPE") || trimmed.starts_with(L"<?xml") || trimmed.starts_with(L"<html")) {
        return true;
    }

    // 4. C/C++ Preprocessor / includes
    if (trimmed.starts_with(L"#include") || trimmed.starts_with(L"#define") ||
        trimmed.starts_with(L"#pragma") || trimmed.starts_with(L"#ifdef") ||
        trimmed.starts_with(L"#ifndef") || trimmed.starts_with(L"#if ") ||
        trimmed.starts_with(L"#elif") || trimmed.starts_with(L"#endif") ||
        trimmed.starts_with(L"using namespace ")) {
        return true;
    }

    // 5. Function and class definitions in common languages (Python, Rust, Go, Kotlin)
    if ((trimmed.starts_with(L"def ") || trimmed.starts_with(L"async def ") ||
         trimmed.starts_with(L"class ")) &&
        trimmed.find(L':') != std::wstring_view::npos) {
        return true;
    }
    if ((trimmed.starts_with(L"fn ") || trimmed.starts_with(L"func ") ||
         trimmed.starts_with(L"pub fn ") || trimmed.starts_with(L"fun ")) &&
        trimmed.find(L'(') != std::wstring_view::npos) {
        return true;
    }

    // 6. Standalone function and method calls (e.g. console.log("..."), print("..."), send_message(...), obj.method(...))
    // In an editor context, recognize one or more consecutive standalone identifier-based
    // function/method calls (even without trailing semicolons), while strictly distinguishing from surrounding prose.
    // In non-editor context, require statements to end with semicolons or have common code prefixes.
    {
        size_t cur = 0;
        int statementCount = 0;
        bool allStatementsAreCalls = true;
        bool allStatementsHaveSemicolon = true;
        bool hasRecognizedPrefix = false;

        static const std::vector<std::wstring_view> kCommonCallPrefixes = {
            L"console.", L"print(", L"printf(", L"println(", L"println!(",
            L"logging.", L"logger.", L"System.out.", L"std::cout", L"std::cin"
        };

        while (cur < trimmed.size()) {
            // Skip leading whitespace / newlines
            while (cur < trimmed.size() && iswspace(trimmed[cur])) {
                ++cur;
            }
            if (cur >= trimmed.size()) break;

            // Skip comment line
            if (trimmed.substr(cur).starts_with(L"//") || trimmed.substr(cur).starts_with(L"#")) {
                while (cur < trimmed.size() && trimmed[cur] != L'\n') {
                    ++cur;
                }
                continue;
            }

            // Expect identifier or dot/scope-separated identifier (e.g. send_message, obj.method, System.out.println)
            if (!iswalpha(trimmed[cur]) && trimmed[cur] != L'_') {
                allStatementsAreCalls = false;
                break;
            }

            size_t idStart = cur;
            while (cur < trimmed.size() && (iswalnum(trimmed[cur]) || trimmed[cur] == L'_' || trimmed[cur] == L'.' || trimmed[cur] == L':')) {
                ++cur;
            }
            std::wstring_view idSpan = trimmed.substr(idStart, cur - idStart);
            for (auto pfx : kCommonCallPrefixes) {
                if (idSpan.starts_with(pfx) || (pfx.ends_with(L"(") && idSpan == pfx.substr(0, pfx.size() - 1))) {
                    hasRecognizedPrefix = true;
                    break;
                }
            }

            // Skip whitespace before '('
            while (cur < trimmed.size() && (trimmed[cur] == L' ' || trimmed[cur] == L'\t')) {
                ++cur;
            }

            if (cur >= trimmed.size() || trimmed[cur] != L'(') {
                allStatementsAreCalls = false;
                break;
            }

            // Parse balanced parentheses respecting strings and escapes
            int depth = 0;
            size_t closeParenPos = std::wstring_view::npos;
            bool inQuote = false;
            wchar_t quoteChar = 0;
            bool escaped = false;
            for (size_t i = cur; i < trimmed.size(); ++i) {
                wchar_t ch = trimmed[i];
                if (escaped) {
                    escaped = false;
                    continue;
                }
                if (ch == L'\\') {
                    escaped = true;
                    continue;
                }
                if (inQuote) {
                    if (ch == quoteChar) inQuote = false;
                    continue;
                }
                if (ch == L'"' || ch == L'\'') {
                    inQuote = true;
                    quoteChar = ch;
                    continue;
                }
                if (ch == L'(') {
                    ++depth;
                } else if (ch == L')') {
                    --depth;
                    if (depth == 0) {
                        closeParenPos = i;
                        break;
                    }
                }
            }

            if (closeParenPos == std::wstring_view::npos) {
                allStatementsAreCalls = false;
                break;
            }

            cur = closeParenPos + 1;
            // Skip inline whitespace
            while (cur < trimmed.size() && (trimmed[cur] == L' ' || trimmed[cur] == L'\t')) {
                ++cur;
            }
            bool hadSemicolon = false;
            if (cur < trimmed.size() && trimmed[cur] == L';') {
                hadSemicolon = true;
                ++cur;
            } else {
                allStatementsHaveSemicolon = false;
            }
            // Skip trailing comment on the same line
            while (cur < trimmed.size() && (trimmed[cur] == L' ' || trimmed[cur] == L'\t')) {
                ++cur;
            }
            if (cur < trimmed.size() && (trimmed.substr(cur).starts_with(L"//") || trimmed.substr(cur).starts_with(L"#"))) {
                while (cur < trimmed.size() && trimmed[cur] != L'\n') {
                    ++cur;
                }
            }
            // Next char must be newline, carriage return, end of string, OR if a semicolon was consumed, another statement may follow!
            if (cur < trimmed.size() && trimmed[cur] != L'\r' && trimmed[cur] != L'\n') {
                if (!hadSemicolon) {
                    allStatementsAreCalls = false;
                    break;
                }
            }

            ++statementCount;
        }

        if (allStatementsAreCalls && statementCount >= 1) {
            if (isEditorContext || allStatementsHaveSemicolon || hasRecognizedPrefix) {
                return true;
            }
        }
    }

    // 7. JSON detection
    size_t end = trimmed.size();
    while (end > 0 && iswspace(trimmed[end - 1])) {
        --end;
    }
    std::wstring_view jsonCandidate = trimmed.substr(0, end);
    if ((jsonCandidate.starts_with(L"{") && jsonCandidate.ends_with(L"}")) ||
        (jsonCandidate.starts_with(L"[") && jsonCandidate.ends_with(L"]"))) {
        if (jsonCandidate.find(L':') != std::wstring_view::npos || jsonCandidate.find(L',') != std::wstring_view::npos) {
            try {
                json::Value v = json::Parse(jsonCandidate);
                if (v.is_object() || v.is_array()) {
                    return true;
                }
            } catch (...) {
            }
        }
    }

    // 7. SQL queries
    auto startsWithCi = [](std::wstring_view s, std::wstring_view prefix) {
        if (s.size() < prefix.size()) return false;
        for (size_t i = 0; i < prefix.size(); ++i) {
            if (std::towupper(s[i]) != std::towupper(prefix[i])) return false;
        }
        return true;
    };
    if (startsWithCi(trimmed, L"SELECT ") || startsWithCi(trimmed, L"INSERT INTO ") ||
        startsWithCi(trimmed, L"CREATE TABLE ") || startsWithCi(trimmed, L"UPDATE ")) {
        std::wstring upper;
        upper.reserve(trimmed.size());
        for (wchar_t c : trimmed) upper.push_back(static_cast<wchar_t>(std::towupper(c)));
        if (upper.find(L" FROM ") != std::wstring::npos || upper.find(L" WHERE ") != std::wstring::npos ||
            upper.find(L" VALUES ") != std::wstring::npos || upper.find(L" SET ") != std::wstring::npos) {
            return true;
        }
    }

    // 8. Language keyword declarations
    static const std::vector<std::wstring_view> kStrongCodePatterns = {
        L"int main(", L"void main(", L"public static void main",
        L"def ", L"fn ", L"func ", L"package main",
        L"class ", L"struct ", L"impl ", L"namespace ",
        L"const ", L"let ", L"var ", L"function ", L"import ", L"export "
    };

    auto hasKeywordAtStatementPosition = [](std::wstring_view str, std::wstring_view kw) {
        size_t pos = 0;
        while ((pos = str.find(kw, pos)) != std::wstring_view::npos) {
            size_t before = pos;
            while (before > 0 && (str[before - 1] == L' ' || str[before - 1] == L'\t')) {
                --before;
            }
            if (before == 0 || str[before - 1] == L'\n' || str[before - 1] == L';' ||
                str[before - 1] == L'{' || str[before - 1] == L'}') {
                return true;
            }
            pos += kw.size();
        }
        return false;
    };

    int codeFeatureScore = 0;
    for (auto pattern : kStrongCodePatterns) {
        if (hasKeywordAtStatementPosition(trimmed, pattern)) {
            if (trimmed.find(L'(') != std::wstring_view::npos ||
                trimmed.find(L'{') != std::wstring_view::npos ||
                trimmed.find(L';') != std::wstring_view::npos ||
                trimmed.find(L':') != std::wstring_view::npos) {
                codeFeatureScore += 2;
                break;
            }
        }
    }

    // 9. Semicolon at end of line and bracket density
    size_t lineCount = 1;
    size_t semicolonsAtLineEnd = 0;
    size_t curlyBraces = 0;
    size_t codeOperators = 0;

    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\n') {
            ++lineCount;
            size_t prev = i;
            while (prev > 0 && iswspace(text[prev - 1])) {
                --prev;
            }
            if (prev > 0 && text[prev - 1] == L';') {
                ++semicolonsAtLineEnd;
            }
        } else if (text[i] == L'{' || text[i] == L'}') {
            ++curlyBraces;
        }
    }
    if (!text.empty()) {
        size_t prev = text.size();
        while (prev > 0 && iswspace(text[prev - 1])) {
            --prev;
        }
        if (prev > 0 && text[prev - 1] == L';') {
            ++semicolonsAtLineEnd;
        }
    }

    auto getEffectiveLineEnd = [](std::wstring_view str, size_t pos) -> size_t {
        size_t lineStart = pos;
        while (lineStart > 0 && str[lineStart - 1] != L'\n') {
            --lineStart;
        }
        size_t rawLineEnd = str.find(L'\n', pos);
        if (rawLineEnd == std::wstring_view::npos) rawLineEnd = str.size();

        bool inQuote = false;
        wchar_t quoteChar = 0;
        bool escaped = false;
        size_t commentStart = rawLineEnd;

        for (size_t i = lineStart; i < rawLineEnd; ++i) {
            wchar_t ch = str[i];
            if (escaped) {
                escaped = false;
                continue;
            }
            if (ch == L'\\') {
                escaped = true;
                continue;
            }
            if (inQuote) {
                if (ch == quoteChar) inQuote = false;
                continue;
            }
            if (ch == L'"' || ch == L'\'') {
                inQuote = true;
                quoteChar = ch;
                continue;
            }
            // Outside quotes: check for comments
            if (i + 1 < rawLineEnd && str[i] == L'/' && (str[i + 1] == L'/' || str[i + 1] == L'*')) {
                commentStart = i;
                break;
            }
            if (ch == L'#') {
                commentStart = i;
                break;
            }
        }

        size_t effectiveEnd = commentStart;
        while (effectiveEnd > lineStart && iswspace(str[effectiveEnd - 1])) {
            --effectiveEnd;
        }
        return effectiveEnd;
    };

    auto getStatementStart = [](std::wstring_view str, size_t pos) -> size_t {
        size_t lineStart = pos;
        while (lineStart > 0 && str[lineStart - 1] != L'\n') {
            --lineStart;
        }

        bool inQuote = false;
        wchar_t quoteChar = 0;
        bool escaped = false;
        size_t stmtStart = lineStart;

        for (size_t i = lineStart; i < pos; ++i) {
            wchar_t ch = str[i];
            if (escaped) {
                escaped = false;
                continue;
            }
            if (ch == L'\\') {
                escaped = true;
                continue;
            }
            if (inQuote) {
                if (ch == quoteChar) inQuote = false;
                continue;
            }
            if (ch == L'"' || ch == L'\'') {
                inQuote = true;
                quoteChar = ch;
                continue;
            }
            // Outside quotes: statement terminators or block delimiters
            if (ch == L';' || ch == L'{' || ch == L'}') {
                stmtStart = i + 1;
            }
        }

        while (stmtStart < pos && iswspace(str[stmtStart])) {
            ++stmtStart;
        }
        return stmtStart;
    };

    auto hasOperatorAtStatementPosition = [&](std::wstring_view str, std::wstring_view op) {
        size_t pos = 0;
        while ((pos = str.find(op, pos)) != std::wstring_view::npos) {
            size_t stmtStart = getStatementStart(str, pos);
            size_t lineEnd = getEffectiveLineEnd(str, pos);
            // Code line must end with ';', '{', '}', or ')'
            if (lineEnd > pos && (str[lineEnd - 1] == L';' || str[lineEnd - 1] == L'{' || str[lineEnd - 1] == L'}' || str[lineEnd - 1] == L')')) {
                // Examine prefix before operator from statement start
                std::wstring_view prefix = str.substr(stmtStart, pos - stmtStart);
                // If there's an assignment '=', take the expression on the right of '=':
                // e.g. "fn = x => x + 1;" or "res = ptr -> method();"
                size_t eqPos = prefix.rfind(L'=');
                if (eqPos != std::wstring_view::npos && eqPos > 0 &&
                    prefix[eqPos - 1] != L'=' && prefix[eqPos - 1] != L'!' &&
                    prefix[eqPos - 1] != L'<' && prefix[eqPos - 1] != L'>') {
                    prefix = prefix.substr(eqPos + 1);
                } else {
                    // Check if prefixed by declaration keyword
                    static const std::vector<std::wstring_view> kDeclPrefixes = {
                        L"const ", L"let ", L"var ", L"auto ", L"return ", L"case "
                    };
                    for (auto dp : kDeclPrefixes) {
                        if (prefix.starts_with(dp)) {
                            prefix = prefix.substr(dp.size());
                            break;
                        }
                    }
                }
                // Trim whitespace from operand before operator
                size_t opStart = 0;
                while (opStart < prefix.size() && iswspace(prefix[opStart])) ++opStart;
                size_t opEnd = prefix.size();
                while (opEnd > opStart && iswspace(prefix[opEnd - 1])) --opEnd;
                std::wstring_view operandBefore = prefix.substr(opStart, opEnd - opStart);

                if (!operandBefore.empty()) {
                    bool validBefore = false;
                    if (operandBefore.front() == L'(' && operandBefore.back() == L')') {
                        validBefore = true; // e.g. (x) => ...
                    } else {
                        // Must be a valid identifier or chain: e.g. "ptr", "x", "std", "this"
                        bool allIdChars = true;
                        for (wchar_t ch : operandBefore) {
                            if (!iswalnum(ch) && ch != L'_' && ch != L'~' && ch != L'*' && ch != L'&' && ch != L'.' && ch != L':' && !iswspace(ch)) {
                                allIdChars = false;
                                break;
                            }
                        }
                        if (allIdChars) {
                            // Check that it's not multiple space-separated words (prose like "Please replace")
                            bool hasSpaceSeparatedWords = false;
                            for (size_t i = 1; i + 1 < operandBefore.size(); ++i) {
                                if (iswspace(operandBefore[i]) && iswalnum(operandBefore[i - 1])) {
                                    size_t nextNonSpace = i;
                                    while (nextNonSpace < operandBefore.size() && iswspace(operandBefore[nextNonSpace])) ++nextNonSpace;
                                    if (nextNonSpace < operandBefore.size() && iswalnum(operandBefore[nextNonSpace])) {
                                        hasSpaceSeparatedWords = true;
                                        break;
                                    }
                                }
                            }
                            if (!hasSpaceSeparatedWords) {
                                validBefore = true;
                            }
                        }
                    }

                    if (validBefore) {
                        // Check operand after operator (allowing legal leading whitespace)
                        size_t afterOp = pos + op.size();
                        while (afterOp < lineEnd && iswspace(str[afterOp])) ++afterOp;
                        if (afterOp < lineEnd) {
                            wchar_t nextCh = str[afterOp];
                            if (iswalnum(nextCh) || nextCh == L'_' || nextCh == L'~' || nextCh == L'*' || nextCh == L'&' || nextCh == L'(' || nextCh == L'{') {
                                return true;
                            }
                        }
                    }
                }
            }
            pos += op.size();
        }
        return false;
    };

    if (hasOperatorAtStatementPosition(text, L"->")) codeFeatureScore += 2;
    if (hasOperatorAtStatementPosition(text, L"::")) codeFeatureScore += 2;
    if (hasOperatorAtStatementPosition(text, L"=>")) codeFeatureScore += 2;

    auto hasComparisonOperatorAtStatementPosition = [&](std::wstring_view str, std::wstring_view op) {
        size_t pos = 0;
        while ((pos = str.find(op, pos)) != std::wstring_view::npos) {
            size_t stmtStart = getStatementStart(str, pos);
            size_t lineEnd = getEffectiveLineEnd(str, pos);
            // Code statement line must end with ';', '{', '}', or ')'
            if (lineEnd > pos && (str[lineEnd - 1] == L';' || str[lineEnd - 1] == L'{' || str[lineEnd - 1] == L'}' || str[lineEnd - 1] == L')')) {
                // Examine prefix before operator from statement start
                std::wstring_view linePrefix = str.substr(stmtStart, pos - stmtStart);
                // Recognize assignment expressions: e.g. "matches = left == right;" or "bool ok = (a == b);"
                size_t eqPos = linePrefix.rfind(L'=');
                if (eqPos != std::wstring_view::npos && eqPos > 0 &&
                    linePrefix[eqPos - 1] != L'=' && linePrefix[eqPos - 1] != L'!' &&
                    linePrefix[eqPos - 1] != L'<' && linePrefix[eqPos - 1] != L'>') {
                    std::wstring_view assignTarget = linePrefix.substr(0, eqPos);
                    while (!assignTarget.empty() && iswspace(assignTarget.back())) assignTarget.remove_suffix(1);
                    while (!assignTarget.empty() && iswspace(assignTarget.front())) assignTarget.remove_prefix(1);
                    bool validTarget = !assignTarget.empty();
                    for (wchar_t ch : assignTarget) {
                        if (!iswalnum(ch) && ch != L'_' && ch != L'[' && ch != L']' && ch != L'.' && ch != L'-' && ch != L'>' && !iswspace(ch)) {
                            validTarget = false;
                            break;
                        }
                    }
                    if (validTarget) {
                        linePrefix = linePrefix.substr(eqPos + 1);
                    }
                } else {
                    // Check control / declaration keywords
                    static const std::vector<std::wstring_view> kStmtPrefixes = {
                        L"if (", L"if(", L"while (", L"while(", L"for (", L"for(",
                        L"return ", L"assert(", L"require(", L"expect(",
                        L"EXPECT_TRUE(", L"EXPECT_FALSE(", L"EXPECT_EQ(", L"EXPECT_NE(",
                        L"ASSERT_TRUE(", L"ASSERT_FALSE(", L"ASSERT_EQ(", L"ASSERT_NE(",
                        L"bool ", L"auto ", L"let ", L"const ", L"var ", L"case "
                    };
                    for (auto sp : kStmtPrefixes) {
                        if (linePrefix.starts_with(sp)) {
                            linePrefix = linePrefix.substr(sp.size());
                            break;
                        }
                    }
                }

                // Trim whitespace and outer parentheses from operand before operator
                size_t opStart = 0;
                while (opStart < linePrefix.size() && iswspace(linePrefix[opStart])) ++opStart;
                size_t opEnd = linePrefix.size();
                while (opEnd > opStart && iswspace(linePrefix[opEnd - 1])) --opEnd;
                std::wstring_view operandBefore = linePrefix.substr(opStart, opEnd - opStart);

                while (!operandBefore.empty() && operandBefore.front() == L'(') operandBefore.remove_prefix(1);
                while (!operandBefore.empty() && operandBefore.back() == L')') operandBefore.remove_suffix(1);
                while (!operandBefore.empty() && iswspace(operandBefore.front())) operandBefore.remove_prefix(1);
                while (!operandBefore.empty() && iswspace(operandBefore.back())) operandBefore.remove_suffix(1);

                if (!operandBefore.empty()) {
                    bool validBefore = true;
                    for (wchar_t ch : operandBefore) {
                        if (!iswalnum(ch) && ch != L'_' && ch != L'.' && ch != L':' && ch != L'-' && ch != L'>' && ch != L'"' && ch != L'\'' && ch != L'[' && ch != L']') {
                            validBefore = false;
                            break;
                        }
                    }

                    if (validBefore) {
                        // Check operand after operator (allowing legal leading whitespace)
                        size_t afterOp = pos + op.size();
                        while (afterOp < lineEnd && iswspace(str[afterOp])) ++afterOp;
                        while (afterOp < lineEnd && (str[afterOp] == L'(' || iswspace(str[afterOp]))) ++afterOp;
                        if (afterOp < lineEnd) {
                            wchar_t nextCh = str[afterOp];
                            if (iswalnum(nextCh) || nextCh == L'_' || nextCh == L'"' || nextCh == L'\'' ||
                                nextCh == L'{' || nextCh == L'&' || nextCh == L'*' || nextCh == L'-' || nextCh == L'!') {
                                size_t nextTokEnd = afterOp;
                                while (nextTokEnd < lineEnd && iswalnum(str[nextTokEnd])) ++nextTokEnd;
                                std::wstring_view nextWord = str.substr(afterOp, nextTokEnd - afterOp);
                                if (nextWord != L"with" && nextWord != L"in" && nextWord != L"and" && nextWord != L"or" && nextWord != L"the" && nextWord != L"to") {
                                    return true;
                                }
                            }
                        }
                    }
                }
            }
            pos += op.size();
        }
        return false;
    };

    auto hasIncDecOperatorAtStatementPosition = [&](std::wstring_view str, std::wstring_view op) {
        size_t pos = 0;
        while ((pos = str.find(op, pos)) != std::wstring_view::npos) {
            size_t stmtStart = getStatementStart(str, pos);
            size_t lineEnd = getEffectiveLineEnd(str, pos);
            // Code line must end with ';', '{', '}', or ')'
            if (lineEnd > pos && (str[lineEnd - 1] == L';' || str[lineEnd - 1] == L'{' || str[lineEnd - 1] == L'}' || str[lineEnd - 1] == L')')) {
                // Postfix: operand before op, with optional legal whitespace: e.g. "i++;", "i ++;", "ptr->count++;", "ptr -> count ++;"
                if (pos > stmtStart) {
                    size_t opEnd = pos;
                    while (opEnd > stmtStart && iswspace(str[opEnd - 1])) --opEnd;
                    std::wstring_view operandBefore = str.substr(stmtStart, opEnd - stmtStart);
                    if (!operandBefore.empty()) {
                        bool allIdChars = true;
                        for (wchar_t ch : operandBefore) {
                            if (!iswalnum(ch) && ch != L'_' && ch != L'.' && ch != L'-' && ch != L'>' && ch != L':' && !iswspace(ch)) {
                                allIdChars = false;
                                break;
                            }
                        }
                        if (allIdChars) {
                            bool hasSpaceSeparatedWords = false;
                            for (size_t i = 1; i + 1 < operandBefore.size(); ++i) {
                                if (iswspace(operandBefore[i]) && iswalnum(operandBefore[i - 1])) {
                                    size_t nextNonSpace = i;
                                    while (nextNonSpace < operandBefore.size() && iswspace(operandBefore[nextNonSpace])) ++nextNonSpace;
                                    if (nextNonSpace < operandBefore.size() && iswalnum(operandBefore[nextNonSpace])) {
                                        hasSpaceSeparatedWords = true;
                                        break;
                                    }
                                }
                            }
                            if (!hasSpaceSeparatedWords) {
                                return true;
                            }
                        }
                    }
                }
                // Prefix: "++i;", "++ i;", "--count;", "-- count;"
                if (stmtStart == pos) {
                    size_t afterOp = pos + op.size();
                    while (afterOp < lineEnd && iswspace(str[afterOp])) ++afterOp;
                    if (afterOp < lineEnd && (iswalnum(str[afterOp]) || str[afterOp] == L'_')) {
                        return true;
                    }
                }
            }
            pos += op.size();
        }
        return false;
    };

    if (hasComparisonOperatorAtStatementPosition(text, L"==")) ++codeOperators;
    if (hasComparisonOperatorAtStatementPosition(text, L"!=")) ++codeOperators;
    if (hasIncDecOperatorAtStatementPosition(text, L"++")) ++codeOperators;
    if (hasIncDecOperatorAtStatementPosition(text, L"--")) ++codeOperators;
    bool hasReturnStatement = false;
    {
        size_t retPos = 0;
        while ((retPos = text.find(L"return ", retPos)) != std::wstring_view::npos) {
            size_t before = retPos;
            while (before > 0 && (text[before - 1] == L' ' || text[before - 1] == L'\t')) {
                --before;
            }
            if (before == 0 || text[before - 1] == L'\n' || text[before - 1] == L';' || text[before - 1] == L'{') {
                size_t lineEnd = text.find(L'\n', retPos);
                if (lineEnd == std::wstring_view::npos) lineEnd = text.size();
                std::wstring_view line = text.substr(retPos, lineEnd - retPos);
                if (line.find(L';') != std::wstring_view::npos) {
                    hasReturnStatement = true;
                    break;
                }
            }
            retPos += 7;
        }
    }
    if (hasReturnStatement) codeFeatureScore += 2;

    if (semicolonsAtLineEnd >= 2) codeFeatureScore += 2;
    if (curlyBraces >= 2) codeFeatureScore += 2;
    if (codeOperators >= 1) codeFeatureScore += 2;

    if (isEditorContext && (codeFeatureScore >= 2 || (lineCount >= 3 && semicolonsAtLineEnd >= 1))) {
        return true;
    }

    return codeFeatureScore >= 3;
}

bool SmartPasteManager::IsLiteralOrShortCircuit(std::wstring_view text, const AppConfig& config) {
    if (text.empty()) {
        return true;
    }

    size_t start = 0;
    while (start < text.size() && iswspace(text[start])) {
        ++start;
    }
    std::wstring_view trimmed = text.substr(start);
    while (!trimmed.empty() && iswspace(trimmed.back())) {
        trimmed.remove_suffix(1);
    }
    if (trimmed.empty()) {
        return true;
    }

    // 1. Language match no-op:
    if (config.operation == TransformOperation::Translate &&
        !config.sourceLanguage.empty() && config.sourceLanguage != L"auto" &&
        config.sourceLanguage == config.targetLanguage) {
        return true;
    }

    // 2. Pure URL: starts with http://, https://, ftp://, www. and contains no whitespace
    if (trimmed.starts_with(L"http://") || trimmed.starts_with(L"https://") ||
        trimmed.starts_with(L"ftp://") || trimmed.starts_with(L"www.")) {
        bool hasWhitespace = false;
        for (wchar_t c : trimmed) {
            if (iswspace(c)) {
                hasWhitespace = true;
                break;
            }
        }
        if (!hasWhitespace) {
            return true;
        }
    }

    // 3. Filepaths
    if (trimmed.size() >= 3 && iswalpha(trimmed[0]) && trimmed[1] == L':' && (trimmed[2] == L'\\' || trimmed[2] == L'/')) {
        if (trimmed.find(L'\n') == std::wstring_view::npos) {
            return true;
        }
    }
    if (trimmed.starts_with(L"\\\\") && trimmed.find(L'\n') == std::wstring_view::npos) {
        return true;
    }
    if (trimmed.starts_with(L"/") && trimmed.find(L'\n') == std::wstring_view::npos &&
        (trimmed.starts_with(L"/usr/") || trimmed.starts_with(L"/etc/") || trimmed.starts_with(L"/home/") ||
         trimmed.starts_with(L"/var/") || trimmed.starts_with(L"/bin/") || trimmed.starts_with(L"/opt/"))) {
        return true;
    }

    // 4. Email address
    if (trimmed.find(L'@') != std::wstring_view::npos && trimmed.find(L'.') != std::wstring_view::npos) {
        bool hasWhitespace = false;
        for (wchar_t c : trimmed) {
            if (iswspace(c)) {
                hasWhitespace = true;
                break;
            }
        }
        if (!hasWhitespace && trimmed.size() <= 100) {
            size_t at = trimmed.find(L'@');
            size_t dot = trimmed.find_last_of(L'.');
            if (at > 0 && dot > at + 1 && dot < trimmed.size() - 1) {
                return true;
            }
        }
    }

    // 5. Pure numeric / formula / symbols: contains NO alphabetic letters
    bool hasAlpha = false;
    for (wchar_t c : trimmed) {
        if (iswalpha(c)) {
            hasAlpha = true;
            break;
        }
    }
    if (!hasAlpha) {
        return true;
    }

    return false;
}

SmartPasteAnalysis SmartPasteManager::AnalyzeContent(std::wstring_view text,
                                                     const AppConfig& config,
                                                     HWND targetHwnd,
                                                     DWORD processId) {
    SmartPasteAnalysis analysis;

    // 1. Secrets / Credentials check (Highest Priority: Prevent cloud leakage)
    if (IsSensitiveOrSecret(text)) {
        analysis.decision = SmartPasteDecision::PasteVerbatim;
        analysis.kind = ContentKind::SecretOrCredential;
        analysis.isSensitive = true;
        analysis.reason = L"Sensitive secret or credential detected; pasting verbatim to prevent cloud leakage";
        return analysis;
    }

    // 2. Source Code / Structured Syntax check (Prevent code corruption)
    const bool isEditor = IsEditorProcess(processId, targetHwnd);
    if (IsSourceCode(text, isEditor)) {
        analysis.decision = SmartPasteDecision::PasteVerbatim;
        analysis.kind = ContentKind::SourceCode;
        analysis.isCode = true;
        analysis.reason = L"Source code or structured syntax detected; pasting verbatim to prevent corruption";
        return analysis;
    }

    // 3. Literal / Short-Circuit check (Save latency and API calls)
    if (IsLiteralOrShortCircuit(text, config)) {
        analysis.decision = SmartPasteDecision::PasteVerbatim;
        analysis.kind = (config.operation == TransformOperation::Translate &&
                         !config.sourceLanguage.empty() && config.sourceLanguage != L"auto" &&
                         config.sourceLanguage == config.targetLanguage)
                            ? ContentKind::LanguageMatch
                            : ContentKind::LiteralOrUrl;
        analysis.isShortCircuit = true;
        analysis.reason = L"Literal, URL, numerical, or identical language; short-circuiting verbatim paste";
        return analysis;
    }

    // 4. Natural language text eligible for transformation
    analysis.decision = SmartPasteDecision::Transform;
    analysis.kind = ContentKind::NaturalLanguage;
    analysis.reason = L"Natural language text eligible for transformation";
    return analysis;
}

SmartPasteAnalysis SmartPasteManager::AnalyzeContent(std::wstring_view text) const {
    const AppConfig cfg = configStore_ ? configStore_->Get() : AppConfig{};
    return AnalyzeContent(text, cfg);
}

bool SmartPasteManager::Trigger(HWND targetHwndOverride) {
    if (!configStore_) {
        return false;
    }

    const AppConfig cfg = configStore_->Get();
    if (!cfg.enabled) {
        if (status_) {
            status_->Show(L"AI Input: disabled");
        }
        return false;
    }

    if (!cfg.smartPasteEnabled) {
        if (status_) {
            status_->Show(L"Smart Paste: disabled in settings");
        }
        return false;
    }

    if (busy_.load()) {
        if (status_) {
            status_->Show(L"Smart Paste: busy");
        }
        return false;
    }

    // Step 1: Inspect clipboard formats
    const auto inspect = ClipboardHelper::InspectClipboard();
    if (inspect.hasUnsupportedNonText) {
        if (status_) {
            status_->Show(L"Smart Paste: clipboard contains non-text data");
        }
        Logger::Instance().Info(cat::Input, L"Smart Paste aborted: clipboard contains non-text data");
        return false;
    }

    if (!inspect.hasText) {
        if (status_) {
            status_->Show(L"Smart Paste: clipboard is empty");
        }
        return false;
    }

    // Step 2: Read clipboard Unicode text and original sequence number
    std::wstring clipboardText;
    DWORD origSequence = 0;
    if (!ClipboardHelper::ReadUnicodeText(clipboardText, origSequence, nullptr) || clipboardText.empty()) {
        if (status_) {
            status_->Show(L"Smart Paste: clipboard is empty");
        }
        return false;
    }

    // Step 3: Capture Target Context before starting async request
    TargetContext target;
    target.targetHwnd = targetHwndOverride ? targetHwndOverride : GetForegroundWindow();
    if (!target.targetHwnd) {
        target.targetHwnd = GetDesktopWindow();
    }
    target.threadId = GetWindowThreadProcessId(target.targetHwnd, &target.processId);

    GUITHREADINFO gti = { sizeof(gti) };
    if (target.threadId && GetGUIThreadInfo(target.threadId, &gti) && gti.hwndFocus) {
        target.focusHwnd = gti.hwndFocus;
    } else {
        target.focusHwnd = target.targetHwnd;
    }

    // Step 4: Pre-paste coordination hook (e.g. flush active composition in InputEngine)
    if (prePasteHook_) {
        prePasteHook_();
    }

    // Step 5: Content Classification and Decision Engine (Opportunity 1)
    const SmartPasteAnalysis analysis = AnalyzeContent(clipboardText, cfg, target.targetHwnd, target.processId);

    if (analysis.decision == SmartPasteDecision::PasteVerbatim) {
        Logger::Instance().Info(cat::Input,
                               L"Smart Paste short-circuited: {} ({} chars, target={:#x}, pid={})",
                               analysis.reason, clipboardText.size(),
                               reinterpret_cast<uintptr_t>(target.targetHwnd), target.processId);

        if (status_) {
            if (analysis.isSensitive) {
                status_->Show(L"Smart Paste: sensitive data (pasted verbatim)");
            } else if (analysis.isCode) {
                status_->Show(L"Smart Paste: code detected (pasted verbatim)");
            } else {
                status_->Show(L"Smart Paste: verbatim (no translation needed)");
            }
        }

        const bool pasted = ClipboardHelper::ExecuteSafePasteAndRestore(
            target.targetHwnd, clipboardText, clipboardText, origSequence);

        if (pasted) {
            Logger::Instance().Info(cat::Input, L"Smart Paste verbatim paste completed successfully");
        } else {
            const DWORD curSeq = GetClipboardSequenceNumber();
            if (curSeq != origSequence) {
                if (status_) {
                    status_->Show(L"Smart Paste cancelled: clipboard was modified externally");
                }
            }
            Logger::Instance().Warn(cat::Input, L"Smart Paste aborted during paste sequence check");
        }
        return true;
    }

    // Step 6: Build standard TranslationRequest reusing current live configuration
    TranslationRequest req;
    req.text = clipboardText;
    req.operation = cfg.operation;
    req.sourceLanguage = cfg.sourceLanguage;
    req.targetLanguage = (cfg.operation == TransformOperation::CorrectAndRewrite)
                             ? cfg.sourceLanguage
                             : cfg.targetLanguage;
    req.profile = cfg.profile;
    req.customInstructions = cfg.customProfileInstructions;
    req.mode = cfg.mode;
    req.useCache = cfg.cacheEnabled;
    req.requestId = ++reqCounter_;

    const int64_t currentId = req.requestId;
    currentRequestId_.store(currentId);
    busy_.store(true);

    if (status_) {
        status_->Show(L"Smart Paste: translating...");
    }

    Logger::Instance().Info(cat::Input,
                           L"Smart Paste request #{} initiated ({} chars, target={:#x}, pid={})",
                           currentId, clipboardText.size(),
                           reinterpret_cast<uintptr_t>(target.targetHwnd), target.processId);

    if (Logger::Instance().LogRawText()) {
        Logger::Instance().Debug(cat::Input, L"Smart Paste request #{} text: '{}'", currentId, clipboardText);
    }

    // Step 7: Submit request through existing TranslationPipeline
    auto alive = alive_;
    pipeline_->Submit(std::move(req), [this, alive, currentId, target, origSequence,
                                       originalText = std::move(clipboardText)](TranslationResult result) {
        if (!alive || !alive->load()) {
            return;
        }
        if (dispatcher_) {
            dispatcher_->Post([this, alive, currentId, target, origSequence,
                               originalText = std::move(originalText),
                               result = std::move(result)]() mutable {
                if (!alive || !alive->load()) {
                    return;
                }
                OnTranslationCompleted(currentId, target, origSequence,
                                       std::move(originalText), std::move(result));
            });
        } else {
            OnTranslationCompleted(currentId, target, origSequence,
                                   std::move(originalText), std::move(result));
        }
    });

    return true;
}

void SmartPasteManager::OnTranslationCompleted(int64_t requestId,
                                               TargetContext target,
                                               DWORD origSequence,
                                               std::wstring originalText,
                                               TranslationResult result) {
    if (requestId != currentRequestId_.load()) {
        Logger::Instance().Debug(cat::Input, L"Smart Paste result #{} discarded (superseded by #{})",
                                requestId, currentRequestId_.load());
        return;
    }

    busy_.store(false);

    // Error Safety: If translation failed, do NOT touch target or clipboard
    if (!result.ok) {
        Logger::Instance().Warn(cat::Input, L"Smart Paste request #{} failed: {}", requestId,
                                result.errorDetail.empty() ? result.error : result.errorDetail);
        if (status_) {
            status_->Show(std::format(L"Smart Paste failed: {}", result.error));
        }
        return;
    }

    // Target Validation: Validate target window is still active foreground window
    if (target.targetHwnd != GetDesktopWindow()) {
        const HWND currentFg = GetForegroundWindow();
        if (currentFg != target.targetHwnd) {
            Logger::Instance().Warn(cat::Input,
                                    L"Smart Paste #{} cancelled: target window changed (orig={:#x}, current={:#x})",
                                    requestId, reinterpret_cast<uintptr_t>(target.targetHwnd),
                                    reinterpret_cast<uintptr_t>(currentFg));
            if (status_) {
                status_->Show(L"Smart Paste cancelled: target window changed");
            }
            return;
        }

        DWORD currentPid = 0;
        if (currentFg) {
            GetWindowThreadProcessId(currentFg, &currentPid);
            if (currentPid && currentPid != target.processId) {
                Logger::Instance().Warn(cat::Input,
                                        L"Smart Paste #{} cancelled: target process changed (orig={}, current={})",
                                        requestId, target.processId, currentPid);
                if (status_) {
                    status_->Show(L"Smart Paste cancelled: target process changed");
                }
                return;
            }
        }
    }

    // Clipboard Sequence Validation & Safe Paste
    if (status_) {
        status_->Show(L"Smart Paste: pasting...");
    }

    const bool pasted = ClipboardHelper::ExecuteSafePasteAndRestore(
        target.targetHwnd, result.text, originalText, origSequence);

    if (pasted) {
        if (status_) {
            status_->Show(L"Smart Paste: done");
        }
        Logger::Instance().Info(cat::Input, L"Smart Paste #{} completed successfully", requestId);
    } else {
        if (status_) {
            status_->Show(L"Smart Paste cancelled: clipboard was modified externally");
        }
        Logger::Instance().Warn(cat::Input, L"Smart Paste #{} aborted during paste sequence check", requestId);
    }
}

} // namespace aiie
