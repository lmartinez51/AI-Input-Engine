#include "mini_test.h"

#include "Configuration/AppConfig.h"
#include "Configuration/ModelCapabilities.h"
#include "UI/SettingsDialog.h"

#include <windows.h>
#include <windowsx.h>

#include <chrono>
#include <thread>

using namespace aiie;

namespace {

HWND WaitForDialog(int timeoutMs = 3000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        HWND hwnd = FindWindowW(L"AIInputEngine.SettingsDialog", nullptr);
        if (hwnd && IsWindow(hwnd)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == GetCurrentProcessId() && GetDlgItem(hwnd, 1) != nullptr && GetDlgItem(hwnd, 11) != nullptr) {
                return hwnd;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return nullptr;
}

void CloseAndJoin(HWND hwnd, std::thread& uiThread) {
    if (hwnd && IsWindow(hwnd)) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
    if (uiThread.joinable()) {
        uiThread.join();
    }
    for (int i = 0; i < 50; ++i) {
        HWND lingering = FindWindowW(L"AIInputEngine.SettingsDialog", nullptr);
        if (!lingering) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

} // namespace

TEST(model_capabilities_resolution) {
    // OpenAI capabilities
    {
        const auto caps = GetModelCapabilities(ProviderId::OpenAI, L"gpt-5.6-luna");
        CHECK(caps.supportsTemperature);
        CHECK(caps.supportsTopP);
        CHECK(!caps.supportsTopK);
        CHECK(caps.supportsMaxOutputTokens);
    }
    // OpenAI reasoning model capability (temperature disallowed on o-series)
    {
        const auto caps = GetModelCapabilities(ProviderId::OpenAI, L"o3-mini");
        CHECK(!caps.supportsTemperature);
        CHECK(caps.supportsTopP);
        CHECK(!caps.supportsTopK);
    }
    // Gemini capabilities
    {
        const auto caps = GetModelCapabilities(ProviderId::Gemini, L"gemini-3.6-flash");
        CHECK(caps.supportsTemperature);
        CHECK(caps.supportsTopP);
        CHECK(caps.supportsTopK);
        CHECK(caps.supportsMaxOutputTokens);
        CHECK_EQ(caps.defaultTopK, 40);
    }
    // Mock capabilities
    {
        const auto caps = GetModelCapabilities(ProviderId::Mock, L"");
        CHECK(!caps.supportsTemperature);
        CHECK(!caps.supportsTopP);
        CHECK(!caps.supportsTopK);
        CHECK(!caps.supportsMaxOutputTokens);
    }
}

TEST(settings_dialog_provider_transitions_no_freeze) {
    std::thread uiThread([] {
        AppConfig cfg;
        cfg.provider = ProviderId::Mock;
        ShowSettingsDialog(nullptr, cfg);
    });

    HWND hwnd = WaitForDialog();
    CHECK(hwnd != nullptr);
    if (!hwnd) {
        if (uiThread.joinable()) uiThread.detach();
        return;
    }

    HWND combo = GetDlgItem(hwnd, 11); // kProvider
    CHECK(combo != nullptr);

    auto switchProvider = [&](int idx) {
        ComboBox_SetCurSel(combo, idx);
        SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(11, CBN_SELCHANGE), reinterpret_cast<LPARAM>(combo));
    };

    // Test transition cycle: Mock -> Gemini -> OpenAI -> Gemini -> Mock -> OpenAI -> Mock
    switchProvider(1); // Gemini
    {
        HWND modelCombo = GetDlgItem(hwnd, 16); // kModel
        wchar_t buf[128] = {};
        GetWindowTextW(modelCombo, buf, 128);
        CHECK_STREQ(buf, L"gemini-3.6-flash");
    }

    switchProvider(2); // OpenAI
    {
        HWND modelCombo = GetDlgItem(hwnd, 16); // kModel
        wchar_t buf[128] = {};
        GetWindowTextW(modelCombo, buf, 128);
        CHECK_STREQ(buf, L"gpt-5.6-luna");
    }

    switchProvider(1); // Gemini
    {
        HWND modelCombo = GetDlgItem(hwnd, 16); // kModel
        wchar_t buf[128] = {};
        GetWindowTextW(modelCombo, buf, 128);
        CHECK_STREQ(buf, L"gemini-3.6-flash");
    }

    switchProvider(0); // Mock
    {
        HWND modelCombo = GetDlgItem(hwnd, 16); // kModel
        wchar_t buf[128] = {};
        GetWindowTextW(modelCombo, buf, 128);
        CHECK_STREQ(buf, L"(not applicable)");
    }

    switchProvider(2); // OpenAI
    {
        HWND modelCombo = GetDlgItem(hwnd, 16); // kModel
        wchar_t buf[128] = {};
        GetWindowTextW(modelCombo, buf, 128);
        CHECK_STREQ(buf, L"gpt-5.6-luna");
    }

    switchProvider(0); // Mock
    {
        HWND modelCombo = GetDlgItem(hwnd, 16); // kModel
        wchar_t buf[128] = {};
        GetWindowTextW(modelCombo, buf, 128);
        CHECK_STREQ(buf, L"(not applicable)");
    }

    CloseAndJoin(hwnd, uiThread);
}

TEST(settings_dialog_profile_transitions) {
    std::thread uiThread([] {
        AppConfig cfg;
        cfg.profile = TranslationProfile::Neutral;
        ShowSettingsDialog(nullptr, cfg);
    });

    HWND hwnd = WaitForDialog();
    CHECK(hwnd != nullptr);
    if (!hwnd) {
        if (uiThread.joinable()) uiThread.detach();
        return;
    }

    HWND profileCombo = GetDlgItem(hwnd, 14); // kProfile
    HWND customEdit = GetDlgItem(hwnd, 15);   // kCustomInstructions
    CHECK(profileCombo != nullptr);
    CHECK(customEdit != nullptr);

    // Initial state: Neutral -> customEdit disabled
    CHECK_EQ(IsWindowEnabled(customEdit), FALSE);

    // Switch to Custom (index 4)
    ComboBox_SetCurSel(profileCombo, 4);
    SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(14, CBN_SELCHANGE), reinterpret_cast<LPARAM>(profileCombo));
    CHECK_EQ(IsWindowEnabled(customEdit), TRUE);

    // Switch to Professional (index 1)
    ComboBox_SetCurSel(profileCombo, 1);
    SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(14, CBN_SELCHANGE), reinterpret_cast<LPARAM>(profileCombo));
    CHECK_EQ(IsWindowEnabled(customEdit), FALSE);

