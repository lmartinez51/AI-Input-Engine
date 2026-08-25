#include "UI/TrayApp.h"

#include "Configuration/LanguageRegistry.h"
#include "Core/Hotkey.h"
#include "Core/Utf.h"
#include "Providers/ProviderFactory.h"
#include "UI/DiagnosticsWindow.h"
#include "UI/SettingsDialog.h"
#include "UI/resource.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace aiie {

namespace {

constexpr wchar_t kClass[] = L"AIInputEngine.TrayWindow";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kTrayIconId = 1;
constexpr int kHotkeyToggle = 1;
constexpr int kHotkeySwap = 2;
constexpr int kHotkeyDiagnostics = 3;
constexpr int kHotkeySmartPaste = 4;
constexpr UINT kMenuToggle = 1001;
constexpr UINT kMenuSwap = 1002;
constexpr UINT kMenuSettings = 1003;
constexpr UINT kMenuDiagnostics = 1004;
constexpr UINT kMenuExit = 1005;
constexpr UINT kMenuSmartPaste = 1006;

enum class MenuItemType {
    Normal,
    Header,
    Separator,
};

struct ModernMenuItem {
    UINT id = 0;
    MenuItemType type = MenuItemType::Normal;
    std::wstring text;
    std::wstring shortcut;
    std::wstring glyph;
    COLORREF glyphColor = 0;
    bool checked = false;
    bool disabled = false;
    bool isBold = false;
    bool isAccent = false;
};

// Detects whether Windows 10/11 is currently using a Dark theme.
bool IsWindowsDarkTheme() {
    DWORD useLight = 1;
    DWORD size = sizeof(useLight);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme",
                     RRF_RT_REG_DWORD, nullptr, &useLight, &size) == ERROR_SUCCESS) {
        return useLight == 0;
    }
    return true; // Default to modern dark if key unavailable
}

enum PreferredAppMode {
    Default,
    AllowDark,
    ForceDark,
    ForceLight,
    Max
};

using fnSetPreferredAppMode = PreferredAppMode(WINAPI*)(PreferredAppMode);
using fnAllowDarkModeForWindow = BOOL(WINAPI*)(HWND, BOOL);

void EnableDarkMenuSupport(HWND hwnd) {
    HMODULE hUxTheme = GetModuleHandleW(L"uxtheme.dll");
    if (!hUxTheme) {
        hUxTheme = LoadLibraryW(L"uxtheme.dll");
    }
    if (hUxTheme) {
        auto setAppMode = reinterpret_cast<fnSetPreferredAppMode>(GetProcAddress(hUxTheme, MAKEINTRESOURCEA(135)));
        if (setAppMode) {
            setAppMode(PreferredAppMode::AllowDark);
        }
        auto allowDark = reinterpret_cast<fnAllowDarkModeForWindow>(GetProcAddress(hUxTheme, MAKEINTRESOURCEA(133)));
        if (allowDark) {
            allowDark(hwnd, TRUE);
        }
    }
}

// Loads the custom AIInputEngine icon or creates a clean fallback icon.
HICON MakeTrayIcon() {
    HICON hResIcon = reinterpret_cast<HICON>(LoadImageW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    if (hResIcon) {
        return hResIcon;
    }

    constexpr int kSize = 16;
    const HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);

    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = kSize;
    bi.bV5Height = kSize;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits,
                                   nullptr, 0);
    if (!bmp) {
        DeleteDC(mem);
        return nullptr;
    }
    HGDIOBJ old = SelectObject(mem, bmp);

    RECT rc{0, 0, kSize, kSize};
    HBRUSH bg = CreateSolidBrush(RGB(30, 27, 75));
    FillRect(mem, &rc, bg);
    DeleteObject(bg);

    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(56, 189, 248));
    SelectObject(mem, GetStockObject(DEFAULT_GUI_FONT));
    DrawTextW(mem, L"\u2728", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SelectObject(mem, old);
    DeleteDC(mem);

    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = bmp;
    ii.hbmMask = nullptr;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(bmp);
    return icon;
}

