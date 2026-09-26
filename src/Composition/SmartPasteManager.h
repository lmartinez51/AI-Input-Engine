#pragma once

#include "Configuration/ConfigStore.h"
#include "Core/ClipboardHelper.h"
#include "Core/Dispatcher.h"
#include "Core/StatusWindow.h"
#include "Translation/TranslationPipeline.h"

#include <windows.h>

#include <atomic>
#include <memory>
#include <string>

namespace aiie {

enum class SmartPasteDecision {
    Transform,      // Send to TranslationPipeline
    PasteVerbatim,  // Bypass pipeline, paste original text directly
    Blocked         // Refuse paste (e.g. unsupported format, target changed)
};

enum class ContentKind {
    NaturalLanguage,
    SourceCode,
    SecretOrCredential,
    LiteralOrUrl,
    LanguageMatch
};

struct SmartPasteAnalysis {
    SmartPasteDecision decision = SmartPasteDecision::Transform;
    ContentKind kind = ContentKind::NaturalLanguage;
    std::wstring reason;
    bool isSensitive = false;
    bool isCode = false;
    bool isShortCircuit = false;
};

// Encapsulates the execution and lifecycle of Smart Paste (Ctrl+Shift+V).
// Reuses the active TranslationPipeline, preserves original clipboard state,
// and guards against window-switch pasting.
class SmartPasteManager {
public:
    SmartPasteManager(std::shared_ptr<TranslationPipeline> pipeline,
                      std::shared_ptr<StatusWindow> status,
                      ConfigStore* configStore,
                      std::shared_ptr<IDispatcher> dispatcher);
    ~SmartPasteManager();

    SmartPasteManager(const SmartPasteManager&) = delete;
    SmartPasteManager& operator=(const SmartPasteManager&) = delete;

    // Sets an optional pre-paste coordination hook (e.g. to flush active composition in InputEngine).
    void SetPrePasteHook(std::function<void()> hook);

    // Triggered when Ctrl+Shift+V hotkey is pressed (or manually in tests).
    // Returns true if a Smart Paste request was accepted and launched.
    bool Trigger(HWND targetHwndOverride = nullptr);

    // Returns true if a Smart Paste request is currently in-flight.
    bool IsBusy() const { return busy_.load(); }

    // Analyzes clipboard content using current configuration.
    SmartPasteAnalysis AnalyzeContent(std::wstring_view text) const;

    // Analyzes clipboard content and target context to determine if the text should be
    // transformed by the pipeline or short-circuited/pasted verbatim.
    static SmartPasteAnalysis AnalyzeContent(std::wstring_view text,
                                             const AppConfig& config,
                                             HWND targetHwnd = nullptr,
                                             DWORD processId = 0);

    // Classification helpers
    static bool IsSensitiveOrSecret(std::wstring_view text);
    static bool IsSourceCode(std::wstring_view text, bool isEditorContext = false);
    static bool IsLiteralOrShortCircuit(std::wstring_view text, const AppConfig& config);
    static bool IsEditorExecutable(std::wstring_view exeName);
    static bool IsEditorProcess(DWORD processId, HWND hwnd = nullptr);
    static std::wstring GetProcessExecutableName(DWORD processId);

private:
    struct TargetContext {
        HWND targetHwnd = nullptr;
        HWND focusHwnd = nullptr;
        DWORD processId = 0;
        DWORD threadId = 0;
    };

    void OnTranslationCompleted(int64_t requestId,
                                TargetContext target,
                                DWORD origSequence,
                                std::wstring originalText,
                                TranslationResult result);

    std::shared_ptr<TranslationPipeline> pipeline_;
    std::shared_ptr<StatusWindow> status_;
    ConfigStore* configStore_ = nullptr;
    std::shared_ptr<IDispatcher> dispatcher_;
    std::function<void()> prePasteHook_;

    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    std::atomic<bool> busy_{false};
    std::atomic<int64_t> currentRequestId_{0};
    int64_t reqCounter_ = 0;
};

} // namespace aiie
