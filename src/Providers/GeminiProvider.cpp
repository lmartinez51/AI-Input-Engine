#include "Providers/GeminiProvider.h"

#include "Core/Json.h"
#include "Core/Logging.h"
#include "Core/Utf.h"
#include "Translation/TranslationPrompt.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>

namespace aiie {

namespace {

std::wstring LanguageForPrompt(std::wstring_view code) {
    if (code == L"auto") return L"auto-detect (identify it from the text)";
    const std::wstring name(LanguageDisplayName(code));
    return name == code ? std::wstring(code) : name;
}

// Extracts the host portion ("host[:port]") from a full https URL.
std::wstring HostFromUrl(const std::wstring& url) {
    std::wstring host = url;
    if (const size_t scheme = host.find(L"://"); scheme != std::wstring::npos) {
        host = host.substr(scheme + 3);
    }
    if (const size_t slash = host.find(L'/'); slash != std::wstring::npos) {
        host = host.substr(0, slash);
    }
    if (const size_t colon = host.find(L':'); colon != std::wstring::npos) {
        host = host.substr(0, colon);
    }
    return host;
}

// Extracts the request-target path (starting with '/') from a full URL.
// WinHttpOpenRequest must receive only the path; passing a full URL makes
// servers reject the request with 404.
std::wstring PathFromUrl(const std::wstring& url) {
    std::wstring path = url;
    if (const size_t scheme = path.find(L"://"); scheme != std::wstring::npos) {
        path = path.substr(scheme + 3);
    }
    if (const size_t slash = path.find(L'/'); slash != std::wstring::npos) {
        path = path.substr(slash);
    } else {
        path = L"/";
    }
    return path;
}

// Formats the HTTP status into a user-presentable error.
std::wstring StatusErrorMessage(DWORD status) {
    switch (status) {
    case 400: return L"Bad request (400): the provider rejected the request";
    case 401:
    case 403: return L"API authentication failed (401/403): check the API key";
    case 404: return L"Not found (404): check the endpoint and model name";
    case 429: return L"Rate limited (429): too many requests";
    default:
        if (status >= 500) return std::format(L"Provider server error ({})", status);
        return std::format(L"Unexpected HTTP status {}", status);
    }
}

// Sleeps for `ms` milliseconds in small increments, returning false immediately if
// cancelled or shutting down.
bool InterruptibleSleep(int ms, const std::shared_ptr<std::atomic<bool>>& cancelled,
                        const std::atomic<bool>& shutdown) {
    constexpr int kStepMs = 10;
    int elapsed = 0;
    while (elapsed < ms) {
        if ((cancelled && cancelled->load()) || shutdown.load()) {
            return false;
        }
        const int sleepChunk = std::min(kStepMs, ms - elapsed);
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepChunk));
        elapsed += sleepChunk;
    }
    return !((cancelled && cancelled->load()) || shutdown.load());
}

} // namespace

GeminiProvider::GeminiProvider(GeminiConfig config) : config_(std::move(config)) {
    worker_ = std::thread([this] { WorkerLoop(); });
}

GeminiProvider::~GeminiProvider() {
    shutdown_.store(true);
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    ResetConnection();
}

bool GeminiProvider::EnsureConnection(const std::wstring& host, int timeoutMs, std::wstring& outError) {
    outError.clear();
    if (!session_) {
        session_ = WinHttpOpen(L"AIInputEngine/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_) {
            outError = std::format(L"WinHttpOpen error {}", GetLastError());
            return false;
        }
        WinHttpSetTimeouts(session_, timeoutMs / 4, timeoutMs / 2, timeoutMs, timeoutMs);
    }
    if (!connect_ || connectedHost_ != host) {
        if (connect_) {
            WinHttpCloseHandle(connect_);
            connect_ = nullptr;
        }
        connect_ = WinHttpConnect(session_, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!connect_) {
            outError = std::format(L"WinHttpConnect error {}", GetLastError());
            return false;
        }
        connectedHost_ = host;
    }
    return true;
}

void GeminiProvider::ResetConnection() {
    if (connect_) {
        WinHttpCloseHandle(connect_);
        connect_ = nullptr;
    }
    if (session_) {
        WinHttpCloseHandle(session_);
        session_ = nullptr;
    }
    connectedHost_.clear();
}

ProviderStatus GeminiProvider::Status() const {
    std::lock_guard lock(statusMutex_);
    return {state_.load(), statusMessage_};
}

