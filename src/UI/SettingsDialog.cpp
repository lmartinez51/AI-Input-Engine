#include "UI/SettingsDialog.h"

#include "Configuration/CredentialStore.h"
#include "Configuration/LanguageRegistry.h"
#include "Configuration/ModelCapabilities.h"
#include "Core/Logging.h"
#include "Providers/GeminiProvider.h"
#include "Providers/OpenAIProvider.h"
#include "UI/resource.h"

#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace aiie {

namespace {

constexpr wchar_t kClass[] = L"AIInputEngine.SettingsDialog";

// Control IDs.
enum Ctrl : int {
    kOk = 1,
    kCancel = 2,
    kEnabled = 10,
    kProvider = 11,
    kSourceLang = 12,
    kTargetLang = 13,
    kProfile = 14,
    kCustomInstructions = 15,
    kModel = 16,
    kEndpoint = 17,
    kApiKey = 18,
    kSaveKey = 19,
    kRemoveKey = 20,
    kTestKey = 21,
    kKeyStatus = 22,
    kAdvancedToggle = 23,
    kDebounce = 24,
    kMinChars = 25,
    kMaxBuffer = 26,
    kTemperature = 27,
    kTopP = 28,
    kTopK = 29,
    kMaxTokens = 30,
    kOperation = 31,
    kSmartPaste = 32,
    kSpeculativeTranslation = 33,
    kSpeculativeNote = 34,
};

constexpr UINT kTestResultMsg = WM_APP + 100;

struct TestResult {
    bool ok = false;
    std::wstring message;
};

struct DialogData {
    AppConfig config;
    bool accepted = false;
    HWND hwnd = nullptr;
    HFONT font = nullptr;
    HFONT fontBold = nullptr;
    HFONT fontTitle = nullptr;
    ProviderId currentProvider = ProviderId::Mock;
    TransformOperation currentOperation = TransformOperation::Translate;
    bool updatingControls = false;
    bool expanded = false;
    UINT dpi = 96;
    std::vector<HWND> advancedControls;
    HWND lblSection1 = nullptr;
    HWND lblSource = nullptr;
    HWND comboSource = nullptr;
    HWND lblTarget = nullptr;
    HWND comboTarget = nullptr;
    HWND lblProfile = nullptr;
    HWND comboProfile = nullptr;
};

// Base layout dimensions (at 96 DPI).
constexpr int kMarginX = 20;
constexpr int kLabelW = 130;
constexpr int kGap = 10;
constexpr int kControlW = 340;
constexpr int kControlH = 24;
constexpr int kRowAdvance = 28;

constexpr int kFullW = kLabelW + kGap + kControlW; // 480
constexpr int kClientW = kMarginX * 2 + kFullW;    // 520
constexpr int kBaseClientH = 552;
constexpr int kExpandedClientH = 720;

// Upward opening bias (at 96 DPI) so expanded dialog keeps action buttons on-screen.
constexpr int kOpenUpwardBiasDip = 85;

// Advanced Settings 2-Column Grid dimensions (at 96 DPI).
constexpr int kAdvColGap = 16;
constexpr int kAdvColW = (kFullW - kAdvColGap) / 2; // 232
constexpr int kAdvLabelW = 130;
constexpr int kAdvGap = 8;
constexpr int kAdvNumEditW = 84;
constexpr int kAdvRowAdvance = 28;

UINT GetWindowDpi(HWND hwnd) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        auto pfn = reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(user32, "GetDpiForWindow"));
        if (pfn && hwnd) {
            UINT dpi = pfn(hwnd);
            if (dpi != 0) return dpi;
        }
        using GetDpiForSystemFn = UINT(WINAPI*)();
        auto pfnSys = reinterpret_cast<GetDpiForSystemFn>(GetProcAddress(user32, "GetDpiForSystem"));
        if (pfnSys) {
            UINT dpi = pfnSys();
            if (dpi != 0) return dpi;
        }
    }
    HDC screen = GetDC(nullptr);
    int dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);
    return dpi > 0 ? static_cast<UINT>(dpi) : 96;
}

inline int ScaleDpi(int value, UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

BOOL AdjustRectForDpi(LPRECT prc, DWORD style, BOOL menu, DWORD exStyle, UINT dpi) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using AdjustWindowRectExForDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
        auto pfn = reinterpret_cast<AdjustWindowRectExForDpiFn>(
            GetProcAddress(user32, "AdjustWindowRectExForDpi"));
        if (pfn) {
            return pfn(prc, style, menu, exStyle, dpi);
        }
    }
    return AdjustWindowRectEx(prc, style, menu, exStyle);
}

HWND AddLabel(HWND parent, HFONT font, const wchar_t* text, int x, int y, int w, int h, int id = 0) {
    HWND hwnd = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_RIGHT | SS_CENTERIMAGE,
                                x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return hwnd;
}

HWND AddSectionHeader(HWND parent, HFONT font, const wchar_t* text, int x, int y, int w, int h) {
    HWND hwnd = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                                x, y, w, h, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return hwnd;
}

HWND AddSeparator(HWND parent, int x, int y, int w) {
    return CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                           x, y, w, 2, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
}

HWND AddEdit(HWND parent, HFONT font, const wchar_t* text, int x, int y, int w, int h, DWORD extra, int id) {
    HWND hwnd = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | extra,
                                x, y, w, h, parent,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr),
                                nullptr);
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return hwnd;
}

void SetEdit(HWND edit, const std::wstring& text) {
    SetWindowTextW(edit, text.c_str());
}

std::wstring GetEdit(HWND edit) {
    if (!edit) return L"";
    wchar_t buf[512] = {};
    GetWindowTextW(edit, buf, static_cast<int>(std::size(buf)));
    return buf;
}

int SelectCombo(HWND combo, const std::wstring& value) {
    const int count = ComboBox_GetCount(combo);
    for (int i = 0; i < count; ++i) {
        wchar_t buf[64];
        ComboBox_GetLBText(combo, i, buf);
        if (value == buf) {
            ComboBox_SetCurSel(combo, i);
            return i;
        }
    }
    ComboBox_SetCurSel(combo, 0);
    return 0;
}

std::wstring GetCombo(HWND combo) {
    const int sel = ComboBox_GetCurSel(combo);
    if (sel == CB_ERR) return L"";
    wchar_t buf[64];
    ComboBox_GetLBText(combo, sel, buf);
    return buf;
}

