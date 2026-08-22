#pragma once

#include <atomic>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace aiie {

enum class LogLevel { Error = 0, Warn = 1, Info = 2, Debug = 3, Trace = 4 };

// Category names used across subsystems.
namespace cat {
inline constexpr std::wstring_view Core = L"CORE";
inline constexpr std::wstring_view Input = L"INPUT";
inline constexpr std::wstring_view TSF = L"TSF";
inline constexpr std::wstring_view Composition = L"COMPOSITION";
inline constexpr std::wstring_view Translation = L"TRANSLATION";
inline constexpr std::wstring_view Gemini = L"GEMINI";
inline constexpr std::wstring_view Ui = L"UI";
inline constexpr std::wstring_view Config = L"CONFIG";
} // namespace cat

struct LogEntry {
    LogLevel level;
    std::wstring category;
    std::wstring message;
    uint64_t tickMs;
};

// Thread-safe structured logger. File sink (UTF-8) + in-memory ring buffer
// that the diagnostics window can display. Never logs raw user text unless
// debug logging with raw-text logging explicitly enabled. Never logs API keys.
class Logger {
public:
    static Logger& Instance();

    void SetLevel(LogLevel level);
    LogLevel Level() const { return level_.load(); }
    // Enable/disable logging of raw user text (privacy). Default off.
    void SetLogRawText(bool enabled);
    bool LogRawText() const { return logRawText_.load(); }
    void SetLogFile(std::wstring_view path);

    void Log(LogLevel level, std::wstring_view category, const std::wstring& message);

    template <typename... Args>
    void LogFmt(LogLevel level, std::wstring_view category, std::wstring_view fmt, Args&&... args) {
        Log(level, category, std::vformat(fmt, std::make_wformat_args(args...)));
    }

    template <typename... Args>
    void Error(std::wstring_view category, std::wstring_view fmt, Args&&... args) {
        LogFmt(LogLevel::Error, category, fmt, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void Warn(std::wstring_view category, std::wstring_view fmt, Args&&... args) {
        LogFmt(LogLevel::Warn, category, fmt, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void Info(std::wstring_view category, std::wstring_view fmt, Args&&... args) {
        LogFmt(LogLevel::Info, category, fmt, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void Debug(std::wstring_view category, std::wstring_view fmt, Args&&... args) {
        LogFmt(LogLevel::Debug, category, fmt, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void Trace(std::wstring_view category, std::wstring_view fmt, Args&&... args) {
        LogFmt(LogLevel::Trace, category, fmt, std::forward<Args>(args)...);
    }

    // Copy of the recent ring buffer (for the diagnostics window).
    std::vector<LogEntry> Snapshot() const;
    void ClearRing();

    // Redact secrets from text before logging (API keys, bearer tokens, etc.).
    static std::wstring Redact(std::wstring_view text, std::wstring_view secret);

private:
    Logger() = default;
    void WriteLine(std::wstring_view line);

    std::atomic<LogLevel> level_{LogLevel::Info};
    std::atomic<bool> logRawText_{false};
    mutable std::mutex fileMutex_;
    std::wstring filePath_;
    mutable std::mutex ringMutex_;
    std::vector<LogEntry> ring_;
    static constexpr size_t kRingCapacity = 512;
};

} // namespace aiie