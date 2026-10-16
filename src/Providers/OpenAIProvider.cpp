#include "Providers/OpenAIProvider.h"

#include "Configuration/ModelCapabilities.h"
#include "Core/Json.h"
#include "Core/Logging.h"
#include "Core/Utf.h"
#include "Translation/TranslationPrompt.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <thread>
#include <vector>

namespace aiie {

namespace {

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
    case 400: return L"Bad request (400): the model or request parameters were rejected";
    case 401:
    case 403: return L"API authentication failed (401/403): check the OpenAI API key";
    case 404: return L"Not found (404): check the endpoint URL and model name";
    case 429: return L"Rate limited (429): too many requests or insufficient quota";
    default:
        if (status >= 500) return std::format(L"OpenAI server error ({})", status);
        return std::format(L"Unexpected HTTP status {}", status);
    }
}

std::wstring TrimWhitespaceAndQuotes(std::wstring text) {
    // Strip leading newlines/carriage returns
    while (!text.empty() && (text.front() == L'\r' || text.front() == L'\n')) {
        text.erase(text.begin());
    }
    // Strip trailing newlines/carriage returns
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) {
        text.pop_back();
    }
    // If wrapped in matching quotation marks, strip them:
    if (text.size() >= 2) {
        if ((text.front() == L'"' && text.back() == L'"') ||
            (text.front() == L'\'' && text.back() == L'\'') ||
            (text.front() == L'\x201C' && text.back() == L'\x201D')) {
            text = text.substr(1, text.size() - 2);
            while (!text.empty() && (text.front() == L'\r' || text.front() == L'\n')) {
                text.erase(text.begin());
            }
            while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) {
                text.pop_back();
            }
        }
    }
    return text;
}

// Performs a raw HTTPS POST request via WinHTTP and returns the response body and status code.
bool PerformHttpRequest(HINTERNET persistentConnect, const std::wstring& endpoint, const std::wstring& authBearerKey,
                        const std::string& utf8Body, int timeoutMs,
                        const std::shared_ptr<std::atomic<bool>>& cancelled,
                        DWORD& outStatusCode,
                        std::wstring& outResponseBody, std::wstring& outError) {
    outStatusCode = 0;
    outResponseBody.clear();
    outError.clear();

    if (cancelled && cancelled->load()) {
        outError = L"Request cancelled";
        return false;
    }

    const std::wstring host = HostFromUrl(endpoint);
    const std::wstring path = PathFromUrl(endpoint);

    HINTERNET session = nullptr;
    HINTERNET connect = persistentConnect;

    if (!connect) {
        session = WinHttpOpen(L"AIInputEngine/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) {
            outError = std::format(L"WinHttpOpen failed: {}", GetLastError());
            return false;
        }

        WinHttpSetTimeouts(session, timeoutMs / 4, timeoutMs / 2, timeoutMs, timeoutMs);

        connect = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!connect) {
            outError = std::format(L"WinHttpConnect failed: {}", GetLastError());
            WinHttpCloseHandle(session);
            return false;
        }
    }

    auto cleanup = [&](HINTERNET req) {
        if (req) {
            WinHttpCloseHandle(req);
        }
        if (session) {
            if (connect && connect != persistentConnect) {
                WinHttpCloseHandle(connect);
            }
            WinHttpCloseHandle(session);
        }
    };

    HINTERNET request = WinHttpOpenRequest(connect, L"POST", path.c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          WINHTTP_FLAG_SECURE);
    if (!request) {
        outError = std::format(L"WinHttpOpenRequest failed: {}", GetLastError());
        cleanup(nullptr);
        return false;
    }

    // Headers
    std::wstring headers = L"Content-Type: application/json; charset=utf-8\r\n";
    if (!authBearerKey.empty()) {
        headers += L"Authorization: Bearer " + authBearerKey + L"\r\n";
    }

    const BOOL sent = WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(headers.size()),
                                         const_cast<char*>(utf8Body.data()),
                                         static_cast<DWORD>(utf8Body.size()),
                                         static_cast<DWORD>(utf8Body.size()), 0);
    if (!sent) {
        outError = std::format(L"WinHttpSendRequest failed: {}", GetLastError());
        cleanup(request);
        return false;
    }

    if (!WinHttpReceiveResponse(request, nullptr)) {
        outError = std::format(L"WinHttpReceiveResponse failed: {}", GetLastError());
        cleanup(request);
        return false;
    }

    if (cancelled && cancelled->load()) {
        cleanup(request);
        outError = L"Request cancelled";
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
                        WINHTTP_NO_HEADER_INDEX);
    outStatusCode = statusCode;

    std::string responseBytes;
    DWORD bytesAvailable = 0;
    while (WinHttpQueryDataAvailable(request, &bytesAvailable) && bytesAvailable > 0) {
        if (cancelled && cancelled->load()) {
            cleanup(request);
            outError = L"Request cancelled";
            return false;
        }
        const size_t prev = responseBytes.size();
        responseBytes.resize(prev + bytesAvailable);
        DWORD bytesRead = 0;
        if (!WinHttpReadData(request, responseBytes.data() + prev, bytesAvailable, &bytesRead)) {
            break;
        }
        responseBytes.resize(prev + bytesRead);
        if (bytesRead == 0) {
            break;
        }
    }

    cleanup(request);

    outResponseBody = utf::FromUtf8(responseBytes);
    return true;
}