void DrawModernMenuItem(DRAWITEMSTRUCT* dis, const ModernMenuItem* item, bool isDark,
                        HFONT fontNormal, HFONT fontBold) {
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;

    const bool selected = (dis->itemState & ODS_SELECTED) && !item->disabled &&
                          item->type != MenuItemType::Separator &&
                          item->type != MenuItemType::Header;

    // 1. Menu Background
    const COLORREF bgCol = isDark ? RGB(24, 27, 34) : RGB(255, 255, 255);
    HBRUSH hBg = CreateSolidBrush(bgCol);
    FillRect(hdc, &rc, hBg);
    DeleteObject(hBg);

    // 2. Separator rendering
    if (item->type == MenuItemType::Separator) {
        const int midY = (rc.top + rc.bottom) / 2;
        HPEN pen = CreatePen(PS_SOLID, 1, isDark ? RGB(40, 48, 60) : RGB(226, 232, 240));
        HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, pen));
        MoveToEx(hdc, rc.left + 12, midY, nullptr);
        LineTo(hdc, rc.right - 12, midY);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
        return;
    }

    // 3. Selection / Hover highlight (rounded rect)
    if (selected) {
        RECT rcHover = rc;
        InflateRect(&rcHover, -4, -1);
        HBRUSH hHover = CreateSolidBrush(isDark ? RGB(38, 47, 61) : RGB(241, 245, 249));
        HPEN penHover = CreatePen(PS_SOLID, 1, isDark ? RGB(56, 189, 248) : RGB(203, 213, 225));
        HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(hdc, hHover));
        HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, penHover));
        RoundRect(hdc, rcHover.left, rcHover.top, rcHover.right, rcHover.bottom, 6, 6);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(hHover);
        DeleteObject(penHover);
    }

    SetBkMode(hdc, TRANSPARENT);

    // 4. Icon / Glyph column (width ~26px)
    RECT rcIcon = rc;
    rcIcon.left += 8;
    rcIcon.right = rcIcon.left + 24;

    if (!item->glyph.empty()) {
        COLORREF gCol = item->glyphColor;
        if (gCol == 0) {
            if (item->disabled) {
                gCol = isDark ? RGB(75, 85, 99) : RGB(180, 190, 205);
            } else if (item->isAccent) {
                gCol = isDark ? RGB(56, 189, 248) : RGB(2, 132, 199);
            } else {
                gCol = isDark ? RGB(203, 213, 225) : RGB(71, 85, 105);
            }
        }
        SetTextColor(hdc, gCol);
        HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, fontNormal));
        DrawTextW(hdc, item->glyph.c_str(), -1, &rcIcon, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, oldFont);
    }

    // 5. Main text column
    RECT rcText = rc;
    rcText.left += 34;
    rcText.right -= 12;

    COLORREF textCol;
    if (item->disabled) {
        if (item->type == MenuItemType::Header) {
            textCol = item->isAccent ? (isDark ? RGB(56, 189, 248) : RGB(2, 132, 199))
                                     : (isDark ? RGB(148, 163, 184) : RGB(100, 116, 139));
        } else {
            textCol = isDark ? RGB(75, 85, 99) : RGB(175, 185, 200);
        }
    } else if (selected) {
        textCol = isDark ? RGB(255, 255, 255) : RGB(15, 23, 42);
    } else {
        textCol = isDark ? RGB(241, 245, 249) : RGB(30, 41, 59);
    }

    SetTextColor(hdc, textCol);
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, item->isBold ? fontBold : fontNormal));
    DrawTextW(hdc, item->text.c_str(), -1, &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 6. Shortcut text (right-aligned)
    if (!item->shortcut.empty()) {
        RECT rcShort = rc;
        rcShort.right -= 12;
        const COLORREF scCol = item->disabled ? (isDark ? RGB(65, 75, 90) : RGB(190, 200, 215))
                                              : (isDark ? RGB(100, 116, 139) : RGB(148, 163, 184));
        SetTextColor(hdc, scCol);
        DrawTextW(hdc, item->shortcut.c_str(), -1, &rcShort, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    SelectObject(hdc, oldFont);
}

} // namespace

TrayApp::TrayApp() = default;

TrayApp::~TrayApp() {
    UnregisterHotkeys();
    if (backend_) {
        backend_->Stop();
    }
    if (hwnd_ && icon_) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = kTrayIconId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }
    if (icon_) {
        DestroyIcon(icon_);
    }
    if (menuFont_ && menuFont_ != GetStockObject(DEFAULT_GUI_FONT)) {
        DeleteObject(menuFont_);
    }
    if (menuFontBold_ && menuFontBold_ != menuFont_ && menuFontBold_ != GetStockObject(DEFAULT_GUI_FONT)) {
        DeleteObject(menuFontBold_);
    }
    if (hwnd_) {
        DestroyWindow(hwnd_);
    }
}