void GeminiProvider::CancelAll() {
    std::lock_guard lock(mutex_);
    for (auto& p : queue_) {
        p.cancelled->store(true);
    }
    queue_.clear();
    if (activeCancelled_) {
        activeCancelled_->store(true);
    }
}

void GeminiProvider::Translate(TranslationRequest request, TranslationCallback callback) {
    Pending p;
    p.cancelled = request.cancellationToken ? request.cancellationToken : std::make_shared<std::atomic<bool>>(false);
    p.request = std::move(request);
    p.callback = std::move(callback);
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(p));
    }
    cv_.notify_all();
}

void GeminiProvider::WorkerLoop() {
    for (;;) {
        Pending p;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return shutdown_.load() || !queue_.empty(); });
            if (shutdown_.load()) {
                return;
            }
            p = std::move(queue_.front());
            queue_.pop_front();
            activeCancelled_ = p.cancelled;
        }
        if (p.cancelled->load()) {
            TranslationResult r;
            r.requestId = p.request.requestId;
            r.ok = false;
            r.error = L"cancelled";
            if (p.callback) {
                p.callback(std::move(r));
            }
            {
                std::lock_guard lock(mutex_);
                activeCancelled_.reset();
            }
            continue;
        }
        TranslationResult result;
        result.requestId = p.request.requestId;
        const bool ok = Execute(p, result);
        result.ok = ok;
        if (ok) {
            ++completed_;
        } else {
            ++failed_;
        }
        {
            std::lock_guard lock(statusMutex_);
            state_.store(ok ? ProviderStatus::State::Ready : ProviderStatus::State::Error);
            statusMessage_ = ok ? L"ready" : result.errorDetail;
        }
        if (p.callback) {
            p.callback(std::move(result));
        }
        {
            std::lock_guard lock(mutex_);
            activeCancelled_.reset();
        }
    }
}