// Performs a streaming HTTPS POST request via WinHTTP and Server-Sent Events (SSE).
bool PerformHttpStreamingRequest(HINTERNET persistentConnect, const std::wstring& endpoint, const std::wstring& authBearerKey,
                                 const std::string& utf8Body, int timeoutMs,
                                 const std::shared_ptr<std::atomic<bool>>& cancelled,
                                 RequestTelemetry& telemetry,
                                 DWORD& outStatusCode, std::wstring& outResponseBody,
                                 std::wstring& outError) {
    outStatusCode = 0;
    outResponseBody.clear();
    outError.clear();
    telemetry.streamed = true;

    if (cancelled && cancelled->load()) {
        telemetry.cancelled = true;
        outError = L"Request cancelled";
        return false;
    }

    const std::wstring host = HostFromUrl(endpoint);
    const std::wstring path = PathFromUrl(endpoint);

    HINTERNET session = nullptr;
    HINTERNET connect = persistentConnect;

    if (!connect) {
        session = WinHttpOpen(L"AIInputEngine/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) {
            outError = std::format(L"WinHttpOpen failed: {}", GetLastError());
            return false;
        }

        WinHttpSetTimeouts(session, timeoutMs / 4, timeoutMs / 2, timeoutMs, timeoutMs);

        connect = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!connect) {
            outError = std::format(L"WinHttpConnect failed: {}", GetLastError());
            WinHttpCloseHandle(session);
            return false;
        }
    }

    auto cleanup = [&](HINTERNET req) {
        if (req) {
            WinHttpCloseHandle(req);
        }
        if (session) {
            if (connect && connect != persistentConnect) {
                WinHttpCloseHandle(connect);
            }
            WinHttpCloseHandle(session);
        }
    };

    HINTERNET request = WinHttpOpenRequest(connect, L"POST", path.c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          WINHTTP_FLAG_SECURE);
    if (!request) {
        outError = std::format(L"WinHttpOpenRequest failed: {}", GetLastError());
        cleanup(nullptr);
        return false;
    }

    std::wstring headers = L"Content-Type: application/json; charset=utf-8\r\nAccept: text/event-stream\r\n";
    if (!authBearerKey.empty()) {
        headers += L"Authorization: Bearer " + authBearerKey + L"\r\n";
    }

    const auto tReqStart = std::chrono::steady_clock::now();

    const BOOL sent = WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(headers.size()),
                                         const_cast<char*>(utf8Body.data()),
                                         static_cast<DWORD>(utf8Body.size()),
                                         static_cast<DWORD>(utf8Body.size()), 0);
    if (!sent) {
        outError = std::format(L"WinHttpSendRequest failed: {}", GetLastError());
        cleanup(request);
        return false;
    }

    if (!WinHttpReceiveResponse(request, nullptr)) {
        outError = std::format(L"WinHttpReceiveResponse failed: {}", GetLastError());
        cleanup(request);
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
                        WINHTTP_NO_HEADER_INDEX);
    outStatusCode = statusCode;

    if (statusCode != 200) {
        // Read full error body
        std::string responseBytes;
        DWORD bytesAvailable = 0;
        while (WinHttpQueryDataAvailable(request, &bytesAvailable) && bytesAvailable > 0) {
            const size_t prev = responseBytes.size();
            responseBytes.resize(prev + bytesAvailable);
            DWORD bytesRead = 0;
            if (!WinHttpReadData(request, responseBytes.data() + prev, bytesAvailable, &bytesRead)) {
                break;
            }
            responseBytes.resize(prev + bytesRead);
            if (bytesRead == 0) break;
        }
        cleanup(request);
        outResponseBody = utf::FromUtf8(responseBytes);
        return true;
    }

    // Stream reading loop
    std::string sseBuffer;
    std::wstring accumulatedText;
    std::wstring doneText;
    bool completed = false;
    std::vector<SseEvent> events;
    std::vector<char> chunkBuf(8192);

    while (true) {
        if (cancelled && cancelled->load()) {
            telemetry.cancelled = true;
            cleanup(request);
            outError = L"Request cancelled";
            return false;
        }

        DWORD bytesAvailable = 0;
        if (!WinHttpQueryDataAvailable(request, &bytesAvailable)) {
            break;
        }
        if (bytesAvailable == 0) {
            break; // Stream EOF
        }

        DWORD bytesToRead = std::min<DWORD>(bytesAvailable, static_cast<DWORD>(chunkBuf.size()));
        DWORD bytesRead = 0;
        if (!WinHttpReadData(request, chunkBuf.data(), bytesToRead, &bytesRead) || bytesRead == 0) {
            break;
        }

        events.clear();
        OpenAIProvider::ParseSseChunk(std::string_view(chunkBuf.data(), bytesRead), sseBuffer, events);

        for (const auto& ev : events) {
            std::wstring sseErr;
            if (!OpenAIProvider::ProcessSseEvent(ev, accumulatedText, doneText, completed, sseErr)) {
                outError = sseErr;
                cleanup(request);
                return false;
            }

            // If we just received the first delta text, record TTFT
            if (telemetry.firstTokenTimeMs == 0 && !accumulatedText.empty()) {
                const auto tFirst = std::chrono::steady_clock::now();
                telemetry.firstTokenTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(tFirst - tReqStart).count();
            }
        }

        if (completed) {
            break;
        }
    }

    const auto tEnd = std::chrono::steady_clock::now();
    telemetry.completionTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(tEnd - tReqStart).count();
    telemetry.completed = completed;

    cleanup(request);

    if (cancelled && cancelled->load()) {
        telemetry.cancelled = true;
        outError = L"Request cancelled";
        return false;
    }

    // Determine final output string (prefer authoritative doneText, fallback to accumulatedText)
    std::wstring finalRaw = !doneText.empty() ? doneText : accumulatedText;
    outResponseBody = TrimWhitespaceAndQuotes(finalRaw);
    telemetry.outputLength = outResponseBody.size();

    if (outResponseBody.empty() && !completed) {
        outError = L"Stream ended before output completion";
        return false;
    }

    return true;
}

} // namespace