bool TrayApp::Init(HINSTANCE instance) {
    instance_ = instance;

    // Process-wide logger (single instance, file sink + ring buffer).
    logger_ = std::shared_ptr<Logger>(&Logger::Instance(), [](Logger*) {});
    logger_->SetLogFile((ConfigStore::DefaultLogDir() / L"aiie.log").wstring());

    config_.Load(); // parse failure keeps defaults (already logged by store)

    WNDCLASS wc{};
    wc.lpfnWndProc = &TrayApp::WndProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    if (!RegisterClassW(&wc)) {
        Logger::Instance().Error(cat::Ui, L"RegisterClassW failed: {}", GetLastError());
        return false;
    }
    hwnd_ = CreateWindowExW(0, kClass, L"AI Input Engine", WS_OVERLAPPEDWINDOW, 0, 0, 0, 0,
                            nullptr, nullptr, instance_, this);
    if (!hwnd_) {
        Logger::Instance().Error(cat::Ui, L"CreateWindow failed: {}", GetLastError());
        return false;
    }

    EnableDarkMenuSupport(hwnd_);

    // Segoe UI font for modern popup menu
    const UINT dpi = GetDpiForSystem();
    menuFont_ = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    menuFontBold_ = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (!menuFont_) menuFont_ = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    if (!menuFontBold_) menuFontBold_ = menuFont_;

    // Everything the engine touches must be created on this (UI) thread so
    // the message dispatcher's window lives here.
    dispatcher_ = std::make_shared<MessageDispatcher>();
    cache_ = std::make_shared<TranslationCache>(static_cast<size_t>(config_.Get().cacheMaxEntries));
    backend_ = std::make_shared<HookBackend>();
    status_ = std::make_shared<StatusWindow>();
    backend_->SetStatusCallback([this](std::wstring_view msg, bool show) { SetStatus(msg, show); });

    ApplyConfig();

    backend_->SetEngine(engine_.get());
    backend_->Start();

    icon_ = MakeTrayIcon();
    if (!icon_) {
        icon_ = static_cast<HICON>(LoadImageW(nullptr, IDI_APPLICATION, IMAGE_ICON, 16, 16,
                                              LR_SHARED));
    }

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = kTrayMessage;
    nid.hIcon = icon_;
    std::wstring tip = L"AI Input Engine \u2014 ";
    tip += pipeline_ && pipeline_->Provider() ? pipeline_->Provider()->DisplayName()
                                              : std::wstring(L"no provider");
    wcscpy_s(nid.szTip, tip.c_str());
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        Logger::Instance().Error(cat::Ui, L"Shell_NotifyIcon failed: {}", GetLastError());
        return false;
    }

    Logger::Instance().Info(cat::Ui, L"AI Input Engine started (provider: {})",
                            ProviderName(config_.Get().provider));
    return true;
}

