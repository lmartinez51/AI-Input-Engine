#include "mini_test.h"

#include "Configuration/AppConfig.h"
#include "Configuration/ConfigStore.h"
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
    // OpenAI gpt-5.6-luna capabilities
    {
        const auto caps = GetModelCapabilities(ProviderId::OpenAI, L"gpt-5.6-luna");
        CHECK(caps.SupportsTemperature());
        CHECK(caps.SupportsTopP());
        CHECK(!caps.SupportsTopK());
        CHECK(caps.SupportsMaxOutputTokens());
        CHECK(caps.SupportsReasoningEffort());
        CHECK(caps.SupportsStreaming());
        CHECK(caps.SupportsImageInput());
        CHECK_STREQ(caps.reasoning.defaultLevel, L"none");
        CHECK_EQ(caps.reasoning.allowedLevels.size(), static_cast<size_t>(3));
    }
    // OpenAI gpt-5.4 and gpt-5.4-mini capabilities
    {
        const auto caps54 = GetModelCapabilities(ProviderId::OpenAI, L"gpt-5.4");
        CHECK(caps54.SupportsReasoningEffort());
        CHECK(caps54.SupportsTemperature());
        CHECK(caps54.SupportsTopP());
        CHECK(!caps54.SupportsTopK());

        const auto caps54m = GetModelCapabilities(ProviderId::OpenAI, L"gpt-5.4-mini");
        CHECK(caps54m.SupportsReasoningEffort());
        CHECK(caps54m.SupportsTemperature());
        CHECK(caps54m.SupportsTopP());
        CHECK(!caps54m.SupportsTopK());
    }
    // OpenAI gpt-4o / gpt-4o-mini (non-reasoning models)
    {
        const auto caps4o = GetModelCapabilities(ProviderId::OpenAI, L"gpt-4o");
        CHECK(!caps4o.SupportsReasoningEffort());
        CHECK(caps4o.SupportsTemperature());
        CHECK(caps4o.SupportsTopP());
        CHECK(!caps4o.SupportsTopK());
        CHECK(caps4o.SupportsMaxOutputTokens());

        const auto caps4om = GetModelCapabilities(ProviderId::OpenAI, L"gpt-4o-mini");
        CHECK(!caps4om.SupportsReasoningEffort());
        CHECK(caps4om.SupportsTemperature());
        CHECK(caps4om.SupportsTopP());
        CHECK(!caps4om.SupportsTopK());
    }
    // OpenAI reasoning model capability (temperature/top_p disallowed on o-series)
    {
        const auto capsO3 = GetModelCapabilities(ProviderId::OpenAI, L"o3-mini");
        CHECK(capsO3.SupportsReasoningEffort());
        CHECK_STREQ(capsO3.reasoning.defaultLevel, L"medium");
        CHECK(!capsO3.SupportsTemperature());
        CHECK(!capsO3.SupportsTopP());
        CHECK(!capsO3.SupportsTopK());
        CHECK(capsO3.SupportsMaxOutputTokens());

        const auto capsO1 = GetModelCapabilities(ProviderId::OpenAI, L"o1");
        CHECK(capsO1.SupportsReasoningEffort());
        CHECK(!capsO1.SupportsTemperature());
        CHECK(!capsO1.SupportsTopP());
        CHECK(!capsO1.SupportsTopK());
    }
    // Gemini capabilities
    {
        const auto caps = GetModelCapabilities(ProviderId::Gemini, L"gemini-3.7-flash");
        CHECK(caps.SupportsTemperature());
        CHECK(caps.SupportsTopP());
        CHECK(caps.SupportsTopK());
        CHECK(caps.SupportsMaxOutputTokens());
        CHECK(!caps.SupportsReasoningEffort());
        CHECK_EQ(caps.topK.defaultValue, 40);
        CHECK_EQ(caps.temperature.defaultValue, 0.2);
    }
    // Mock capabilities
    {
        const auto caps = GetModelCapabilities(ProviderId::Mock, L"");
        CHECK(!caps.SupportsTemperature());
        CHECK(!caps.SupportsTopP());
        CHECK(!caps.SupportsTopK());
        CHECK(!caps.SupportsMaxOutputTokens());
        CHECK(!caps.SupportsReasoningEffort());
    }
}