void PopulateLanguages(HWND combo, TransformOperation op, bool isTarget) {
    ComboBox_ResetContent(combo);
    for (const auto& lang : GetSupportedLanguages()) {
        if (IsValidLanguageForOperation(lang.code, op, isTarget)) {
            ComboBox_AddString(combo, lang.displayName.data());
        }
    }
}

int SelectLanguageCombo(HWND combo, std::wstring_view code) {
    const std::wstring_view targetName = LanguageDisplayName(code);
    const int count = ComboBox_GetCount(combo);
    wchar_t buf[128] = {};
    for (int i = 0; i < count; ++i) {
        ComboBox_GetLBText(combo, i, buf);
        if (targetName == buf) {
            ComboBox_SetCurSel(combo, i);
            return i;
        }
    }
    ComboBox_SetCurSel(combo, 0);
    return 0;
}

std::wstring GetSelectedLanguageCode(HWND combo) {
    const int sel = ComboBox_GetCurSel(combo);
    if (sel == CB_ERR) return L"";
    wchar_t buf[128] = {};
    ComboBox_GetLBText(combo, sel, buf);
    return std::wstring(LanguageCodeFromDisplayName(buf));
}

void PopulateProfiles(HWND combo, TransformOperation op, TranslationProfile currentProfile) {
    ComboBox_ResetContent(combo);
    ComboBox_AddString(combo, ProfileDisplayName(TranslationProfile::Neutral, op).data());
    ComboBox_AddString(combo, ProfileDisplayName(TranslationProfile::Professional, op).data());
    ComboBox_AddString(combo, ProfileDisplayName(TranslationProfile::Casual, op).data());
    ComboBox_AddString(combo, ProfileDisplayName(TranslationProfile::Slang, op).data());
    ComboBox_AddString(combo, ProfileDisplayName(TranslationProfile::Custom, op).data());

    int profSel = 0;
    switch (currentProfile) {
    case TranslationProfile::Professional: profSel = 1; break;
    case TranslationProfile::Casual: profSel = 2; break;
    case TranslationProfile::Slang: profSel = 3; break;
    case TranslationProfile::Custom: profSel = 4; break;
    default: profSel = 0; break;
    }
    ComboBox_SetCurSel(combo, profSel);
}

void UpdateOperationControls(HWND hwnd, DialogData& data, TransformOperation op) {
    (void)hwnd;
    data.currentOperation = op;
    if (op == TransformOperation::CorrectAndRewrite) {
        if (data.lblSection1) SetWindowTextW(data.lblSection1, L"Correction & Rewriting");
        if (data.lblSource) SetWindowTextW(data.lblSource, L"Language");
        if (data.lblTarget) ShowWindow(data.lblTarget, SW_HIDE);
        if (data.comboTarget) {
            ShowWindow(data.comboTarget, SW_HIDE);
            EnableWindow(data.comboTarget, FALSE);
        }
        if (data.lblProfile) SetWindowTextW(data.lblProfile, L"Writing Profile");

        std::wstring curSrc = GetSelectedLanguageCode(data.comboSource);
        if (curSrc.empty() || curSrc == L"auto" || !IsValidLanguageForOperation(curSrc, op, false)) {
            curSrc = L"es";
        }
        PopulateLanguages(data.comboSource, op, false);
        SelectLanguageCombo(data.comboSource, curSrc);

        const int profSel = ComboBox_GetCurSel(data.comboProfile);
        TranslationProfile prof = TranslationProfile::Neutral;
        if (profSel == 1) prof = TranslationProfile::Professional;
        else if (profSel == 2) prof = TranslationProfile::Casual;
        else if (profSel == 3) prof = TranslationProfile::Slang;
        else if (profSel == 4) prof = TranslationProfile::Custom;
        PopulateProfiles(data.comboProfile, op, prof);
    } else {
        if (data.lblSection1) SetWindowTextW(data.lblSection1, L"Translation");
        if (data.lblSource) SetWindowTextW(data.lblSource, L"Source Language");
        if (data.lblTarget) ShowWindow(data.lblTarget, SW_SHOW);
        if (data.comboTarget) {
            ShowWindow(data.comboTarget, SW_SHOW);
            EnableWindow(data.comboTarget, TRUE);
        }
        if (data.lblProfile) SetWindowTextW(data.lblProfile, L"Translation Profile");

        std::wstring curSrc = GetSelectedLanguageCode(data.comboSource);
        PopulateLanguages(data.comboSource, op, false);
        SelectLanguageCombo(data.comboSource, curSrc.empty() ? L"auto" : curSrc);

        std::wstring curTgt = GetSelectedLanguageCode(data.comboTarget);
        PopulateLanguages(data.comboTarget, op, true);
        SelectLanguageCombo(data.comboTarget, curTgt.empty() ? L"en" : curTgt);

        const int profSel = ComboBox_GetCurSel(data.comboProfile);
        TranslationProfile prof = TranslationProfile::Neutral;
        if (profSel == 1) prof = TranslationProfile::Professional;
        else if (profSel == 2) prof = TranslationProfile::Casual;
        else if (profSel == 3) prof = TranslationProfile::Slang;
        else if (profSel == 4) prof = TranslationProfile::Custom;
        PopulateProfiles(data.comboProfile, op, prof);
    }
}

const wchar_t* CredentialTargetForProvider(ProviderId id) {
    switch (id) {
    case ProviderId::Gemini: return CredentialStore::kGeminiTarget;
    case ProviderId::OpenAI: return CredentialStore::kOpenAITarget;
    case ProviderId::Mock: return nullptr;
    }
    return nullptr;
}

std::wstring KeyStatusText(ProviderId provider) {
    const wchar_t* target = CredentialTargetForProvider(provider);
    if (!target) {
        return L"Mock provider does not require an API key.";
    }
    if (CredentialStore::HasApiKey(target)) {
        return L"\u2713 API key is configured securely in Windows Credential Manager.";
    }
    return L"No API key configured for this provider. Enter a key above and click 'Save Key'.";
}

std::wstring EffectiveTestKey(HWND hwnd, ProviderId provider) {
    const std::wstring typed = GetEdit(GetDlgItem(hwnd, kApiKey));
    if (!typed.empty()) {
        return typed;
    }
    const wchar_t* target = CredentialTargetForProvider(provider);
    if (target) {
        std::wstring key;
        if (CredentialStore::GetApiKey(target, key)) {
            return key;
        }
    }
    return L"";
}