bool GeminiProvider::Execute(Pending& p, TranslationResult& out) {
    const TranslationRequest& request = p.request;
    const std::wstring key = config_.apiKey;
    if (key.empty()) {
        out.error = L"Gemini API key is not configured";
        out.errorDetail = L"Set the API key in Settings or the GEMINI_API_KEY environment variable";
        Logger::Instance().Error(cat::Gemini, L"request #{} failed: API key not configured", request.requestId);
        return false;
    }

const std::wstring url = std::format(L"{}/models/{}:generateContent",
                                         config_.endpoint, config_.model);
    const std::wstring body = BuildRequestBody(request, config_.model);

    DWORD lastStatus = 0;
    for (int attempt = 0; attempt <= config_.maxRetries; ++attempt) {
        if (p.cancelled->load()) {
            out.error = L"cancelled";
            out.errorDetail = L"request cancelled";
            return false;
        }
        if (attempt > 0) {
            const int sleepMs = 500 * (1 << (attempt - 1));
            if (!InterruptibleSleep(sleepMs, p.cancelled, shutdown_)) {
                out.error = L"cancelled";
                out.errorDetail = L"request cancelled";
                return false;
            }
        }

        const std::wstring host = HostFromUrl(config_.endpoint);
        std::wstring connErr;
        if (!EnsureConnection(host, config_.timeoutMs, connErr)) {
            out.error = L"WinHTTP connect failed";
            out.errorDetail = connErr;
            continue; // retryable
        }

        // Authentication via the x-goog-api-key header (supported by the
        // generativelanguage API). The key is never placed in the URL or
        // logged; the header itself is never logged.
        const std::wstring headers =
            L"Content-Type: application/json\r\nx-goog-api-key: " + key + L"\r\n";

        HINTERNET requestHandle = WinHttpOpenRequest(static_cast<HINTERNET>(connect_), L"POST",
                                                     PathFromUrl(url).c_str(),
                                                     nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!requestHandle) {
            ResetConnection();
            out.error = L"WinHTTP request failed";
            out.errorDetail = std::format(L"WinHttpOpenRequest error {}", GetLastError());
            continue;
        }

        WinHttpSetTimeouts(requestHandle, config_.timeoutMs, config_.timeoutMs,
                           config_.timeoutMs, config_.timeoutMs);

        const std::string utf8Body = utf::ToUtf8(body);
        BOOL sent = WinHttpSendRequest(requestHandle, headers.c_str(), static_cast<DWORD>(-1),
                                       const_cast<char*>(utf8Body.data()),
                                       static_cast<DWORD>(utf8Body.size()),
                                       static_cast<DWORD>(utf8Body.size()), 0);
        if (!sent) {
            ResetConnection();
            out.error = L"Network error";
            out.errorDetail = std::format(L"WinHttpSendRequest error {} ({})", GetLastError(),
                                          p.cancelled->load() ? L"cancelled" : L"io");
            WinHttpCloseHandle(requestHandle);
            if (p.cancelled->load()) {
                out.error = L"cancelled";
            }
            continue;
        }

        if (!WinHttpReceiveResponse(requestHandle, nullptr)) {
            ResetConnection();
            out.error = L"Network error";
            out.errorDetail = std::format(L"WinHttpReceiveResponse error {}", GetLastError());
            WinHttpCloseHandle(requestHandle);
            continue;
        }

        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        WinHttpQueryHeaders(requestHandle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);

        std::string response;
        std::array<char, 8192> chunkBuf;
        DWORD available = 0;
        while (WinHttpQueryDataAvailable(requestHandle, &available) && available > 0) {
            const DWORD toRead = std::min<DWORD>(available, static_cast<DWORD>(chunkBuf.size()));
            DWORD read = 0;
            if (!WinHttpReadData(requestHandle, chunkBuf.data(), toRead, &read) || read == 0) {
                break;
            }
            response.append(chunkBuf.data(), read);
        }

        WinHttpCloseHandle(requestHandle);

        lastStatus = status;
        if (status == 200) {
            std::wstring text, detected, error;
            if (ParseResponseBody(utf::FromUtf8(response), text, detected, error)) {
                out.text = std::move(text);
                out.detectedSource = std::move(detected);
                Logger::Instance().Debug(cat::Gemini, L"request #{} ok ({} chars)", request.requestId,
                                         out.text.size());
                return true;
            }
            out.error = L"Invalid provider response";
            out.errorDetail = std::move(error);
            Logger::Instance().Error(cat::Gemini, L"request #{} parse failed: {}", request.requestId,
                                     out.errorDetail);
            return false; // malformed responses are not retried
        }

        // Structured error body.
        std::wstring apiMessage;
        try {
            const json::Value root = json::Parse(utf::FromUtf8(response));
            if (const json::Value* err = root.find(L"error")) {
                apiMessage = err->get_string(L"message");
            }
        } catch (const json::JsonError&) {
            // fall through with generic message
        }
        out.error = StatusErrorMessage(status);
        out.errorDetail = apiMessage.empty()
                              ? std::format(L"HTTP {}", status)
                              : std::format(L"HTTP {}: {}", status, apiMessage);

        const bool retryable = status == 429 || status == 500 || status == 502 || status == 503 || status == 504;
        if (!retryable) {
            break;
        }
        Logger::Instance().Warn(cat::Gemini, L"request #{} attempt {} failed ({}), retrying", request.requestId,
                                attempt + 1, status);
    }
    Logger::Instance().Error(cat::Gemini, L"request #{} failed after retries: {}", request.requestId,
                             out.errorDetail);
    return false;
}