OpenAIProvider::OpenAIProvider(OpenAIConfig config) : config_(std::move(config)) {
    worker_ = std::thread([this] { WorkerLoop(); });
}

OpenAIProvider::~OpenAIProvider() {
    shutdown_.store(true);
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    ResetConnection();
}

bool OpenAIProvider::EnsureConnection(const std::wstring& host, int timeoutMs, std::wstring& outError) {
    outError.clear();
    if (!session_) {
        session_ = WinHttpOpen(L"AIInputEngine/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_) {
            outError = std::format(L"WinHttpOpen failed: {}", GetLastError());
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
            outError = std::format(L"WinHttpConnect failed: {}", GetLastError());
            return false;
        }
        connectedHost_ = host;
    }
    return true;
}

void OpenAIProvider::ResetConnection() {
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

ProviderStatus OpenAIProvider::Status() const {
    std::lock_guard lock(statusMutex_);
    return {state_.load(), statusMessage_};
}

void OpenAIProvider::CancelAll() {
    std::lock_guard lock(mutex_);
    for (auto& p : queue_) {
        p.cancelled->store(true);
    }
    queue_.clear();
}

void OpenAIProvider::Translate(TranslationRequest request, TranslationCallback callback) {
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

std::wstring OpenAIProvider::BuildRequestBody(const TranslationRequest& request,
                                              std::wstring_view model,
                                              bool stream,
                                              std::optional<std::wstring_view> reasoningEffort,
                                              const ModelParameters* params) {
    json::Value root = json::Value::MakeObject();
    root.set(L"model", std::wstring(model));

    const std::wstring instructions = BuildInstructionsForRequest(request);
    root.set(L"instructions", instructions);
    root.set(L"input", FormatUserPayload(request));

    const ModelCapabilities caps = GetModelCapabilities(ProviderId::OpenAI, model);

    if (stream && caps.SupportsStreaming()) {
        root.set(L"stream", true);
    }

    // Merge requested parameters
    ModelParameters merged;
    if (params) {
        merged = *params;
    }
    if (reasoningEffort.has_value() && !reasoningEffort->empty()) {
        merged.reasoningEffort = std::wstring(*reasoningEffort);
    }

    // Sanitize parameters strictly against model capabilities
    const ModelParameters validParams = SanitizeModelParameters(ProviderId::OpenAI, model, merged);

    if (caps.SupportsReasoningEffort() && validParams.reasoningEffort.has_value()) {
        json::Value reasoningObj = json::Value::MakeObject();
        reasoningObj.set(L"effort", *validParams.reasoningEffort);
        root.set(L"reasoning", std::move(reasoningObj));
    }

    if (caps.SupportsTemperature() && validParams.temperature.has_value()) {
        root.set(L"temperature", *validParams.temperature);
    }

    if (caps.SupportsTopP() && validParams.topP.has_value()) {
        root.set(L"top_p", *validParams.topP);
    }

    if (caps.SupportsMaxOutputTokens() && validParams.maxOutputTokens.has_value()) {
        root.set(L"max_output_tokens", static_cast<double>(*validParams.maxOutputTokens));
    }

    return json::Serialize(root);
}

void OpenAIProvider::ParseSseChunk(std::string_view chunk, std::string& buffer, std::vector<SseEvent>& outEvents) {
    buffer.append(chunk.data(), chunk.size());

    while (true) {
        size_t delimPos = std::string::npos;
        size_t delimLen = 0;

        size_t p1 = buffer.find("\n\n");
        size_t p2 = buffer.find("\r\n\r\n");

        if (p1 != std::string::npos && (p2 == std::string::npos || p1 < p2)) {
            delimPos = p1;
            delimLen = 2;
        } else if (p2 != std::string::npos) {
            delimPos = p2;
            delimLen = 4;
        }

        if (delimPos == std::string::npos) {
            break;
        }

        std::string_view block(buffer.data(), delimPos);
        std::wstring currentEvent;
        std::wstring currentData;

        size_t lineStart = 0;
        while (lineStart < block.size()) {
            size_t lineEnd = block.find('\n', lineStart);
            if (lineEnd == std::string::npos) {
                lineEnd = block.size();
            }

            size_t len = lineEnd - lineStart;
            std::string_view line(block.data() + lineStart, len);
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            lineStart = lineEnd + 1;

            if (line.starts_with("event:")) {
                std::string_view val = line.substr(6);
                while (!val.empty() && val.front() == ' ') val.remove_prefix(1);
                currentEvent = utf::FromUtf8(val);
            } else if (line.starts_with("data:")) {
                std::string_view val = line.substr(5);
                while (!val.empty() && val.front() == ' ') val.remove_prefix(1);
                if (!currentData.empty()) {
                    currentData.push_back(L'\n');
                }
                currentData.append(utf::FromUtf8(val));
            }
        }

        if (!currentEvent.empty() || !currentData.empty()) {
            outEvents.push_back({std::move(currentEvent), std::move(currentData)});
        }

        buffer.erase(0, delimPos + delimLen);
    }
}

bool OpenAIProvider::ProcessSseEvent(const SseEvent& event, std::wstring& inOutAccumulatedText,
                                     std::wstring& inOutDoneText, bool& outCompleted, std::wstring& outError) {
    if (event.data == L"[DONE]") {
        outCompleted = true;
        return true;
    }

    json::Value root;
    try {
        root = json::Parse(event.data);
    } catch (const json::JsonError&) {
        if (event.eventType == L"error" || event.eventType == L"response.failed") {
            outError = event.data;
            outCompleted = false;
            return false;
        }
        return true;
    }

    if (event.eventType == L"error" || event.eventType == L"response.failed" || root.find(L"error")) {
        if (const json::Value* errObj = root.find(L"error")) {
            if (errObj->is_object()) {
                outError = errObj->get_string(L"message", L"OpenAI streaming error");
                outCompleted = false;
                return false;
            }
        }
        if (const json::Value* respObj = root.find(L"response")) {
            if (respObj->is_object()) {
                if (const json::Value* errObj = respObj->find(L"error")) {
                    if (errObj->is_object()) {
                        outError = errObj->get_string(L"message", L"OpenAI streaming error");
                        outCompleted = false;
                        return false;
                    }
                }
            }
        }
        outError = root.get_string(L"message", L"OpenAI streaming error");
        outCompleted = false;
        return false;
    }

    if (event.eventType == L"response.output_text.delta") {
        std::wstring delta = root.get_string(L"delta", L"");
        if (!delta.empty()) {
            inOutAccumulatedText.append(delta);
        }
        return true;
    }

    if (event.eventType == L"response.output_text.done") {
        std::wstring text = root.get_string(L"text", L"");
        if (!text.empty()) {
            inOutDoneText = std::move(text);
        }
        return true;
    }

    if (event.eventType == L"response.completed" || event.eventType == L"response.done") {
        outCompleted = true;
        if (const json::Value* respObj = root.find(L"response")) {
            if (respObj->is_object()) {
                std::wstring outText, err;
                if (ParseResponseBody(json::Serialize(*respObj), outText, err) && !outText.empty()) {
                    inOutDoneText = std::move(outText);
                }
            }
        }
        return true;
    }

    return true;
}

bool OpenAIProvider::ParseResponseBody(std::wstring_view body, std::wstring& outText,
                                       std::wstring& error) {
    outText.clear();
    error.clear();

    json::Value root;
    try {
        root = json::Parse(body);
    } catch (const json::JsonError& e) {
        error = std::format(L"Malformed response JSON: {}", utf::FromUtf8(e.what()));
        return false;
    }

    // Check for API error payload:
    if (const json::Value* errObj = root.find(L"error")) {
        if (errObj->is_object()) {
            error = errObj->get_string(L"message", L"OpenAI API returned an error");
            return false;
        }
    }

    // Strategy 1: Direct output_text convenience property (Responses API)
    if (const json::Value* outTextVal = root.find(L"output_text")) {
        if (outTextVal->is_string()) {
            outText = TrimWhitespaceAndQuotes(outTextVal->as_string());
            if (!outText.empty()) {
                return true;
            }
        }
    }

    // Strategy 2: output[] array -> item.type == "message" -> content[] -> text
    if (const json::Value* outputVal = root.find(L"output")) {
        if (outputVal->is_array()) {
            for (const auto& item : outputVal->as_array()) {
                if (item.is_object()) {
                    if (const json::Value* contentVal = item.find(L"content")) {
                        if (contentVal->is_array()) {
                            for (const auto& contentItem : contentVal->as_array()) {
                                if (contentItem.is_object()) {
                                    std::wstring txt = contentItem.get_string(L"text", L"");
                                    if (!txt.empty()) {
                                        outText = TrimWhitespaceAndQuotes(txt);
                                        return true;
                                    }
                                }
                            }
                        }
                    }
                    std::wstring txt = item.get_string(L"text", L"");
                    if (!txt.empty()) {
                        outText = TrimWhitespaceAndQuotes(txt);
                        return true;
                    }
                }
            }
        }
    }

    // Strategy 3: Chat Completions fallback (choices[0].message.content)
    if (const json::Value* choicesVal = root.find(L"choices")) {
        if (choicesVal->is_array() && !choicesVal->as_array().empty()) {
            const auto& firstChoice = choicesVal->as_array()[0];
            if (firstChoice.is_object()) {
                if (const json::Value* msgVal = firstChoice.find(L"message")) {
                    if (msgVal->is_object()) {
                        std::wstring txt = msgVal->get_string(L"content", L"");
                        if (!txt.empty()) {
                            outText = TrimWhitespaceAndQuotes(txt);
                            return true;
                        }
                    }
                }
            }
        }
    }

    error = L"Response did not contain translated text";
    return false;
}

bool OpenAIProvider::TestConnection(const std::wstring& endpoint, const std::wstring& model,
                                    const std::wstring& apiKey, int timeoutMs,
                                    std::wstring& outError) {
    outError.clear();
    if (apiKey.empty()) {
        outError = L"API key is not configured";
        return false;
    }

    TranslationRequest req;
    req.text = L"Hola";
    req.sourceLanguage = L"es";
    req.targetLanguage = L"en";
    req.mode = TransformMode::Translate;

    const std::wstring jsonBody = BuildRequestBody(req, model);
    const std::string utf8Body = utf::ToUtf8(jsonBody);

    DWORD statusCode = 0;
    std::wstring responseBody;
    std::wstring httpError;
    if (!PerformHttpRequest(nullptr, endpoint, apiKey, utf8Body, timeoutMs, nullptr, statusCode, responseBody, httpError)) {
        outError = httpError;
        return false;
    }

    if (statusCode != 200) {
        std::wstring parsedText, parsedError;
        if (ParseResponseBody(responseBody, parsedText, parsedError) && !parsedError.empty()) {
            outError = parsedError;
        } else {
            outError = StatusErrorMessage(statusCode);
        }
        return false;
    }

    std::wstring translatedText, parseError;
    if (!ParseResponseBody(responseBody, translatedText, parseError)) {
        outError = parseError;
        return false;
    }

    return true;
}

void OpenAIProvider::WorkerLoop() {
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
        }
        if (p.cancelled->load()) {
            Logger::Instance().Debug(cat::Translation, L"[TRANSLATION] provider skipping cancelled queued req=#{}", p.request.requestId);
            TranslationResult res;
            res.requestId = p.request.requestId;
            res.ok = false;
            res.error = L"Request cancelled";
            p.callback(std::move(res));
            continue;
        }

        {
            std::lock_guard lock(statusMutex_);
            state_.store(ProviderStatus::State::Busy);
            statusMessage_ = L"Translating...";
        }

        TranslationResult res = ExecuteOne(p.request, p.cancelled);

        {
            std::lock_guard lock(statusMutex_);
            if (res.ok) {
                state_.store(ProviderStatus::State::Ready);
                statusMessage_ = L"Ready";
            } else {
                state_.store(ProviderStatus::State::Error);
                statusMessage_ = res.error;
            }
        }

        p.callback(std::move(res));
    }
}