int TrayApp::Run() {
    MSG msg;
    while (!exitRequested_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return exitRequested_ ? 0 : 1;
}

void TrayApp::OnToggleHotkey() {
    AppConfig cfg = config_.Get();
    cfg.enabled = !cfg.enabled;
    config_.Set(cfg);
    config_.Save();
    engine_->SetEnabled(cfg.enabled);
    SetStatus(cfg.enabled ? L"AI Input Engine: ON" : L"AI Input Engine: OFF", true);
    Logger::Instance().Info(cat::Ui, L"toggled {}", cfg.enabled ? L"on" : L"off");
}

void TrayApp::OnSwapHotkey() {
    AppConfig cfg = config_.Get();
    if (cfg.operation == TransformOperation::CorrectAndRewrite) {
        SetStatus(L"Swap is not applicable in Correct & Rewrite mode", true);
        return;
    }
    if (cfg.sourceLanguage == L"auto") {
        SetStatus(L"Cannot swap: source is Auto-Detect", true);
        Logger::Instance().Warn(cat::Ui, L"swap requested with auto source");
        return;
    }
    std::swap(cfg.sourceLanguage, cfg.targetLanguage);
    config_.Set(cfg);
    config_.Save();
    ApplyConfig();
    const std::wstring srcName(LanguageDisplayName(cfg.sourceLanguage));
    const std::wstring tgtName(LanguageDisplayName(cfg.targetLanguage));
    SetStatus(L"Languages: " + srcName + L" \u2192 " + tgtName, true);
}

void TrayApp::OnDiagnosticsHotkey() {
    ShowDiagnostics();
}

void TrayApp::OnSmartPasteHotkey() {
    if (smartPaste_) {
        smartPaste_->Trigger();
    }
}

void TrayApp::ApplyConfig() {
    const AppConfig& cfg = config_.Get();

    logger_->SetLevel(cfg.logLevel);
    logger_->SetLogRawText(cfg.logRawText);
    cache_->Configure(cfg.cacheEnabled, static_cast<size_t>(cfg.cacheMaxEntries));

    Logger::Instance().Info(cat::Config,
                            L"apply config: provider={} source={} target={} debounce={} minChars={} level={}",
                            ProviderName(cfg.provider), cfg.sourceLanguage, cfg.targetLanguage,
                            cfg.debounceMs, cfg.minChars, static_cast<int>(cfg.logLevel));

    if (!engine_) {
        pipeline_ = std::make_shared<TranslationPipeline>(
            ProviderFactory::Create(cfg), cache_, cfg.requestTimeoutMs);
        engine_ = std::make_shared<InputEngine>(dispatcher_, backend_, pipeline_, logger_);
        smartPaste_ = std::make_shared<SmartPasteManager>(pipeline_, status_, &config_, dispatcher_);
    }

    RebuildProvider();
    engine_->ApplyConfig(cfg);
    backend_->ApplyConfig(cfg);
    UnregisterHotkeys();
    RegisterHotkeys();
}

void TrayApp::ReloadConfig() {
    if (!config_.Load()) {
        SetStatus(L"Config parse failed; keeping current settings", true);
        return;
    }
    ApplyConfig();
    SetStatus(L"Configuration reloaded", true);
}

void TrayApp::RebuildProvider() {
    const AppConfig& cfg = config_.Get();
    auto provider = ProviderFactory::Create(cfg);
    if (!provider) {
        Logger::Instance().Error(cat::Ui, L"no provider created for {}", ProviderName(cfg.provider));
        return;
    }
    cache_->Clear();
    pipeline_->SetProvider(provider);
    Logger::Instance().Info(cat::Ui, L"provider: {}", provider->DisplayName());
}

void TrayApp::RegisterHotkeys() {
    const AppConfig& cfg = config_.Get();
    if (cfg.hotkeyToggle.IsValid() && !RegisterHotKey(hwnd_, kHotkeyToggle, cfg.hotkeyToggle.modifiers,
                                                      cfg.hotkeyToggle.vk)) {
        Logger::Instance().Warn(cat::Ui, L"RegisterHotKey toggle failed: {}", GetLastError());
    }
    if (cfg.hotkeySwap.IsValid() && !RegisterHotKey(hwnd_, kHotkeySwap, cfg.hotkeySwap.modifiers,
                                                    cfg.hotkeySwap.vk)) {
        Logger::Instance().Warn(cat::Ui, L"RegisterHotKey swap failed: {}", GetLastError());
    }
    if (cfg.hotkeyDiagnostics.IsValid() &&
        !RegisterHotKey(hwnd_, kHotkeyDiagnostics, cfg.hotkeyDiagnostics.modifiers,
                        cfg.hotkeyDiagnostics.vk)) {
        Logger::Instance().Warn(cat::Ui, L"RegisterHotKey diagnostics failed: {}", GetLastError());
    }
    if (cfg.smartPasteEnabled && cfg.hotkeySmartPaste.IsValid() &&
        !RegisterHotKey(hwnd_, kHotkeySmartPaste, cfg.hotkeySmartPaste.modifiers,
                        cfg.hotkeySmartPaste.vk)) {
        Logger::Instance().Warn(cat::Ui, L"RegisterHotKey smart paste failed: {}", GetLastError());
    }
}

void TrayApp::UnregisterHotkeys() {
    UnregisterHotKey(hwnd_, kHotkeyToggle);
    UnregisterHotKey(hwnd_, kHotkeySwap);
    UnregisterHotKey(hwnd_, kHotkeyDiagnostics);
    UnregisterHotKey(hwnd_, kHotkeySmartPaste);
}

void TrayApp::UpdateTrayMenu() {
    const AppConfig& cfg = config_.Get();
    const bool enabled = cfg.enabled;
    const bool isTranslate = (cfg.operation == TransformOperation::Translate);

    std::vector<std::unique_ptr<ModernMenuItem>> items;
    auto addItem = [&](ModernMenuItem item) {
        items.push_back(std::make_unique<ModernMenuItem>(std::move(item)));
        return items.back().get();
    };

    HMENU menu = CreatePopupMenu();

    // 1. Enable / Disable toggle
    ModernMenuItem toggleItem;
    toggleItem.id = kMenuToggle;
    toggleItem.text = enabled ? L"Enabled" : L"Disabled";
    toggleItem.shortcut = L"Ctrl+Shift+Space";
    toggleItem.checked = enabled;
    toggleItem.glyph = enabled ? L"\u2713" : L"\u25CB"; // ✓ or ○
    toggleItem.glyphColor = enabled ? RGB(56, 189, 248) : RGB(100, 116, 139);
    toggleItem.isBold = true;
    auto* pToggle = addItem(std::move(toggleItem));
    AppendMenuW(menu, MF_OWNERDRAW | MF_ENABLED, pToggle->id, reinterpret_cast<LPCWSTR>(pToggle));

    // Separator
    ModernMenuItem sep1;
    sep1.type = MenuItemType::Separator;
    auto* pSep1 = addItem(std::move(sep1));
    AppendMenuW(menu, MF_OWNERDRAW, 0, reinterpret_cast<LPCWSTR>(pSep1));

    // 2. Context-Aware Mode Header
    ModernMenuItem modeItem;
    modeItem.type = MenuItemType::Header;
    modeItem.disabled = true;
    modeItem.isBold = true;
    modeItem.isAccent = true;
    modeItem.glyph = L"\u2726"; // ✦
    const std::wstring srcName(LanguageDisplayName(cfg.sourceLanguage));
    const std::wstring tgtName(LanguageDisplayName(cfg.targetLanguage));
    if (isTranslate) {
        modeItem.text = L"Translate: " + srcName + L" \u2192 " + tgtName;
    } else {
        modeItem.text = L"Correct & Rewrite: " + srcName;
    }
    auto* pMode = addItem(std::move(modeItem));
    AppendMenuW(menu, MF_OWNERDRAW | MF_GRAYED, 0, reinterpret_cast<LPCWSTR>(pMode));

    // 3. Swap Languages (Disabled/Grayed in CorrectAndRewrite mode per Requirement 5)
    ModernMenuItem swapItem;
    swapItem.id = kMenuSwap;
    swapItem.text = L"Swap Languages";
    swapItem.shortcut = L"Ctrl+Shift+S";
    swapItem.glyph = L"\u21C4"; // ⇄
    if (!isTranslate || cfg.sourceLanguage == L"auto") {
        swapItem.disabled = true;
    }
    auto* pSwap = addItem(std::move(swapItem));
    AppendMenuW(menu, MF_OWNERDRAW | (pSwap->disabled ? MF_GRAYED : MF_ENABLED), pSwap->id,
                reinterpret_cast<LPCWSTR>(pSwap));

    // 4. Writing Profile / Translation Profile Info
    ModernMenuItem profileItem;
    profileItem.type = MenuItemType::Header;
    profileItem.disabled = true;
    profileItem.glyph = isTranslate ? L"\u2022" : L"\u270D"; // • or ✍
    std::wstring profName(ProfileDisplayName(cfg.profile, cfg.operation));
    const size_t paren = profName.find(L" (");
    if (paren != std::wstring::npos) {
        profName = profName.substr(0, paren);
    }
    if (isTranslate) {
        profileItem.text = L"Profile: " + profName;
    } else {
        profileItem.text = L"Writing Profile: " + profName;
    }
    auto* pProfile = addItem(std::move(profileItem));
    AppendMenuW(menu, MF_OWNERDRAW | MF_GRAYED, 0, reinterpret_cast<LPCWSTR>(pProfile));

    // 5. Provider & Model Info
    ModernMenuItem provItem;
    provItem.type = MenuItemType::Header;
    provItem.disabled = true;
    provItem.glyph = L"\u26A1"; // ⚡
    std::wstring provSummary = std::wstring(pipeline_ && pipeline_->Provider() ? pipeline_->Provider()->DisplayName()
                                                                               : L"Mock (offline demo)");
    if (cfg.provider != ProviderId::Mock && !cfg.model.empty()) {
        provSummary += L" \u2022 " + cfg.model;
    }
    provItem.text = provSummary;
    auto* pProv = addItem(std::move(provItem));
    AppendMenuW(menu, MF_OWNERDRAW | MF_GRAYED, 0, reinterpret_cast<LPCWSTR>(pProv));

    // 5.5 Smart Paste state
    ModernMenuItem pasteItem;
    pasteItem.type = MenuItemType::Header;
    pasteItem.disabled = true;
    pasteItem.glyph = L"\u29C9"; // ⧉
    pasteItem.shortcut = FormatHotkey(cfg.hotkeySmartPaste);
    pasteItem.text = cfg.smartPasteEnabled ? L"Smart Paste: Enabled" : L"Smart Paste: Disabled";
    auto* pPaste = addItem(std::move(pasteItem));
    AppendMenuW(menu, MF_OWNERDRAW | MF_GRAYED, 0, reinterpret_cast<LPCWSTR>(pPaste));

    // Separator
    ModernMenuItem sep2;
    sep2.type = MenuItemType::Separator;
    auto* pSep2 = addItem(std::move(sep2));
    AppendMenuW(menu, MF_OWNERDRAW, 0, reinterpret_cast<LPCWSTR>(pSep2));

    // 6. Settings
    ModernMenuItem setItem;
    setItem.id = kMenuSettings;
    setItem.text = L"Settings\u2026";
    setItem.glyph = L"\u2699"; // ⚙
    auto* pSet = addItem(std::move(setItem));
    AppendMenuW(menu, MF_OWNERDRAW | MF_ENABLED, pSet->id, reinterpret_cast<LPCWSTR>(pSet));

    // 7. Diagnostics
    ModernMenuItem diagItem;
    diagItem.id = kMenuDiagnostics;
    diagItem.text = L"Diagnostics\u2026";
    diagItem.shortcut = L"Ctrl+Shift+D";
    diagItem.glyph = L"\u2261"; // ≡
    auto* pDiag = addItem(std::move(diagItem));
    AppendMenuW(menu, MF_OWNERDRAW | MF_ENABLED, pDiag->id, reinterpret_cast<LPCWSTR>(pDiag));

    // Separator
    ModernMenuItem sep3;
    sep3.type = MenuItemType::Separator;
    auto* pSep3 = addItem(std::move(sep3));
    AppendMenuW(menu, MF_OWNERDRAW, 0, reinterpret_cast<LPCWSTR>(pSep3));

    // 8. Exit
    ModernMenuItem exitItem;
    exitItem.id = kMenuExit;
    exitItem.text = L"Exit";
    exitItem.glyph = L"\u23FB"; // ⏻
    auto* pExit = addItem(std::move(exitItem));
    AppendMenuW(menu, MF_OWNERDRAW | MF_ENABLED, pExit->id, reinterpret_cast<LPCWSTR>(pExit));

    SetForegroundWindow(hwnd_);
    const POINT pt = [] {
        POINT p{};
        GetCursorPos(&p);
        return p;
    }();
    const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y,
                                    0, hwnd_, nullptr);
    DestroyMenu(menu);

    if (cmd == 0) {
        return;
    }
    switch (cmd) {
    case kMenuToggle:
        OnToggleHotkey();
        break;
    case kMenuSwap:
        OnSwapHotkey();
        break;
    case kMenuSettings:
        ShowSettingsDialog();
        break;
    case kMenuDiagnostics:
        ShowDiagnostics();
        break;
    case kMenuExit:
        Logger::Instance().Info(cat::Ui, L"exit requested");
        exitRequested_ = true;
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        break;
    default:
        break;
    }
}