void UpdateProviderControls(HWND hwnd, ProviderId provider, const std::wstring& modelOverride,
                            const std::wstring& endpointOverride) {
    HWND modelCombo = GetDlgItem(hwnd, kModel);
    HWND endpointEdit = GetDlgItem(hwnd, kEndpoint);
    HWND keyEdit = GetDlgItem(hwnd, kApiKey);
    HWND btnSave = GetDlgItem(hwnd, kSaveKey);
    HWND btnRemove = GetDlgItem(hwnd, kRemoveKey);
    HWND btnTest = GetDlgItem(hwnd, kTestKey);
    HWND keyStatus = GetDlgItem(hwnd, kKeyStatus);

    ComboBox_ResetContent(modelCombo);

    if (provider == ProviderId::Mock) {
        SetWindowTextW(modelCombo, L"(not applicable)");
        SetEdit(endpointEdit, L"");
        EnableWindow(modelCombo, FALSE);
        EnableWindow(endpointEdit, FALSE);
        EnableWindow(keyEdit, FALSE);
        EnableWindow(btnSave, FALSE);
        EnableWindow(btnRemove, FALSE);
        EnableWindow(btnTest, FALSE);
        SetWindowTextW(keyStatus, L"Mock provider runs locally without an API key.");
        return;
    }

    EnableWindow(modelCombo, TRUE);
    EnableWindow(endpointEdit, TRUE);
    EnableWindow(keyEdit, TRUE);
    EnableWindow(btnSave, TRUE);
    EnableWindow(btnRemove, TRUE);
    EnableWindow(btnTest, TRUE);

    const auto models = KnownModelsForProvider(provider);
    for (const auto& m : models) {
        ComboBox_AddString(modelCombo, m.id.c_str());
    }

    std::wstring effectiveModel = modelOverride.empty() ? DefaultModelForProvider(provider) : modelOverride;
    SetWindowTextW(modelCombo, effectiveModel.c_str());

    std::wstring effectiveEndpoint = endpointOverride.empty() ? DefaultEndpointForProvider(provider) : endpointOverride;
    SetEdit(endpointEdit, effectiveEndpoint);

    SetWindowTextW(keyStatus, KeyStatusText(provider).c_str());
}

bool IsSpeculativeSupported(ProviderId provider, const std::wstring& model) {
    if (provider != ProviderId::OpenAI) return false;
    return (model.empty() || model == L"gpt-5.6-luna");
}

void UpdateCapabilityControls(HWND hwnd, ProviderId provider, const std::wstring& model) {
    const ModelCapabilities caps = GetModelCapabilities(provider, model);
    HWND tempEdit = GetDlgItem(hwnd, kTemperature);
    HWND topPEdit = GetDlgItem(hwnd, kTopP);
    HWND topKEdit = GetDlgItem(hwnd, kTopK);
    HWND maxTokensEdit = GetDlgItem(hwnd, kMaxTokens);

    EnableWindow(tempEdit, caps.SupportsTemperature() ? TRUE : FALSE);
    EnableWindow(topPEdit, caps.SupportsTopP() ? TRUE : FALSE);
    EnableWindow(topKEdit, caps.SupportsTopK() ? TRUE : FALSE);
    EnableWindow(maxTokensEdit, caps.SupportsMaxOutputTokens() ? TRUE : FALSE);

    HWND specChk = GetDlgItem(hwnd, kSpeculativeTranslation);
    HWND specNote = GetDlgItem(hwnd, kSpeculativeNote);
    const bool specSupported = IsSpeculativeSupported(provider, model);
    if (specChk) {
        EnableWindow(specChk, specSupported ? TRUE : FALSE);
    }
    if (specNote) {
        if (specSupported) {
            SetWindowTextW(specNote, L"Prepare translations in the background during longer pauses for faster final results.");
        } else {
            SetWindowTextW(specNote, L"Predictive translation is currently only available for OpenAI (gpt-5.6-luna).");
        }
    }
}

void CollectValues(HWND hwnd, DialogData& data) {
    data.config.enabled = IsDlgButtonChecked(hwnd, kEnabled) == BST_CHECKED;

    const int opSel = ComboBox_GetCurSel(GetDlgItem(hwnd, kOperation));
    data.config.operation = (opSel == 1) ? TransformOperation::CorrectAndRewrite : TransformOperation::Translate;

    const int sel = ComboBox_GetCurSel(GetDlgItem(hwnd, kProvider));
    if (sel == 1) {
        data.config.provider = ProviderId::Gemini;
    } else if (sel == 2) {
        data.config.provider = ProviderId::OpenAI;
    } else {
        data.config.provider = ProviderId::Mock;
    }

    data.config.sourceLanguage = GetSelectedLanguageCode(GetDlgItem(hwnd, kSourceLang));
    if (data.config.operation == TransformOperation::CorrectAndRewrite) {
        data.config.targetLanguage = data.config.sourceLanguage;
    } else {
        data.config.targetLanguage = GetSelectedLanguageCode(GetDlgItem(hwnd, kTargetLang));
    }

    const int profIdx = ComboBox_GetCurSel(GetDlgItem(hwnd, kProfile));
    switch (profIdx) {
    case 1: data.config.profile = TranslationProfile::Professional; break;
    case 2: data.config.profile = TranslationProfile::Casual; break;
    case 3: data.config.profile = TranslationProfile::Slang; break;
    case 4: data.config.profile = TranslationProfile::Custom; break;
    default: data.config.profile = TranslationProfile::Neutral; break;
    }
    data.config.customProfileInstructions = GetEdit(GetDlgItem(hwnd, kCustomInstructions));
    data.config.smartPasteEnabled = (IsDlgButtonChecked(hwnd, kSmartPaste) == BST_CHECKED);

    data.config.endpoint = GetEdit(GetDlgItem(hwnd, kEndpoint));
    data.config.model = GetEdit(GetDlgItem(hwnd, kModel));

    data.config.debounceMs = _wtoi(GetEdit(GetDlgItem(hwnd, kDebounce)).c_str());
    data.config.minChars = _wtoi(GetEdit(GetDlgItem(hwnd, kMinChars)).c_str());
    data.config.maxBufferChars = _wtoi(GetEdit(GetDlgItem(hwnd, kMaxBuffer)).c_str());

    // Predictive Translation
    const bool specChecked = (IsDlgButtonChecked(hwnd, kSpeculativeTranslation) == BST_CHECKED);
    const bool specSupported = IsSpeculativeSupported(data.config.provider, data.config.model);
    data.config.speculativeTranslationEnabled = (specChecked && specSupported);

    // Advanced Model Parameters
    const std::wstring tempStr = GetEdit(GetDlgItem(hwnd, kTemperature));
    if (!tempStr.empty()) {
        try { data.config.modelParams.temperature = std::stod(tempStr); } catch (...) {}
    } else {
        data.config.modelParams.temperature.reset();
    }

    const std::wstring topPStr = GetEdit(GetDlgItem(hwnd, kTopP));
    if (!topPStr.empty()) {
        try { data.config.modelParams.topP = std::stod(topPStr); } catch (...) {}
    } else {
        data.config.modelParams.topP.reset();
    }

    const std::wstring topKStr = GetEdit(GetDlgItem(hwnd, kTopK));
    if (!topKStr.empty()) {
        data.config.modelParams.topK = _wtoi(topKStr.c_str());
    } else {
        data.config.modelParams.topK.reset();
    }

    const std::wstring maxTokStr = GetEdit(GetDlgItem(hwnd, kMaxTokens));
    if (!maxTokStr.empty()) {
        data.config.modelParams.maxOutputTokens = _wtoi(maxTokStr.c_str());
    } else {
        data.config.modelParams.maxOutputTokens.reset();
    }

    // Sanitize collected model parameters against provider and model capabilities
    data.config.modelParams = SanitizeModelParameters(data.config.provider, data.config.model, data.config.modelParams);

    // API key handling: an explicitly typed key is stored securely on OK for the active provider.
    const std::wstring typed = GetEdit(GetDlgItem(hwnd, kApiKey));
    const wchar_t* target = CredentialTargetForProvider(data.config.provider);
    if (!typed.empty() && target) {
        (void)CredentialStore::SetApiKey(target, typed);
    }
}