    // Switch back to Custom (index 4)
    ComboBox_SetCurSel(profileCombo, 4);
    SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(14, CBN_SELCHANGE), reinterpret_cast<LPARAM>(profileCombo));
    CHECK_EQ(IsWindowEnabled(customEdit), TRUE);

    CloseAndJoin(hwnd, uiThread);
}

TEST(settings_dialog_advanced_expand_collapse) {
    std::thread uiThread([] {
        AppConfig cfg;
        ShowSettingsDialog(nullptr, cfg);
    });

    HWND hwnd = WaitForDialog();
    CHECK(hwnd != nullptr);
    if (!hwnd) {
        if (uiThread.joinable()) uiThread.detach();
        return;
    }

    HWND toggleBtn = GetDlgItem(hwnd, 23); // kAdvancedToggle
    HWND debEdit = GetDlgItem(hwnd, 24);   // kDebounce
    CHECK(toggleBtn != nullptr);
    CHECK(debEdit != nullptr);

    // Initial: advanced controls are hidden
    CHECK_EQ(IsWindowVisible(debEdit), FALSE);

    // Click toggle button -> expanded
    SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(23, BN_CLICKED), reinterpret_cast<LPARAM>(toggleBtn));
    CHECK_EQ(IsWindowVisible(debEdit), TRUE);

    // Click toggle button -> collapsed
    SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(23, BN_CLICKED), reinterpret_cast<LPARAM>(toggleBtn));
    CHECK_EQ(IsWindowVisible(debEdit), FALSE);

    CloseAndJoin(hwnd, uiThread);
}

