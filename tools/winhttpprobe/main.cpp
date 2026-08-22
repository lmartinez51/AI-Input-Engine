// winhttpprobe: diagnostic tool that replicates GeminiProvider's exact
// WinHTTP call sequence with per-phase timing, and compares the two access
// types (DEFAULT_PROXY vs NO_PROXY). Never prints the API key.
//
// Usage: aiie_winhttpprobe [--noproxy]

#include <windows.h>
#include <winhttp.h>
#include <wininet.h>

#ifndef WINHTTP_OPTION_ENABLE_HTTP2_PROTOCOL
#define WINHTTP_OPTION_ENABLE_HTTP2_PROTOCOL 158
#endif

#ifndef WINHTTP_DISABLE_EXPECT_100_CONTINUE
#define WINHTTP_DISABLE_EXPECT_100_CONTINUE 0x00000004
#endif

#include <cstdio>
#include <string>
#include <vector>

namespace {

std::wstring EnvKey() {
    wchar_t buf[4096] = {};
    const DWORD n = GetEnvironmentVariableW(L"GEMINI_API_KEY", buf, 4096);
    return n > 0 && n < 4096 ? std::wstring(buf) : std::wstring();
}

unsigned long long Now() { return GetTickCount64(); }

struct Phase {
    const wchar_t* name;
    unsigned long long start;
    unsigned long long end;
};

bool Run(const std::wstring& key, DWORD accessType, const wchar_t* accessLabel,
         bool ignoreRevocation, bool disableExpect100, bool disableHttp2) {
    const wchar_t* kEndpoint = L"https://generativelanguage.googleapis.com/v1beta";
    const wchar_t* kModel = L"gemini-3.6-flash";

    std::wstring url = std::wstring(kEndpoint) + L"/models/" + kModel + L":generateContent";
    const std::wstring body =
        L"{\"contents\":[{\"parts\":[{\"text\":\"Hola mundo\"}]}]}";

    std::vector<Phase> phases;
    const auto t0 = Now();

    HINTERNET session = WinHttpOpen(L"AIInputEngine/0.1", accessType,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    phases.push_back({L"WinHttpOpen", t0, Now()});
    if (!session) {
        std::wprintf(L"  %ls: WinHttpOpen failed %lu\n", accessLabel, GetLastError());
        return false;
    }

    std::wstring host = kEndpoint + 8; // strip https://
    if (const size_t slash = host.find(L'/'); slash != std::wstring::npos) {
        host = host.substr(0, slash);
    }
    HINTERNET connect = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    phases.push_back({L"WinHttpConnect", phases.back().end, Now()});
    if (!connect) {
        std::wprintf(L"  %ls: WinHttpConnect failed %lu\n", accessLabel, GetLastError());
        WinHttpCloseHandle(session);
        return false;
    }

    const size_t pathAt = url.find(L'/', 8);
    const std::wstring path = pathAt == std::wstring::npos ? std::wstring(L"/") : url.substr(pathAt);
    HINTERNET req = WinHttpOpenRequest(connect, L"POST", path.c_str(), nullptr,
                                       WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    phases.push_back({L"WinHttpOpenRequest", phases.back().end, Now()});
    if (!req) {
        std::wprintf(L"  %ls: WinHttpOpenRequest failed %lu\n", accessLabel, GetLastError());
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    if (ignoreRevocation) {
        DWORD flags = SECURITY_FLAG_IGNORE_REVOCATION;
        WinHttpSetOption(req, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));
    }
    if (disableExpect100) {
        DWORD feature = WINHTTP_DISABLE_EXPECT_100_CONTINUE;
        WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &feature, sizeof(feature));
    }
    if (disableHttp2) {
        DWORD enable = 0;
        WinHttpSetOption(req, WINHTTP_OPTION_ENABLE_HTTP2_PROTOCOL, &enable, sizeof(enable));
    }
    WinHttpSetTimeouts(req, 30000, 30000, 30000, 30000);

    const std::wstring headers = L"Content-Type: application/json\r\nx-goog-api-key: " + key + L"\r\n";
    const std::string utf8Body = [&] {
        const int n = WideCharToMultiByte(CP_UTF8, 0, body.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(n - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, body.c_str(), -1, s.data(), n, nullptr, nullptr);
        return s;
    }();
    const BOOL sent = WinHttpSendRequest(req, headers.c_str(), static_cast<DWORD>(-1),
                                         const_cast<char*>(utf8Body.data()),
                                         static_cast<DWORD>(utf8Body.size()),
                                         static_cast<DWORD>(utf8Body.size()), 0);
    phases.push_back({L"WinHttpSendRequest", phases.back().end, Now()});
    if (!sent) {
        std::wprintf(L"  %ls: WinHttpSendRequest failed %lu\n", accessLabel, GetLastError());
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    const BOOL recv = WinHttpReceiveResponse(req, nullptr);
    phases.push_back({L"WinHttpReceiveResponse", phases.back().end, Now()});
    if (!recv) {
        std::wprintf(L"  %ls: WinHttpReceiveResponse failed %lu\n", accessLabel, GetLastError());
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    phases.push_back({L"status headers", phases.back().end, Now()});

    std::string response;
    DWORD available = 0;
    do {
        if (!WinHttpQueryDataAvailable(req, &available) || available == 0) {
            break;
        }
        std::vector<char> chunk(available);
        DWORD read = 0;
        if (!WinHttpReadData(req, chunk.data(), available, &read) || read == 0) {
            break;
        }
        response.append(chunk.data(), read);
    } while (available > 0);
    phases.push_back({L"read body", phases.back().end, Now()});

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    std::wprintf(L"  %ls: total %llu ms (http %lu, %zu bytes)\n", accessLabel,
                 Now() - t0, status, response.size());
    for (size_t i = 0; i < phases.size(); ++i) {
        std::wprintf(L"      %-24ls %llu ms\n", phases[i].name,
                     static_cast<unsigned long long>(phases[i].end - phases[i].start));
    }
    return status == 200;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    bool noproxy = false;
    bool norevoke = false;
    bool noexpect = false;
    bool noh2 = false;
    for (int i = 1; i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--noproxy") {
            noproxy = true;
        } else if (std::wstring(argv[i]) == L"--norevoke") {
            norevoke = true;
        } else if (std::wstring(argv[i]) == L"--noexpect") {
            noexpect = true;
        } else if (std::wstring(argv[i]) == L"--noh2") {
            noh2 = true;
        }
    }
    const std::wstring key = EnvKey();
    if (key.empty()) {
        std::wprintf(L"GEMINI_API_KEY not set\n");
        return 2;
    }

    std::wprintf(L"== DEFAULT_PROXY (current provider behavior) [revoke=%d expect=%d h2=%d] ==\n",
                 norevoke ? 0 : 1, noexpect ? 0 : 1, noh2 ? 0 : 1);
    const bool a = Run(key, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, L"default-proxy", norevoke,
                       noexpect, noh2);
    if (noproxy) {
        std::wprintf(L"== NO_PROXY [revoke=%d expect=%d h2=%d] ==\n", norevoke ? 0 : 1,
                     noexpect ? 0 : 1, noh2 ? 0 : 1);
        const bool b = Run(key, WINHTTP_ACCESS_TYPE_NO_PROXY, L"no-proxy", norevoke, noexpect,
                           noh2);
        return (a && b) ? 0 : 1;
    }
    return a ? 0 : 1;
}