void TrayApp::ShowSettingsDialog() {
    AppConfig cfg = config_.Get();
    if (aiie::ShowSettingsDialog(hwnd_, cfg)) {
        config_.Set(cfg);
        config_.Save();
        ApplyConfig();
        SetStatus(L"Settings applied", true);
    }
}

void TrayApp::ShowDiagnostics() {
    if (IsDiagnosticsOpen()) {
        return; // already visible
    }
    ShowDiagnosticsWindow(pipeline_->Provider(), hwnd_);
}

void TrayApp::SetStatus(std::wstring_view message, bool show) {
    if (!status_) {
        return;
    }
    if (show) {
        status_->Show(message);
    } else {
        status_->Hide();
    }
}

LRESULT CALLBACK TrayApp::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<TrayApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCCREATE: {
        const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return TRUE;
    }
    case WM_MEASUREITEM: {
        auto* mis = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        if (mis->CtlType == ODT_MENU && mis->itemData && self) {
            auto* item = reinterpret_cast<ModernMenuItem*>(mis->itemData);
            HDC hdc = GetDC(hwnd);
            HFONT old = static_cast<HFONT>(SelectObject(hdc, item->isBold ? self->menuFontBold_ : self->menuFont_));
            SIZE szText{}, szShort{};
            GetTextExtentPoint32W(hdc, item->text.c_str(), static_cast<int>(item->text.size()), &szText);
            if (!item->shortcut.empty()) {
                GetTextExtentPoint32W(hdc, item->shortcut.c_str(), static_cast<int>(item->shortcut.size()), &szShort);
            }
            SelectObject(hdc, old);
            ReleaseDC(hwnd, hdc);

            if (item->type == MenuItemType::Separator) {
                mis->itemHeight = 9;
                mis->itemWidth = 240;
            } else if (item->type == MenuItemType::Header) {
                mis->itemHeight = 26;
                mis->itemWidth = szText.cx + 50;
            } else {
                mis->itemHeight = 30;
                const int shortW = item->shortcut.empty() ? 0 : (szShort.cx + 24);
                mis->itemWidth = szText.cx + shortW + 56;
                if (mis->itemWidth < 240) {
                    mis->itemWidth = 240;
                }
            }
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (dis->CtlType == ODT_MENU && dis->itemData && self) {
            auto* item = reinterpret_cast<ModernMenuItem*>(dis->itemData);
            const bool isDark = IsWindowsDarkTheme();
            DrawModernMenuItem(dis, item, isDark, self->menuFont_, self->menuFontBold_);
            return TRUE;
        }
        break;
    }
    case WM_HOTKEY:
        if (self) {
            switch (wp) {
            case kHotkeyToggle:
                self->OnToggleHotkey();
                break;
            case kHotkeySwap:
                self->OnSwapHotkey();
                break;
            case kHotkeyDiagnostics:
                self->OnDiagnosticsHotkey();
                break;
            case kHotkeySmartPaste:
                self->OnSmartPasteHotkey();
                break;
            default:
                break;
            }
        }
        return 0;
    case kTrayMessage:
        if (self) {
            switch (LOWORD(lp)) {
            case WM_RBUTTONUP:
            case WM_CONTEXTMENU:
                self->UpdateTrayMenu();
                break;
            case WM_LBUTTONDBLCLK:
                self->OnToggleHotkey();
                break;
            default:
                break;
            }
        }
        return 0;
    case WM_CLOSE:
        if (self) {
            self->exitRequested_ = true;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace aiie