TEST(settings_dialog_ok_does_not_quit_app) {
    AppConfig cfg;
    cfg.provider = ProviderId::Mock;

    std::atomic<bool> dialogDone{false};
    std::atomic<bool> okAccepted{false};
    std::thread uiThread([&] {
        okAccepted.store(ShowSettingsDialog(nullptr, cfg));
        dialogDone.store(true);
    });

    HWND hwnd = WaitForDialog();
    CHECK(hwnd != nullptr);
    if (!hwnd) {
        if (uiThread.joinable()) uiThread.detach();
        return;
    }

    // Click OK (command ID 1) via PostMessage so message pump dispatches cleanly
    PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(1, BN_CLICKED), 0);

    if (uiThread.joinable()) {
        uiThread.join();
    }

    CHECK(dialogDone.load());
    CHECK(okAccepted.load());

    // Verify no WM_QUIT was posted to the message queue: PeekMessage with PM_NOREMOVE must not see WM_QUIT
    MSG msg{};
    const BOOL hasQuit = PeekMessageW(&msg, nullptr, WM_QUIT, WM_QUIT, PM_NOREMOVE);
    CHECK_EQ(hasQuit, FALSE);
}

TEST(settings_dialog_user_friendly_language_names) {
    std::thread uiThread([] {
        AppConfig cfg;
        cfg.sourceLanguage = L"es";
        cfg.targetLanguage = L"zh-Hans";
        ShowSettingsDialog(nullptr, cfg);
    });

    HWND hwnd = WaitForDialog();
    CHECK(hwnd != nullptr);
    if (!hwnd) {
        if (uiThread.joinable()) uiThread.detach();
        return;
    }

    HWND srcCombo = GetDlgItem(hwnd, 12); // kSourceLang
    HWND tgtCombo = GetDlgItem(hwnd, 13); // kTargetLang
    CHECK(srcCombo != nullptr);
    CHECK(tgtCombo != nullptr);

    // Visible text in the combo box must be human-readable, not internal code
    wchar_t bufSrc[128] = {};
    GetWindowTextW(srcCombo, bufSrc, 128);
    CHECK_STREQ(bufSrc, L"Spanish");

    wchar_t bufTgt[128] = {};
    GetWindowTextW(tgtCombo, bufTgt, 128);
    CHECK_STREQ(bufTgt, L"Chinese (Simplified)");

    // List items must be human readable
    const int count = ComboBox_GetCount(srcCombo);
    bool foundPt = false;
    bool foundRawPt = false;
    for (int i = 0; i < count; ++i) {
        wchar_t itemText[128] = {};
        ComboBox_GetLBText(srcCombo, i, itemText);
        if (wcscmp(itemText, L"Portuguese") == 0) foundPt = true;
        if (wcscmp(itemText, L"pt") == 0) foundRawPt = true;
    }
    CHECK(foundPt);
    CHECK(!foundRawPt);

    CloseAndJoin(hwnd, uiThread);
}

TEST(settings_dialog_operation_labels_and_target_visibility) {
    std::thread uiThread([] {
        AppConfig cfg;
        cfg.operation = TransformOperation::Translate;
        ShowSettingsDialog(nullptr, cfg);
    });

    HWND hwnd = WaitForDialog();
    CHECK(hwnd != nullptr);
    if (!hwnd) {
        if (uiThread.joinable()) uiThread.detach();
        return;
    }

    HWND opCombo = GetDlgItem(hwnd, 31);     // kOperation
    HWND tgtCombo = GetDlgItem(hwnd, 13);    // kTargetLang
    CHECK(opCombo != nullptr);
    CHECK(tgtCombo != nullptr);

    // In Translate mode, target is visible
    CHECK_EQ(IsWindowVisible(tgtCombo), TRUE);

    // Switch to Correct & Rewrite (index 1)
    ComboBox_SetCurSel(opCombo, 1);
    SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(31, CBN_SELCHANGE), reinterpret_cast<LPARAM>(opCombo));

    // In Correct & Rewrite mode, target is hidden and disabled
    CHECK_EQ(IsWindowVisible(tgtCombo), FALSE);
    CHECK_EQ(IsWindowEnabled(tgtCombo), FALSE);

    // Switch back to Translate (index 0)
    ComboBox_SetCurSel(opCombo, 0);
    SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(31, CBN_SELCHANGE), reinterpret_cast<LPARAM>(opCombo));

    CHECK_EQ(IsWindowVisible(tgtCombo), TRUE);
    CHECK_EQ(IsWindowEnabled(tgtCombo), TRUE);

    CloseAndJoin(hwnd, uiThread);
}