TranslationResult OpenAIProvider::ExecuteOne(const TranslationRequest& req,
                                             const std::shared_ptr<std::atomic<bool>>& cancelled) {
    TranslationResult res;
    res.requestId = req.requestId;

    if (config_.apiKey.empty()) {
        res.ok = false;
        res.error = L"OpenAI API key is not configured. Open Settings to enter a key.";
        return res;
    }

    std::optional<std::wstring_view> reasoningEffortOpt;
    if (config_.modelParams.reasoningEffort.has_value()) {
        reasoningEffortOpt = *config_.modelParams.reasoningEffort;
    } else if (config_.reasoningEffort.has_value()) {
        reasoningEffortOpt = *config_.reasoningEffort;
    }

    const ModelCapabilities caps = GetModelCapabilities(ProviderId::OpenAI, config_.model);
    const bool useStreaming = config_.stream && caps.SupportsStreaming();

    const std::wstring jsonBody = BuildRequestBody(req, config_.model, useStreaming,
                                                   reasoningEffortOpt, &config_.modelParams);
    const std::string utf8Body = utf::ToUtf8(jsonBody);

    int attempts = 0;
    const int maxAttempts = std::max(1, config_.maxRetries + 1);

    while (attempts < maxAttempts) {
        if (cancelled && cancelled->load()) {
            res.ok = false;
            res.error = L"Request cancelled";
            return res;
        }

        ++attempts;
        DWORD statusCode = 0;
        std::wstring responseBody;
        std::wstring httpError;
        RequestTelemetry telemetry;

        const auto tStart = std::chrono::steady_clock::now();
        bool httpOk = false;

        const std::wstring host = HostFromUrl(config_.endpoint);
        std::wstring connErr;
        if (!EnsureConnection(host, config_.timeoutMs, connErr)) {
            Logger::Instance().Warn(cat::Translation, L"OpenAI connect error (attempt {}/{}): {}",
                                    attempts, maxAttempts, connErr);
            if (attempts < maxAttempts) {
                if (!InterruptibleSleep(500 * attempts, cancelled, shutdown_)) {
                    res.ok = false;
                    res.error = L"Request cancelled";
                    return res;
                }
                continue;
            }
            res.ok = false;
            res.error = L"Network error communicating with OpenAI";
            res.errorDetail = connErr;
            return res;
        }

        if (useStreaming) {
            httpOk = PerformHttpStreamingRequest(static_cast<HINTERNET>(connect_), config_.endpoint, config_.apiKey, utf8Body,
                                                 config_.timeoutMs, cancelled, telemetry,
                                                 statusCode, responseBody, httpError);
        } else {
            httpOk = PerformHttpRequest(static_cast<HINTERNET>(connect_), config_.endpoint, config_.apiKey, utf8Body,
                                        config_.timeoutMs, cancelled, statusCode, responseBody, httpError);
            const auto tEnd = std::chrono::steady_clock::now();
            telemetry.completionTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(tEnd - tStart).count();
            telemetry.streamed = false;
        }

        if (cancelled && cancelled->load()) {
            res.ok = false;
            res.error = L"Request cancelled";
            return res;
        }

        if (!httpOk) {
            ResetConnection();
            if (httpError == L"Request cancelled") {
                res.ok = false;
                res.error = L"Request cancelled";
                return res;
            }
            Logger::Instance().Warn(cat::Translation, L"OpenAI HTTP error (attempt {}/{}): {}",
                                    attempts, maxAttempts, httpError);
            if (attempts < maxAttempts) {
                if (!InterruptibleSleep(500 * attempts, cancelled, shutdown_)) {
                    res.ok = false;
                    res.error = L"Request cancelled";
                    return res;
                }
                continue;
            }
            res.ok = false;
            res.error = L"Network error communicating with OpenAI";
            res.errorDetail = httpError;
            return res;
        }

        if (statusCode == 200) {
            if (config_.stream) {
                if (!responseBody.empty()) {
                    res.ok = true;
                    res.text = std::move(responseBody);
                    if (telemetry.firstTokenTimeMs > 0) {
                        Logger::Instance().Info(cat::Translation,
                            L"OpenAI translation completed in {} ms (TTFT={} ms, len={})",
                            telemetry.completionTimeMs, telemetry.firstTokenTimeMs, res.text.size());
                    } else {
                        Logger::Instance().Info(cat::Translation,
                            L"OpenAI translation completed in {} ms (len={})",
                            telemetry.completionTimeMs, res.text.size());
                    }
                    return res;
                }
            } else {
                std::wstring translated, parseError;
                if (ParseResponseBody(responseBody, translated, parseError)) {
                    res.ok = true;
                    res.text = std::move(translated);
                    Logger::Instance().Info(cat::Translation, L"OpenAI translation completed in {} ms (len={})",
                                            telemetry.completionTimeMs, res.text.size());
                    return res;
                }
            }
            res.ok = false;
            res.error = L"Failed to parse OpenAI response";
            res.errorDetail = httpError.empty() ? L"Empty or incomplete response" : httpError;
            return res;
        }

        // Handle error status codes:
        Logger::Instance().Warn(cat::Translation, L"OpenAI status {} in {} ms (attempt {}/{})",
                                statusCode, telemetry.completionTimeMs, attempts, maxAttempts);

        // Parse detailed message if provided by API:
        std::wstring parsedText, parsedError;
        std::wstring apiMsg;
        if (ParseResponseBody(responseBody, parsedText, parsedError) && !parsedError.empty()) {
            apiMsg = parsedError;
        } else {
            apiMsg = StatusErrorMessage(statusCode);
        }

        // Retry on 429 (rate limit) or 5xx (server error)
        if ((statusCode == 429 || statusCode >= 500) && attempts < maxAttempts) {
            if (!InterruptibleSleep(1000 * attempts, cancelled, shutdown_)) {
                res.ok = false;
                res.error = L"Request cancelled";
                return res;
            }
            continue;
        }

        res.ok = false;
        res.error = apiMsg;
        res.errorDetail = StatusErrorMessage(statusCode);
        return res;
    }

    res.ok = false;
    res.error = L"OpenAI request failed after retries";
    return res;
}

} // namespace aiie
