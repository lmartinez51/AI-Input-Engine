#pragma once

#include "Composition/InputEngine.h"
#include "Configuration/ConfigStore.h"
#include "Core/Dispatcher.h"
#include "Core/Logging.h"
#include "Core/StatusWindow.h"
#include "Input/HookBackend.h"
#include "Translation/TranslationCache.h"
#include "Translation/TranslationPipeline.h"

#include <windows.h>

#include <memory>
#include <string>

namespace aiie {

// System tray host for AI Input Engine. Owns the hook backend, the engine,
// the pipeline, configuration, and the modern context menu. Runs a message window
// (the UI thread): all engine callbacks are marshalled here via the message
// dispatcher.
class TrayApp {
public:
    TrayApp();
    ~TrayApp();

    TrayApp(const TrayApp&) = delete;
    TrayApp& operator=(const TrayApp&) = delete;

    bool Init(HINSTANCE instance);
    int Run(); // message loop until exit requested

    // Called when a global hotkey was registered to fire (WM_HOTKEY).
    void OnToggleHotkey();
    void OnSwapHotkey();
    void OnDiagnosticsHotkey();

private:
    void ApplyConfig();
    void ReloadConfig();
    void RebuildProvider();
    void UpdateTrayMenu();
    void ShowSettingsDialog();
    void ShowDiagnostics();
    void SetStatus(std::wstring_view message, bool show);
    void RegisterHotkeys();
    void UnregisterHotkeys();

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HICON icon_ = nullptr;
    HFONT menuFont_ = nullptr;
    HFONT menuFontBold_ = nullptr;
    bool exitRequested_ = false;

    std::shared_ptr<Logger> logger_; // non-owning: wraps the process-wide Logger::Instance()
    ConfigStore config_;
    std::shared_ptr<MessageDispatcher> dispatcher_;
    std::shared_ptr<TranslationCache> cache_;
    std::shared_ptr<TranslationPipeline> pipeline_;
    std::shared_ptr<HookBackend> backend_;
    std::shared_ptr<InputEngine> engine_;
    std::shared_ptr<StatusWindow> status_;
};

} // namespace aiie