void RunTestConnection(HWND hwnd, ProviderId provider, const std::wstring& endpoint,
                       const std::wstring& model, const std::wstring& key) {
    auto* result = new TestResult();
    std::thread([hwnd, provider, endpoint, model, key, result] {
        result->message.clear();
        if (provider == ProviderId::Gemini) {
            result->ok = GeminiProvider::TestConnection(endpoint, model, key, 45000, result->message);
        } else if (provider == ProviderId::OpenAI) {
            result->ok = OpenAIProvider::TestConnection(endpoint, model, key, 45000, result->message);
        } else {
            result->ok = true;
            result->message = L"Mock provider does not require network connection.";
        }
        PostMessageW(hwnd, kTestResultMsg, 0, reinterpret_cast<LPARAM>(result));
    }).detach();
}

void ToggleExpansion(HWND hwnd, DialogData& data) {
    data.expanded = !data.expanded;
    HWND toggleBtn = GetDlgItem(hwnd, kAdvancedToggle);
    SetWindowTextW(toggleBtn, data.expanded ? L"Advanced Settings \u25b4" : L"Advanced Settings \u25be");

    for (HWND hCtrl : data.advancedControls) {
        ShowWindow(hCtrl, data.expanded ? SW_SHOW : SW_HIDE);
    }

    const int clientH = ScaleDpi(data.expanded ? kExpandedClientH : kBaseClientH, data.dpi);
    const int clientW = ScaleDpi(kClientW, data.dpi);

    RECT rc{0, 0, clientW, clientH};
    const DWORD style = WS_CAPTION | WS_SYSMENU | WS_VISIBLE;
    const DWORD exStyle = WS_EX_DLGMODALFRAME | WS_EX_APPWINDOW;
    AdjustRectForDpi(&rc, style, FALSE, exStyle, data.dpi);
    const int winW = rc.right - rc.left;
    const int winH = rc.bottom - rc.top;

    SetWindowPos(hwnd, nullptr, 0, 0, winW, winH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    // Relocate action buttons to the bottom of current client height
    const int marginX = ScaleDpi(kMarginX, data.dpi);
    const int fullW = ScaleDpi(kFullW, data.dpi);
    const int actionW = ScaleDpi(100, data.dpi);
    const int actionH = ScaleDpi(28, data.dpi);
    const int actionY = clientH - actionH - ScaleDpi(14, data.dpi);
    const int cancelX = marginX + fullW - actionW;
    const int okX = cancelX - actionW - ScaleDpi(10, data.dpi);

    SetWindowPos(GetDlgItem(hwnd, kOk), nullptr, okX, actionY, actionW, actionH, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hwnd, kCancel), nullptr, cancelX, actionY, actionW, actionH, SWP_NOZORDER);
}

} // namespace