TEST(model_parameter_sanitization_rules) {
    // 1. gpt-4o: reasoningEffort stripped, topK stripped
    {
        ModelParameters raw;
        raw.reasoningEffort = L"low";
        raw.temperature = 0.7;
        raw.topP = 0.9;
        raw.topK = 50;
        raw.maxOutputTokens = 2048;

        const ModelParameters clean = SanitizeModelParameters(ProviderId::OpenAI, L"gpt-4o", raw);
        CHECK(!clean.reasoningEffort.has_value());
        CHECK(clean.temperature.has_value());
        CHECK_EQ(*clean.temperature, 0.7);
        CHECK(clean.topP.has_value());
        CHECK_EQ(*clean.topP, 0.9);
        CHECK(!clean.topK.has_value()); // topK stripped for OpenAI
        CHECK(clean.maxOutputTokens.has_value());
        CHECK_EQ(*clean.maxOutputTokens, 2048);
    }

    // 2. o3-mini: temperature stripped, topP stripped, topK stripped, valid reasoning preserved
    {
        ModelParameters raw;
        raw.reasoningEffort = L"high";
        raw.temperature = 0.7;
        raw.topP = 0.9;
        raw.topK = 50;

        const ModelParameters clean = SanitizeModelParameters(ProviderId::OpenAI, L"o3-mini", raw);
        CHECK(clean.reasoningEffort.has_value());
        CHECK_STREQ(*clean.reasoningEffort, L"high");
        CHECK(!clean.temperature.has_value()); // temperature stripped for o-series
        CHECK(!clean.topP.has_value());        // topP stripped for o-series
        CHECK(!clean.topK.has_value());
    }

    // 3. gpt-5.6-luna conflict: when reasoning is "low" (active), temperature is omitted
    {
        ModelParameters raw;
        raw.reasoningEffort = L"low";
        raw.temperature = 0.5;

        const ModelParameters clean = SanitizeModelParameters(ProviderId::OpenAI, L"gpt-5.6-luna", raw);
        CHECK(clean.reasoningEffort.has_value());
        CHECK_STREQ(*clean.reasoningEffort, L"low");
        CHECK(!clean.temperature.has_value()); // omitted to avoid conflict with active reasoning
    }

    // 4. gpt-5.6-luna non-conflict: when reasoning is "none", temperature is preserved
    {
        ModelParameters raw;
        raw.reasoningEffort = L"none";
        raw.temperature = 0.5;

        const ModelParameters clean = SanitizeModelParameters(ProviderId::OpenAI, L"gpt-5.6-luna", raw);
        CHECK(clean.reasoningEffort.has_value());
        CHECK_STREQ(*clean.reasoningEffort, L"none");
        CHECK(clean.temperature.has_value());
        CHECK_EQ(*clean.temperature, 0.5);
    }

    // 5. Gemini: reasoning stripped, topK preserved
    {
        ModelParameters raw;
        raw.reasoningEffort = L"medium";
        raw.temperature = 0.3;
        raw.topP = 0.95;
        raw.topK = 40;

        const ModelParameters clean = SanitizeModelParameters(ProviderId::Gemini, L"gemini-3.7-flash", raw);
        CHECK(!clean.reasoningEffort.has_value());
        CHECK(clean.temperature.has_value());
        CHECK_EQ(*clean.temperature, 0.3);
        CHECK(clean.topK.has_value());
        CHECK_EQ(*clean.topK, 40);
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
        CHECK_STREQ(buf, L"gemini-3.7-flash");
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
        CHECK_STREQ(buf, L"gemini-3.7-flash");
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

TEST(settings_dialog_compact_advanced_layout) {
    std::thread uiThread([] {
        AppConfig cfg;
        cfg.provider = ProviderId::Gemini;
        cfg.model = L"gemini-3.7-flash";
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
    HWND minEdit = GetDlgItem(hwnd, 25);   // kMinChars
    HWND maxBufEdit = GetDlgItem(hwnd, 26);// kMaxBuffer
    HWND maxTokEdit = GetDlgItem(hwnd, 30);// kMaxTokens
    HWND tempEdit = GetDlgItem(hwnd, 27);  // kTemperature
    HWND topPEdit = GetDlgItem(hwnd, 28);  // kTopP
    HWND topKEdit = GetDlgItem(hwnd, 29);  // kTopK
    HWND okBtn = GetDlgItem(hwnd, 1);      // kOk
    HWND cancelBtn = GetDlgItem(hwnd, 2);  // kCancel
    HWND modelCombo = GetDlgItem(hwnd, 16);// kModel
    HWND endpointEdit = GetDlgItem(hwnd, 17);// kEndpoint

    CHECK(toggleBtn != nullptr);
    CHECK(debEdit != nullptr);
    CHECK(minEdit != nullptr);
    CHECK(okBtn != nullptr);
    CHECK(cancelBtn != nullptr);

    // Expand Advanced Settings
    SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(23, BN_CLICKED), reinterpret_cast<LPARAM>(toggleBtn));
    CHECK_EQ(IsWindowVisible(debEdit), TRUE);
    CHECK_EQ(IsWindowVisible(minEdit), TRUE);
    CHECK_EQ(IsWindowVisible(okBtn), TRUE);
    CHECK_EQ(IsWindowVisible(cancelBtn), TRUE);

    // Verify Model and Endpoint controls are wide
    RECT rcModel{}, rcEndpoint{};
    GetWindowRect(modelCombo, &rcModel);
    GetWindowRect(endpointEdit, &rcEndpoint);
    const int modelW = rcModel.right - rcModel.left;
    const int endpointW = rcEndpoint.right - rcEndpoint.left;
    CHECK(modelW >= 250);
    CHECK(endpointW >= 250);

    // Verify numeric edits are compact (approx <= 150 px)
    RECT rcDeb{}, rcMin{}, rcMaxBuf{}, rcMaxTok{}, rcTemp{}, rcTopP{}, rcTopK{}, rcOk{};
    GetWindowRect(debEdit, &rcDeb);
    GetWindowRect(minEdit, &rcMin);
    GetWindowRect(maxBufEdit, &rcMaxBuf);
    GetWindowRect(maxTokEdit, &rcMaxTok);
    GetWindowRect(tempEdit, &rcTemp);
    GetWindowRect(topPEdit, &rcTopP);
    GetWindowRect(topKEdit, &rcTopK);
    GetWindowRect(okBtn, &rcOk);

    const int debW = rcDeb.right - rcDeb.left;
    const int minW = rcMin.right - rcMin.left;
    CHECK(debW <= 150);
    CHECK(minW <= 150);

    // Verify 2-Column alignment: Col 1 is to the left of Col 2
    CHECK(rcDeb.left < rcMin.left);
    CHECK(rcMaxBuf.left < rcMaxTok.left);
    CHECK(rcTemp.left < rcTopP.left);

    // Verify same row Y-coordinates (Row 1, Row 2, Row 3)
    CHECK_EQ(rcDeb.top, rcMin.top);
    CHECK_EQ(rcMaxBuf.top, rcMaxTok.top);
    CHECK_EQ(rcTemp.top, rcTopP.top);

    // Verify Row 2 is below Row 1, Row 3 below Row 2, Row 4 below Row 3
    CHECK(rcMaxBuf.top > rcDeb.top);
    CHECK(rcTemp.top > rcMaxBuf.top);
    CHECK(rcTopK.top > rcTemp.top);

    // Verify Action buttons (OK/Cancel) are placed below the advanced grid
    CHECK(rcOk.top > rcTopK.top);

    CloseAndJoin(hwnd, uiThread);
}

TEST(speculative_config_defaults_and_roundtrip) {
    // 1. Default config must have speculativeTranslationEnabled == false
    AppConfig defaultConfig;
    CHECK(!defaultConfig.speculativeTranslationEnabled);
    CHECK_EQ(defaultConfig.speculativeDebounceMs, 150);
    CHECK_EQ(defaultConfig.speculativeMinChars, 5);

    // 2. Deserialization of empty/legacy JSON must default to false
    const auto tempDir = std::filesystem::temp_directory_path() / L"aiie_test_spec_cfg";
    std::filesystem::create_directories(tempDir);
    const auto tempPath = tempDir / L"config.json";

    // Write empty JSON
    {
        HANDLE hFile = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            const char* emptyJson = "{}";
            DWORD written = 0;
            WriteFile(hFile, emptyJson, static_cast<DWORD>(strlen(emptyJson)), &written, nullptr);
            CloseHandle(hFile);
        }
    }

    ConfigStore legacyStore(tempPath);
    CHECK(legacyStore.Load());
    CHECK(!legacyStore.Get().speculativeTranslationEnabled);
    CHECK_EQ(legacyStore.Get().speculativeDebounceMs, 150);
    CHECK_EQ(legacyStore.Get().speculativeMinChars, 5);

    // 3. Round-trip save and load when explicitly enabled
    AppConfig enabledConfig;
    enabledConfig.provider = ProviderId::OpenAI;
    enabledConfig.model = L"gpt-5.6-luna";
    enabledConfig.speculativeTranslationEnabled = true;
    enabledConfig.speculativeDebounceMs = 200;
    enabledConfig.speculativeMinChars = 6;

    ConfigStore saveStore(tempPath);
    saveStore.Set(enabledConfig);
    CHECK(saveStore.Save());

    ConfigStore loadStore(tempPath);
    CHECK(loadStore.Load());
    CHECK(loadStore.Get().speculativeTranslationEnabled);
    CHECK_EQ(loadStore.Get().speculativeDebounceMs, 200);
    CHECK_EQ(loadStore.Get().speculativeMinChars, 6);

    // 4. Round-trip save and load when explicitly disabled
    enabledConfig.speculativeTranslationEnabled = false;
    saveStore.Set(enabledConfig);
    CHECK(saveStore.Save());

    ConfigStore loadStoreDisabled(tempPath);
    CHECK(loadStoreDisabled.Load());
    CHECK(!loadStoreDisabled.Get().speculativeTranslationEnabled);

    std::filesystem::remove_all(tempDir);
}

TEST(settings_dialog_speculative_control_state_and_provider_gating) {
    std::thread uiThread([] {
        AppConfig cfg;
        cfg.provider = ProviderId::Gemini;
        cfg.model = L"gemini-2.5-flash";
        cfg.speculativeTranslationEnabled = false;
        ShowSettingsDialog(nullptr, cfg);
    });

    HWND hwnd = WaitForDialog();
    CHECK(hwnd != nullptr);
    if (!hwnd) {
        if (uiThread.joinable()) uiThread.detach();
        return;
    }

    HWND toggleBtn = GetDlgItem(hwnd, 23); // kAdvancedToggle
    HWND specChk = GetDlgItem(hwnd, 33);   // kSpeculativeTranslation
    HWND specNote = GetDlgItem(hwnd, 34);  // kSpeculativeNote
    HWND provCombo = GetDlgItem(hwnd, 11); // kProvider

    CHECK(toggleBtn != nullptr);
    CHECK(specChk != nullptr);
    CHECK(specNote != nullptr);
    CHECK(provCombo != nullptr);

    // Expand Advanced Settings
    SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(23, BN_CLICKED), reinterpret_cast<LPARAM>(toggleBtn));
    CHECK_EQ(IsWindowVisible(specChk), TRUE);
    CHECK_EQ(IsWindowVisible(specNote), TRUE);

    // 1. Under Gemini provider, checkbox must be DISABLED (grayed out)
    CHECK_EQ(IsWindowEnabled(specChk), FALSE);
    CHECK_EQ(IsDlgButtonChecked(hwnd, 33), BST_UNCHECKED);
    wchar_t noteBuf[256] = {};
    GetWindowTextW(specNote, noteBuf, 256);
    CHECK(wcsstr(noteBuf, L"only available for OpenAI") != nullptr);

    // 2. Switch provider to OpenAI (index 2)
    ComboBox_SetCurSel(provCombo, 2);
    SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(11, CBN_SELCHANGE), reinterpret_cast<LPARAM>(provCombo));

    // Under OpenAI provider (default model gpt-5.6-luna), checkbox must be ENABLED
    CHECK_EQ(IsWindowEnabled(specChk), TRUE);
    GetWindowTextW(specNote, noteBuf, 256);
    CHECK(wcsstr(noteBuf, L"Translate in the background") != nullptr);

    // 3. User checks the checkbox
    CheckDlgButton(hwnd, 33, BST_CHECKED);
    CHECK_EQ(IsDlgButtonChecked(hwnd, 33), BST_CHECKED);

    // 4. Switch provider back to Mock (index 0) -> checkbox becomes disabled
    ComboBox_SetCurSel(provCombo, 0);
    SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(11, CBN_SELCHANGE), reinterpret_cast<LPARAM>(provCombo));
    CHECK_EQ(IsWindowEnabled(specChk), FALSE);

    CloseAndJoin(hwnd, uiThread);
}