bool GeminiProvider::TestConnection(const std::wstring& endpoint, const std::wstring& model,
                                    const std::wstring& apiKey, int timeoutMs,
                                    std::wstring& outError) {
    if (apiKey.empty()) {
        outError = L"no API key configured";
        return false;
    }

    TranslationRequest probe;
    probe.text = L"hola mundo";
    probe.sourceLanguage = L"es";
    probe.targetLanguage = L"en";
    const std::wstring url = std::format(L"{}/models/{}:generateContent", endpoint, model);
    const std::wstring body = BuildRequestBody(probe, model);
    const std::wstring headers =
        L"Content-Type: application/json\r\nx-goog-api-key: " + apiKey + L"\r\n";

    HINTERNET session = WinHttpOpen(L"AIInputEngine/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        outError = std::format(L"WinHttpOpen error {}", GetLastError());
        return false;
    }
    std::wstring host = endpoint;
    if (const size_t scheme = host.find(L"://"); scheme != std::wstring::npos) {
        host = host.substr(scheme + 3);
    }
    if (const size_t slash = host.find(L'/'); slash != std::wstring::npos) {
        host = host.substr(0, slash);
    }
    if (const size_t colon = host.find(L':'); colon != std::wstring::npos) {
        host = host.substr(0, colon);
    }
    HINTERNET connect = WinHttpConnect(session, HostFromUrl(endpoint).c_str(),
                                       INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) {
        outError = std::format(L"WinHttpConnect error {}", GetLastError());
        WinHttpCloseHandle(session);
        return false;
    }
    HINTERNET requestHandle = WinHttpOpenRequest(connect, L"POST", PathFromUrl(url).c_str(), nullptr,
                                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                 WINHTTP_FLAG_SECURE);
    if (!requestHandle) {
        outError = std::format(L"WinHttpOpenRequest error {}", GetLastError());
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }
    WinHttpSetTimeouts(requestHandle, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    const std::string utf8Body = utf::ToUtf8(body);
    if (!WinHttpSendRequest(requestHandle, headers.c_str(), static_cast<DWORD>(-1),
                            const_cast<char*>(utf8Body.data()),
                            static_cast<DWORD>(utf8Body.size()),
                            static_cast<DWORD>(utf8Body.size()), 0)) {
        outError = std::format(L"WinHttpSendRequest error {}", GetLastError());
        WinHttpCloseHandle(requestHandle);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }
    if (!WinHttpReceiveResponse(requestHandle, nullptr)) {
        outError = std::format(L"WinHttpReceiveResponse error {}", GetLastError());
        WinHttpCloseHandle(requestHandle);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(requestHandle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);

    std::string response;
    std::array<char, 8192> chunkBuf;
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(requestHandle, &available) && available > 0) {
        const DWORD toRead = std::min<DWORD>(available, static_cast<DWORD>(chunkBuf.size()));
        DWORD read = 0;
        if (!WinHttpReadData(requestHandle, chunkBuf.data(), toRead, &read) || read == 0) {
            break;
        }
        response.append(chunkBuf.data(), read);
    }

    WinHttpCloseHandle(requestHandle);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    if (status == 200) {
        std::wstring text, detected, error;
        if (ParseResponseBody(utf::FromUtf8(response), text, detected, error)) {
            return true;
        }
        outError = L"response parse failed: " + error;
        return false;
    }

    std::wstring apiMessage;
    try {
        const json::Value root = json::Parse(utf::FromUtf8(response));
        if (const json::Value* err = root.find(L"error")) {
            apiMessage = err->get_string(L"message");
        }
    } catch (const json::JsonError&) {
    }
    outError = apiMessage.empty() ? std::format(L"HTTP {}", status)
                                  : std::format(L"HTTP {}: {}", status, apiMessage);
    return false;
}

bool GeminiProvider::ListModels(const std::wstring& endpoint, const std::wstring& apiKey,
                                int timeoutMs, std::vector<std::wstring>& outModelIds,
                                std::wstring& outError) {
    if (apiKey.empty()) {
        outError = L"no API key configured";
        return false;
    }
    outModelIds.clear();

    const std::wstring url = endpoint + L"/models";
    const std::wstring headers = L"x-goog-api-key: " + apiKey + L"\r\n";

    HINTERNET session = WinHttpOpen(L"AIInputEngine/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        outError = std::format(L"WinHttpOpen error {}", GetLastError());
        return false;
    }
    std::wstring host = endpoint;
    if (const size_t scheme = host.find(L"://"); scheme != std::wstring::npos) {
        host = host.substr(scheme + 3);
    }
    if (const size_t slash = host.find(L'/'); slash != std::wstring::npos) {
        host = host.substr(0, slash);
    }
    if (const size_t colon = host.find(L':'); colon != std::wstring::npos) {
        host = host.substr(0, colon);
    }
    HINTERNET connect = WinHttpConnect(session, HostFromUrl(endpoint).c_str(),
                                       INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) {
        outError = std::format(L"WinHttpConnect error {}", GetLastError());
        WinHttpCloseHandle(session);
        return false;
    }
    HINTERNET requestHandle = WinHttpOpenRequest(connect, L"GET", PathFromUrl(url).c_str(), nullptr,
                                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                 WINHTTP_FLAG_SECURE);
    if (!requestHandle) {
        outError = std::format(L"WinHttpOpenRequest error {}", GetLastError());
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }
    WinHttpSetTimeouts(requestHandle, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    if (!WinHttpSendRequest(requestHandle, headers.c_str(), static_cast<DWORD>(-1),
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        outError = std::format(L"WinHttpSendRequest error {}", GetLastError());
        WinHttpCloseHandle(requestHandle);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }
    if (!WinHttpReceiveResponse(requestHandle, nullptr)) {
        outError = std::format(L"WinHttpReceiveResponse error {}", GetLastError());
        WinHttpCloseHandle(requestHandle);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(requestHandle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);

    std::string response;
    std::array<char, 8192> chunkBuf;
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(requestHandle, &available) && available > 0) {
        const DWORD toRead = std::min<DWORD>(available, static_cast<DWORD>(chunkBuf.size()));
        DWORD read = 0;
        if (!WinHttpReadData(requestHandle, chunkBuf.data(), toRead, &read) || read == 0) {
            break;
        }
        response.append(chunkBuf.data(), read);
    }

    WinHttpCloseHandle(requestHandle);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    if (status != 200) {
        std::wstring apiMessage;
        try {
            const json::Value root = json::Parse(utf::FromUtf8(response));
            if (const json::Value* err = root.find(L"error")) {
                apiMessage = err->get_string(L"message");
            }
        } catch (const json::JsonError&) {
        }
        outError = apiMessage.empty() ? std::format(L"HTTP {}", status)
                                      : std::format(L"HTTP {}: {}", status, apiMessage);
        return false;
    }

    try {
        const json::Value root = json::Parse(utf::FromUtf8(response));
        const json::Value* models = root.find(L"models");
        if (models && models->is_array()) {
            for (const json::Value& m : models->as_array()) {
                const std::wstring id = m.get_string(L"name");
                if (!id.empty()) {
                    outModelIds.push_back(id);
                }
            }
        }
    } catch (const json::JsonError&) {
        outError = L"model list parse failed";
        return false;
    }
    return !outModelIds.empty();
}

std::wstring GeminiProvider::BuildRequestBody(const TranslationRequest& request, std::wstring_view model) {
    (void)model;
    json::Value root = json::Value::MakeObject();

    std::wstring systemPrompt = BuildInstructionsForRequest(request);

    json::Value sys = json::Value::MakeObject();
    json::Value sysParts = json::Value::MakeArray();
    json::Value sysPart = json::Value::MakeObject();
    sysPart.set(L"text", systemPrompt);
    sysParts.push_back(std::move(sysPart));
    sys.set(L"parts", std::move(sysParts));
    root.set(L"systemInstruction", std::move(sys));

    json::Value userPart = json::Value::MakeObject();
    userPart.set(L"text", FormatUserPayload(request));
    json::Value contents = json::Value::MakeArray();
    json::Value content = json::Value::MakeObject();
    content.set(L"role", L"user");
    json::Value parts = json::Value::MakeArray();
    parts.push_back(std::move(userPart));
    content.set(L"parts", std::move(parts));
    contents.push_back(std::move(content));
    root.set(L"contents", std::move(contents));

    json::Value gen = json::Value::MakeObject();
    gen.set(L"temperature", 0.2);
    gen.set(L"maxOutputTokens", 1024);
    root.set(L"generationConfig", std::move(gen));

    return json::Serialize(root, -1);
}

bool GeminiProvider::ParseResponseBody(std::wstring_view body, std::wstring& outText,
                                       std::wstring& outDetectedSource, std::wstring& error) {
    try {
        const json::Value root = json::Parse(body);
        const json::Value* candidates = root.find(L"candidates");
        if (!candidates || candidates->as_array().empty()) {
            // Empty candidates can happen for prompt blocking.
            error = L"no candidates in response";
            return false;
        }
        const json::Value& candidate = candidates->as_array().front();
        const json::Value* content = candidate.find(L"content");
        if (!content) {
            error = L"candidate missing content";
            return false;
        }
        const json::Value* parts = content->find(L"parts");
        if (!parts) {
            error = L"content missing parts";
            return false;
        }
        std::wstring text;
        for (const json::Value& part : parts->as_array()) {
            text.append(part.get_string(L"text"));
        }
        if (text.empty()) {
            error = L"empty translation text";
            return false;
        }
        outText = std::move(text);
        outDetectedSource = root.get_string(L"detectedSourceLanguage", L"");
        return true;
    } catch (const json::JsonError& e) {
        error = utf::FromUtf8(e.what());
        return false;
    }
}

} // namespace aiie