#include "Core/Logging.h"
#include "Core/Utf.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace aiie {

namespace {
const wchar_t* LevelName(LogLevel l) {
    switch (l) {
    case LogLevel::Error: return L"ERROR";
    case LogLevel::Warn: return L"WARN";
    case LogLevel::Info: return L"INFO";
    case LogLevel::Debug: return L"DEBUG";
    case LogLevel::Trace: return L"TRACE";
    }
    return L"?";
}

std::wstring NowTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    std::wostringstream os;
    os << std::put_time(&tm, L"%Y-%m-%d %H:%M:%S") << L"." << std::setw(3) << std::setfill(L'0') << ms.count();
    return os.str();
}
} // namespace

Logger& Logger::Instance() {
    static Logger logger;
    return logger;
}

void Logger::SetLevel(LogLevel level) { level_.store(level); }

void Logger::SetLogRawText(bool enabled) { logRawText_.store(enabled); }

void Logger::SetLogFile(std::wstring_view path) {
    std::lock_guard lock(fileMutex_);
    filePath_.assign(path);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    // Open immediately to validate the path; failure is non-fatal (log to nowhere).
    std::ofstream f(filePath_, std::ios::app);
    (void)f;
}

void Logger::Log(LogLevel level, std::wstring_view category, const std::wstring& message) {
    if (static_cast<int>(level) > static_cast<int>(level_.load())) {
        return;
    }
    const std::wstring line = std::format(L"[{}][{}][{}] {}",
                                          NowTimestamp(), LevelName(level), category, message);

    {
        std::lock_guard lock(ringMutex_);
        if (ring_.size() >= kRingCapacity) {
            ring_.erase(ring_.begin(), ring_.begin() + static_cast<ptrdiff_t>(ring_.size() - kRingCapacity + 1));
        }
        ring_.push_back(LogEntry{level, std::wstring(category), message, GetTickCount64()});
    }

    std::lock_guard lock(fileMutex_);
    if (filePath_.empty()) {
        return;
    }
    std::ofstream f(filePath_, std::ios::app);
    if (f) {
        const std::string utf8 = utf::ToUtf8(line);
        f << utf8 << '\n';
    }
}

std::vector<LogEntry> Logger::Snapshot() const {
    std::lock_guard lock(ringMutex_);
    return ring_;
}

void Logger::ClearRing() {
    std::lock_guard lock(ringMutex_);
    ring_.clear();
}

std::wstring Logger::Redact(std::wstring_view text, std::wstring_view secret) {
    if (secret.empty() || secret.size() < 4) {
        return std::wstring(text);
    }
    std::wstring out(text);
    size_t pos = 0;
    while ((pos = out.find(secret, pos)) != std::wstring::npos) {
        out.replace(pos, secret.size(), L"***");
        pos += 3;
    }
    return out;
}

} // namespace aiie