bool ShowSettingsDialog(HWND parent, AppConfig& config) {
    struct Local {
        static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
            DialogData* data = reinterpret_cast<DialogData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            switch (msg) {
            case WM_NCCREATE: {
                const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                                  reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
                return TRUE;
            }
            case WM_CTLCOLORSTATIC: {
                HDC hdc = reinterpret_cast<HDC>(wp);
                SetBkMode(hdc, TRANSPARENT);
                return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_BTNFACE));
            }
            case kTestResultMsg: {
                const auto* result = reinterpret_cast<TestResult*>(lp);
                if (data && result) {
                    EnableWindow(GetDlgItem(hwnd, kTestKey), TRUE);
                    SetWindowTextW(GetDlgItem(hwnd, kKeyStatus),
                                   result->ok ? L"\u2713 Connection successful! Endpoint and credentials verified."
                                              : (L"Connection failed:\n" + result->message).c_str());
                }
                delete result;
                return 0;
            }
            case WM_COMMAND:
                if (data) {
                    if (HIWORD(wp) == CBN_SELCHANGE && LOWORD(wp) == kOperation) {
                        if (data->updatingControls) return 0;
                        data->updatingControls = true;
                        const int sel = ComboBox_GetCurSel(GetDlgItem(hwnd, kOperation));
                        TransformOperation newOp = (sel == 1) ? TransformOperation::CorrectAndRewrite : TransformOperation::Translate;
                        UpdateOperationControls(hwnd, *data, newOp);
                        data->updatingControls = false;
                        return 0;
                    }
                    if (HIWORD(wp) == CBN_SELCHANGE && LOWORD(wp) == kProvider) {
                        if (data->updatingControls) {
                            return 0;
                        }
                        data->updatingControls = true;
                        const int sel = ComboBox_GetCurSel(GetDlgItem(hwnd, kProvider));
                        ProviderId newProv = ProviderId::Mock;
                        if (sel == 1) newProv = ProviderId::Gemini;
                        else if (sel == 2) newProv = ProviderId::OpenAI;

                        data->currentProvider = newProv;
                        UpdateProviderControls(hwnd, newProv, L"", L"");
                        UpdateCapabilityControls(hwnd, newProv, GetEdit(GetDlgItem(hwnd, kModel)));
                        data->updatingControls = false;
                        return 0;
                    }
                    if (HIWORD(wp) == CBN_SELCHANGE && LOWORD(wp) == kProfile) {
                        const int profSel = ComboBox_GetCurSel(GetDlgItem(hwnd, kProfile));
                        HWND customEdit = GetDlgItem(hwnd, kCustomInstructions);
                        EnableWindow(customEdit, profSel == 4 ? TRUE : FALSE);
                        return 0;
                    }
                    if ((HIWORD(wp) == CBN_SELCHANGE || HIWORD(wp) == CBN_EDITCHANGE) && LOWORD(wp) == kModel) {
                        if (!data->updatingControls) {
                            UpdateCapabilityControls(hwnd, data->currentProvider, GetEdit(GetDlgItem(hwnd, kModel)));
                        }
                        return 0;
                    }
                    if (HIWORD(wp) == BN_CLICKED) {
                        switch (LOWORD(wp)) {
                        case kOk:
                            CollectValues(hwnd, *data);
                            data->accepted = true;
                            DestroyWindow(hwnd);
                            return 0;
                        case kCancel:
                            DestroyWindow(hwnd);
                            return 0;
                        case kAdvancedToggle:
                            ToggleExpansion(hwnd, *data);
                            return 0;
                        case kSaveKey: {
                            const std::wstring key = GetEdit(GetDlgItem(hwnd, kApiKey));
                            const wchar_t* target = CredentialTargetForProvider(data->currentProvider);
                            const bool ok = target && !key.empty() && CredentialStore::SetApiKey(target, key);
                            SetWindowTextW(GetDlgItem(hwnd, kKeyStatus),
                                           ok ? L"\u2713 Key stored securely in Windows Credential Manager."
                                              : (key.empty() ? L"Please enter an API key above first."
                                                             : L"Failed to store key in Credential Manager."));
                            SetWindowTextW(GetDlgItem(hwnd, kApiKey), L"");
                            return 0;
                        }
                        case kRemoveKey: {
                            const wchar_t* target = CredentialTargetForProvider(data->currentProvider);
                            if (target) {
                                (void)CredentialStore::RemoveApiKey(target);
                            }
                            SetWindowTextW(GetDlgItem(hwnd, kKeyStatus), KeyStatusText(data->currentProvider).c_str());
                            SetWindowTextW(GetDlgItem(hwnd, kApiKey), L"");
                            return 0;
                        }
                        case kTestKey: {
                            EnableWindow(GetDlgItem(hwnd, kTestKey), FALSE);
                            SetWindowTextW(GetDlgItem(hwnd, kKeyStatus), L"Testing connection\u2026 please wait.");
                            RunTestConnection(hwnd, data->currentProvider,
                                              GetEdit(GetDlgItem(hwnd, kEndpoint)),
                                              GetEdit(GetDlgItem(hwnd, kModel)),
                                              EffectiveTestKey(hwnd, data->currentProvider));
                            return 0;
                        }
                        default:
                            break;
                        }
                    }
                }
                return 0;
            case WM_CLOSE:
                DestroyWindow(hwnd);
                return 0;
            case WM_DESTROY:
                PostThreadMessageW(GetCurrentThreadId(), WM_NULL, 0, 0);
                return 0;
            default:
                return DefWindowProcW(hwnd, msg, wp, lp);
            }
        }
    };

    static bool registered = [] {
        WNDCLASS wc{};
        wc.lpfnWndProc = &Local::Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        return RegisterClassW(&wc) != 0;
    }();
    (void)registered;

    UINT dpi = GetWindowDpi(parent);

    const int clientW = ScaleDpi(kClientW, dpi);
    const int clientH = ScaleDpi(kBaseClientH, dpi);

    RECT rc{0, 0, clientW, clientH};
    const DWORD style = WS_CAPTION | WS_SYSMENU | WS_VISIBLE;
    const DWORD exStyle = WS_EX_DLGMODALFRAME | WS_EX_APPWINDOW;
    AdjustRectForDpi(&rc, style, FALSE, exStyle, dpi);
    const int winW = rc.right - rc.left;
    const int winH = rc.bottom - rc.top;

    RECT workArea{};
    HMONITOR hMon = MonitorFromWindow(parent ? parent : GetDesktopWindow(), MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(MONITORINFO)};
    if (hMon && GetMonitorInfoW(hMon, &mi)) {
        workArea = mi.rcWork;
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    }

    const int screenW = workArea.right - workArea.left;
    const int screenH = workArea.bottom - workArea.top;
    int x = workArea.left + std::max(0, (screenW - winW) / 2);
    int y = workArea.top + std::max(0, (screenH - winH) / 2) - ScaleDpi(kOpenUpwardBiasDip, dpi);

    // Clamp to monitor work area so dialog never extends outside usable desktop
    x = std::max(static_cast<int>(workArea.left), std::min(x, static_cast<int>(workArea.right - winW)));
    y = std::max(static_cast<int>(workArea.top), std::min(y, static_cast<int>(workArea.bottom - winH)));

    DialogData data;
    data.config = config;
    data.currentProvider = config.provider;
    data.currentOperation = config.operation;
    data.dpi = dpi;

    HWND hwnd = CreateWindowExW(exStyle, kClass,
                                L"AI Input Engine \u2014 Settings", style,
                                x, y, winW, winH, parent, nullptr,
                                GetModuleHandleW(nullptr), &data);
    if (!hwnd) {
        return false;
    }
    data.hwnd = hwnd;

    // Set custom application icon on the settings window titlebar
    HICON hAppIcon = reinterpret_cast<HICON>(LoadImageW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
        ScaleDpi(32, dpi), ScaleDpi(32, dpi), LR_DEFAULTCOLOR));
    HICON hSmallIcon = reinterpret_cast<HICON>(LoadImageW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
        ScaleDpi(16, dpi), ScaleDpi(16, dpi), LR_DEFAULTCOLOR));
    if (hAppIcon) SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hAppIcon));
    if (hSmallIcon) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hSmallIcon));

    if (parent) {
        EnableWindow(parent, FALSE);
    }
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&data));

    // Fonts: Segoe UI Regular, Bold, and Title
    HFONT font = CreateFontW(-ScaleDpi(12, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT fontBold = CreateFontW(-ScaleDpi(12, dpi), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT fontTitle = CreateFontW(-ScaleDpi(15, dpi), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (!font) font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    if (!fontBold) fontBold = font;
    if (!fontTitle) fontTitle = font;
    data.font = font;
    data.fontBold = fontBold;
    data.fontTitle = fontTitle;

    const int marginX = ScaleDpi(kMarginX, dpi);
    const int labelW = ScaleDpi(kLabelW, dpi);
    const int gap = ScaleDpi(kGap, dpi);
    const int controlX = marginX + labelW + gap;
    const int controlW = ScaleDpi(kControlW, dpi);
    const int controlH = ScaleDpi(kControlH, dpi);
    const int rowAdv = ScaleDpi(kRowAdvance, dpi);
    const int fullW = labelW + gap + controlW;

    // --- Header Section ---
    int row = ScaleDpi(14, dpi);
    HWND titleLabel = CreateWindowExW(0, L"STATIC", L"AI Input Engine", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                      marginX, row, fullW - ScaleDpi(90, dpi), ScaleDpi(20, dpi), hwnd,
                                      nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(titleLabel, WM_SETFONT, reinterpret_cast<WPARAM>(fontTitle), TRUE);

    // Enabled checkbox on top right
    HWND chk = CreateWindowExW(0, L"BUTTON", L"Enabled", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                               BS_AUTOCHECKBOX, marginX + fullW - ScaleDpi(80, dpi), row, ScaleDpi(80, dpi), controlH, hwnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEnabled)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(chk, WM_SETFONT, reinterpret_cast<WPARAM>(fontBold), TRUE);
    CheckDlgButton(hwnd, kEnabled, config.enabled ? BST_CHECKED : BST_UNCHECKED);

    row += ScaleDpi(20, dpi);
    HWND subtitleLabel = CreateWindowExW(0, L"STATIC", L"Live System-Wide AI Text Transformation", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                         marginX, row, fullW, ScaleDpi(16, dpi), hwnd,
                                         nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(subtitleLabel, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

    row += ScaleDpi(22, dpi);
    AddSeparator(hwnd, marginX, row, fullW);
    row += ScaleDpi(8, dpi);

    // --- Section 1: Operation & Transformation ---
    data.lblSection1 = AddSectionHeader(hwnd, fontBold,
                                        config.operation == TransformOperation::CorrectAndRewrite
                                            ? L"Correction & Rewriting"
                                            : L"Translation",
                                        marginX, row, fullW, ScaleDpi(18, dpi));
    row += ScaleDpi(22, dpi);

    // Operation
    AddLabel(hwnd, font, L"Operation", marginX, row, labelW, controlH);
    HWND opCombo = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                   CBS_DROPDOWNLIST, controlX, row, controlW, ScaleDpi(200, dpi), hwnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOperation)), GetModuleHandleW(nullptr),
                                   nullptr);
    SendMessageW(opCombo, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    ComboBox_AddString(opCombo, L"Translate");
    ComboBox_AddString(opCombo, L"Correct & Rewrite");
    ComboBox_SetCurSel(opCombo, config.operation == TransformOperation::CorrectAndRewrite ? 1 : 0);
    row += rowAdv;

    // Source Language / Language
    data.lblSource = AddLabel(hwnd, font,
                              config.operation == TransformOperation::CorrectAndRewrite ? L"Language" : L"Source Language",
                              marginX, row, labelW, controlH);
    data.comboSource = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                       CBS_DROPDOWNLIST, controlX, row, controlW, ScaleDpi(200, dpi), hwnd,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSourceLang)), GetModuleHandleW(nullptr),
                                       nullptr);
    SendMessageW(data.comboSource, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    PopulateLanguages(data.comboSource, config.operation, false);
    SelectLanguageCombo(data.comboSource, config.sourceLanguage);
    row += rowAdv;

    // Target Language
    data.lblTarget = AddLabel(hwnd, font, L"Target Language", marginX, row, labelW, controlH);
    data.comboTarget = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                       CBS_DROPDOWNLIST, controlX, row, controlW, ScaleDpi(200, dpi), hwnd,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTargetLang)), GetModuleHandleW(nullptr),
                                       nullptr);
    SendMessageW(data.comboTarget, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    PopulateLanguages(data.comboTarget, config.operation, true);
    SelectLanguageCombo(data.comboTarget, config.targetLanguage);
    if (config.operation == TransformOperation::CorrectAndRewrite) {
        ShowWindow(data.lblTarget, SW_HIDE);
        ShowWindow(data.comboTarget, SW_HIDE);
        EnableWindow(data.comboTarget, FALSE);
    }
    row += rowAdv;

    // Profile
    data.lblProfile = AddLabel(hwnd, font,
                               config.operation == TransformOperation::CorrectAndRewrite ? L"Writing Profile" : L"Translation Profile",
                               marginX, row, labelW, controlH);
    data.comboProfile = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                        CBS_DROPDOWNLIST, controlX, row, controlW, ScaleDpi(200, dpi), hwnd,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kProfile)), GetModuleHandleW(nullptr),
                                        nullptr);
    SendMessageW(data.comboProfile, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    PopulateProfiles(data.comboProfile, config.operation, config.profile);
    row += rowAdv;

    // Custom Profile Instructions (multiline edit)
    AddLabel(hwnd, font, L"Custom Prompt", marginX, row, labelW, controlH);
    HWND customEdit = AddEdit(hwnd, font, config.customProfileInstructions.c_str(),
                              controlX, row, controlW, ScaleDpi(40, dpi),
                              ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL, kCustomInstructions);
    EnableWindow(customEdit, config.profile == TranslationProfile::Custom ? TRUE : FALSE);
    row += ScaleDpi(44, dpi);

    // Smart Paste Checkbox
    AddLabel(hwnd, font, L"Smart Paste", marginX, row, labelW, controlH);
    HWND chkSmartPaste = CreateWindowExW(0, L"BUTTON", L"Enable Smart Paste (Ctrl+Shift+V)", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                         BS_AUTOCHECKBOX, controlX, row, controlW, controlH, hwnd,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSmartPaste)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(chkSmartPaste, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    CheckDlgButton(hwnd, kSmartPaste, config.smartPasteEnabled ? BST_CHECKED : BST_UNCHECKED);
    row += rowAdv;

    AddSeparator(hwnd, marginX, row, fullW);
    row += ScaleDpi(8, dpi);

    // --- Section 2: AI Provider & Credentials ---
    AddSectionHeader(hwnd, fontBold, L"AI Provider & Credentials", marginX, row, fullW, ScaleDpi(18, dpi));
    row += ScaleDpi(22, dpi);

    // Provider
    AddLabel(hwnd, font, L"Provider", marginX, row, labelW, controlH);
    HWND provider = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                    CBS_DROPDOWNLIST, controlX, row, controlW, ScaleDpi(200, dpi), hwnd,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kProvider)), GetModuleHandleW(nullptr),
                                    nullptr);
    SendMessageW(provider, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    ComboBox_AddString(provider, L"Mock (offline demo)");
    ComboBox_AddString(provider, L"Google Gemini");
    ComboBox_AddString(provider, L"OpenAI");
    int provIdx = 0;
    if (config.provider == ProviderId::Gemini) provIdx = 1;
    else if (config.provider == ProviderId::OpenAI) provIdx = 2;
    ComboBox_SetCurSel(provider, provIdx);
    row += rowAdv;

    // Model
    AddLabel(hwnd, font, L"Model", marginX, row, labelW, controlH);
    HWND modelCombo = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                      CBS_DROPDOWN | CBS_AUTOHSCROLL, controlX, row, controlW, ScaleDpi(200, dpi), hwnd,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kModel)), GetModuleHandleW(nullptr),
                                      nullptr);
    SendMessageW(modelCombo, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    row += rowAdv;

    // Endpoint URL
    AddLabel(hwnd, font, L"Endpoint URL", marginX, row, labelW, controlH);
    AddEdit(hwnd, font, L"", controlX, row, controlW, controlH, 0, kEndpoint);
    row += rowAdv;

    // API Key
    AddLabel(hwnd, font, L"API Key", marginX, row, labelW, controlH);
    AddEdit(hwnd, font, L"", controlX, row, controlW, controlH, ES_PASSWORD, kApiKey);
    row += rowAdv;

    // Key Action Buttons
    const int btnH = ScaleDpi(26, dpi);
    const int btnSaveW = ScaleDpi(88, dpi);
    const int btnRemoveW = ScaleDpi(100, dpi);
    const int btnGap = ScaleDpi(8, dpi);
    const int btnTestW = controlW - (btnSaveW + btnGap + btnRemoveW + btnGap);
    HWND btnSave = CreateWindowExW(0, L"BUTTON", L"Save Key", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                   controlX, row, btnSaveW, btnH, hwnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSaveKey)), GetModuleHandleW(nullptr),
                                   nullptr);
    HWND btnRemove = CreateWindowExW(0, L"BUTTON", L"Remove Key", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                     controlX + btnSaveW + btnGap, row, btnRemoveW, btnH, hwnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRemoveKey)), GetModuleHandleW(nullptr),
                                     nullptr);
    HWND btnTest = CreateWindowExW(0, L"BUTTON", L"Test Connection", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                   controlX + btnSaveW + btnGap + btnRemoveW + btnGap, row, btnTestW, btnH, hwnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTestKey)), GetModuleHandleW(nullptr),
                                   nullptr);
    SendMessageW(btnSave, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessageW(btnRemove, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessageW(btnTest, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    row += btnH + ScaleDpi(6, dpi);

    // Multiline Key Status Label (48px height with SS_NOPREFIX for clean multiline rendering)
    HWND keyStatus = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                                     controlX, row, controlW, ScaleDpi(48, dpi), hwnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kKeyStatus)), GetModuleHandleW(nullptr),
                                     nullptr);
    SendMessageW(keyStatus, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    row += ScaleDpi(52, dpi);

    UpdateProviderControls(hwnd, config.provider, config.model, config.endpoint);

    // --- Section 3: Advanced (Collapsible, Two-Column Layout) ---
    HWND toggleBtn = CreateWindowExW(0, L"BUTTON", L"Advanced Settings \u25be", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                     marginX, row, ScaleDpi(160, dpi), ScaleDpi(26, dpi), hwnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAdvancedToggle)), GetModuleHandleW(nullptr),
                                     nullptr);
    SendMessageW(toggleBtn, WM_SETFONT, reinterpret_cast<WPARAM>(fontBold), TRUE);
    row += ScaleDpi(30, dpi);

    const int advY = row;
    const int col1LabelX = marginX;
    const int col1LabelW = ScaleDpi(kAdvLabelW, dpi);
    const int col1EditX = col1LabelX + col1LabelW + ScaleDpi(kAdvGap, dpi);
    const int col1EditW = ScaleDpi(kAdvNumEditW, dpi);

    const int col2LabelX = marginX + ScaleDpi(kAdvColW + kAdvColGap, dpi);
    const int col2LabelW = ScaleDpi(kAdvLabelW, dpi);
    const int col2EditX = col2LabelX + col2LabelW + ScaleDpi(kAdvGap, dpi);
    const int col2EditW = ScaleDpi(kAdvNumEditW, dpi);

    const int advRowAdv = ScaleDpi(kAdvRowAdvance, dpi);

    // Row 1: Debounce (Col 1) & Min Chars (Col 2)
    const int r1Y = advY;
    HWND hLblDeb = AddLabel(hwnd, font, L"Debounce (ms)", col1LabelX, r1Y, col1LabelW, controlH);
    HWND hEditDeb = AddEdit(hwnd, font, std::to_wstring(config.debounceMs).c_str(), col1EditX, r1Y, col1EditW, controlH, 0, kDebounce);
    HWND hLblMin = AddLabel(hwnd, font, L"Min Characters", col2LabelX, r1Y, col2LabelW, controlH);
    HWND hEditMin = AddEdit(hwnd, font, std::to_wstring(config.minChars).c_str(), col2EditX, r1Y, col2EditW, controlH, 0, kMinChars);
    data.advancedControls.push_back(hLblDeb);
    data.advancedControls.push_back(hEditDeb);
    data.advancedControls.push_back(hLblMin);
    data.advancedControls.push_back(hEditMin);

    // Row 2: Max Buffer (Col 1) & Max Output Tokens (Col 2)
    const int r2Y = advY + advRowAdv;
    HWND hLblMax = AddLabel(hwnd, font, L"Max Buffer Chars", col1LabelX, r2Y, col1LabelW, controlH);
    HWND hEditMax = AddEdit(hwnd, font, std::to_wstring(config.maxBufferChars).c_str(), col1EditX, r2Y, col1EditW, controlH, 0, kMaxBuffer);
    std::wstring maxTokensVal = config.modelParams.maxOutputTokens.has_value() ? std::to_wstring(*config.modelParams.maxOutputTokens) : L"";
    HWND hLblMaxTok = AddLabel(hwnd, font, L"Max Output Tokens", col2LabelX, r2Y, col2LabelW, controlH);
    HWND hEditMaxTok = AddEdit(hwnd, font, maxTokensVal.c_str(), col2EditX, r2Y, col2EditW, controlH, 0, kMaxTokens);
    data.advancedControls.push_back(hLblMax);
    data.advancedControls.push_back(hEditMax);
    data.advancedControls.push_back(hLblMaxTok);
    data.advancedControls.push_back(hEditMaxTok);

    // Row 3: Temperature (Col 1) & Top P (Col 2)
    const int r3Y = advY + advRowAdv * 2;
    std::wstring tempVal = config.modelParams.temperature.has_value() ? std::to_wstring(*config.modelParams.temperature) : L"";
    HWND hLblTemp = AddLabel(hwnd, font, L"Temperature (0.0\u20132.0)", col1LabelX, r3Y, col1LabelW, controlH);
    HWND hEditTemp = AddEdit(hwnd, font, tempVal.c_str(), col1EditX, r3Y, col1EditW, controlH, 0, kTemperature);
    std::wstring topPVal = config.modelParams.topP.has_value() ? std::to_wstring(*config.modelParams.topP) : L"";
    HWND hLblTopP = AddLabel(hwnd, font, L"Top P (0.0\u20131.0)", col2LabelX, r3Y, col2LabelW, controlH);
    HWND hEditTopP = AddEdit(hwnd, font, topPVal.c_str(), col2EditX, r3Y, col2EditW, controlH, 0, kTopP);
    data.advancedControls.push_back(hLblTemp);
    data.advancedControls.push_back(hEditTemp);
    data.advancedControls.push_back(hLblTopP);
    data.advancedControls.push_back(hEditTopP);

    // Row 4: Top K (Col 1)
    const int r4Y = advY + advRowAdv * 3;
    std::wstring topKVal = config.modelParams.topK.has_value() ? std::to_wstring(*config.modelParams.topK) : L"";
    HWND hLblTopK = AddLabel(hwnd, font, L"Top K (1\u2013100)", col1LabelX, r4Y, col1LabelW, controlH);
    HWND hEditTopK = AddEdit(hwnd, font, topKVal.c_str(), col1EditX, r4Y, col1EditW, controlH, 0, kTopK);
    data.advancedControls.push_back(hLblTopK);
    data.advancedControls.push_back(hEditTopK);

    // Row 5: Predictive Translation Checkbox & Explanatory Subtitle
    const int r5Y = advY + advRowAdv * 4;
    HWND hChkSpec = CreateWindowExW(0, L"BUTTON", L"Predictive Translation",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                    col1LabelX, r5Y, fullW, controlH, hwnd,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSpeculativeTranslation)),
                                    GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hChkSpec, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    CheckDlgButton(hwnd, kSpeculativeTranslation,
                   (config.speculativeTranslationEnabled && IsSpeculativeSupported(config.provider, config.model))
                       ? BST_CHECKED : BST_UNCHECKED);
    data.advancedControls.push_back(hChkSpec);

    HWND hLblSpecNote = CreateWindowExW(0, L"STATIC",
                                        L"Prepare translations in the background during longer pauses for faster final results.",
                                        WS_CHILD | WS_VISIBLE | SS_LEFT,
                                        col1LabelX + ScaleDpi(20, dpi), r5Y + ScaleDpi(22, dpi),
                                        fullW - ScaleDpi(20, dpi), ScaleDpi(18, dpi), hwnd,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSpeculativeNote)),
                                        GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hLblSpecNote, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    data.advancedControls.push_back(hLblSpecNote);

    for (HWND hCtrl : data.advancedControls) {
        ShowWindow(hCtrl, SW_HIDE);
    }
    UpdateCapabilityControls(hwnd, config.provider, config.model);

    // --- Action Buttons (Bottom of client area) ---
    const int actionW = ScaleDpi(100, dpi);
    const int actionH = ScaleDpi(28, dpi);
    const int actionY = clientH - actionH - ScaleDpi(14, dpi);
    const int cancelX = marginX + fullW - actionW;
    const int okX = cancelX - actionW - ScaleDpi(10, dpi);

    HWND btnOk = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                 okX, actionY, actionW, actionH, hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOk)), GetModuleHandleW(nullptr), nullptr);
    HWND btnCancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                     cancelX, actionY, actionW, actionH, hwnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancel)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(btnOk, WM_SETFONT, reinterpret_cast<WPARAM>(fontBold), TRUE);
    SendMessageW(btnCancel, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_NULL && msg.hwnd == nullptr) {
            break;
        }
        if (IsDialogMessageW(hwnd, &msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (data.font && data.font != GetStockObject(DEFAULT_GUI_FONT)) DeleteObject(data.font);
    if (data.fontBold && data.fontBold != data.font) DeleteObject(data.fontBold);
    if (data.fontTitle && data.fontTitle != data.font) DeleteObject(data.fontTitle);
    if (hAppIcon) DestroyIcon(hAppIcon);
    if (hSmallIcon) DestroyIcon(hSmallIcon);

    if (parent) {
        EnableWindow(parent, TRUE);
        SetForegroundWindow(parent);
    }

    if (data.accepted) {
        config = data.config;
        return true;
    }
    return false;
}

} // namespace aiie