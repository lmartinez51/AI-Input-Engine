cmake_minimum_required(VERSION 3.20)
project(AIInputEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable all warnings and treat them as errors for clean builds.
if(MSVC)
    add_compile_options(/W4 /WX /permissive- /utf-8)
    add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ---------------------------------------------------------------------------
# Core static library containing the domain model, providers, translation
# pipeline, input composition engine, and configuration store.
# ---------------------------------------------------------------------------
add_library(aiie_core STATIC
    src/Core/Utf.h
    src/Core/Utf.cpp
    src/Core/Json.h
    src/Core/Json.cpp
    src/Core/Hotkey.h
    src/Core/Hotkey.cpp
    src/Core/Logging.h
    src/Core/Logging.cpp
    src/Core/Dispatcher.h
    src/Core/Dispatcher.cpp
    src/Core/StatusWindow.h
    src/Core/StatusWindow.cpp
    src/Core/ClipboardHelper.h
    src/Core/ClipboardHelper.cpp
    src/Configuration/AppConfig.h
    src/Configuration/AppConfig.cpp
    src/Configuration/LanguageRegistry.h
    src/Configuration/LanguageRegistry.cpp
    src/Configuration/ConfigStore.h
    src/Configuration/ConfigStore.cpp
    src/Configuration/CredentialStore.h
    src/Configuration/CredentialStore.cpp
    src/Configuration/ModelCapabilities.h
    src/Configuration/ModelCapabilities.cpp
    src/Translation/TranslationTypes.h
    src/Translation/TranslationPrompt.h
    src/Translation/TranslationPrompt.cpp
    src/Translation/TranslationCache.h
    src/Translation/TranslationCache.cpp
    src/Translation/Debouncer.h
    src/Translation/Debouncer.cpp
    src/Translation/TranslationPipeline.h
    src/Translation/TranslationPipeline.cpp
    src/Providers/MockProvider.h
    src/Providers/MockProvider.cpp
    src/Providers/GeminiProvider.h
    src/Providers/GeminiProvider.cpp
    src/Providers/OpenAIProvider.h
    src/Providers/OpenAIProvider.cpp
    src/Providers/ProviderFactory.h
    src/Providers/ProviderFactory.cpp
    src/Composition/InputEngine.h
    src/Composition/InputEngine.cpp
    src/Composition/SmartPasteManager.h
    src/Composition/SmartPasteManager.cpp
    src/Input/HookBackend.h
    src/Input/HookBackend.cpp
    src/Input/KeyTranslator.h
    src/Input/KeyTranslator.cpp
)

target_include_directories(aiie_core PUBLIC src)
target_link_libraries(aiie_core PUBLIC winhttp)
target_link_libraries(aiie_core PUBLIC advapi32)

# ---------------------------------------------------------------------------
# TSF text service DLL (loaded by the TSF manager into target processes).
# ---------------------------------------------------------------------------
add_library(AIInputEngineTSF SHARED
    src/TSF/TsfGuids.h
    src/TSF/TsfGuids.cpp
    src/TSF/DisplayAttributeInfo.h
    src/TSF/DisplayAttributeInfo.cpp
    src/TSF/TextService.h
    src/TSF/TextService.cpp
    src/TSF/TextServiceModule.cpp
    src/TSF/AIInputEngineTSF.def
)
set_target_properties(AIInputEngineTSF PROPERTIES
    OUTPUT_NAME "AIInputEngineTSF"
    PREFIX ""
)
target_link_libraries(AIInputEngineTSF PRIVATE aiie_core)

# ---------------------------------------------------------------------------
# System tray application (hosts the hook-based input engine + UI).
# ---------------------------------------------------------------------------
add_executable(AIInputEngine WIN32
    src/UI/TrayApp.h
    src/UI/TrayApp.cpp
    src/UI/SettingsDialog.h
    src/UI/SettingsDialog.cpp
    src/UI/DiagnosticsWindow.h
    src/UI/DiagnosticsWindow.cpp
    src/Application/Main.cpp
    resources/resource.h
    resources/app.rc
)
target_link_libraries(AIInputEngine PRIVATE aiie_core)
target_link_libraries(AIInputEngine PRIVATE shell32 comctl32 ole32 uuid uiautomationcore)

# ---------------------------------------------------------------------------
# Unit tests (console).
# ---------------------------------------------------------------------------
add_executable(aiie_unit_tests
    tests/mini_test.h
    tests/unit/main.cpp
    tests/unit/json_test.cpp
    tests/unit/config_test.cpp
    tests/unit/debounce_test.cpp
    tests/unit/pipeline_test.cpp
    tests/unit/cache_test.cpp
    tests/unit/mock_test.cpp
    tests/unit/gemini_test.cpp
    tests/unit/openai_test.cpp
    tests/unit/unicode_test.cpp
    tests/unit/hotkey_test.cpp
    tests/unit/engine_test.cpp
    tests/unit/provider_factory_test.cpp
    tests/unit/shutdown_test.cpp
    tests/unit/settings_test.cpp
    tests/unit/smart_paste_test.cpp
    tests/unit/diagnostics_test.cpp
    src/UI/SettingsDialog.h
    src/UI/SettingsDialog.cpp
    src/UI/DiagnosticsWindow.h
    src/UI/DiagnosticsWindow.cpp
)
target_link_libraries(aiie_unit_tests PRIVATE aiie_core shell32 comctl32 ole32 uuid)
target_include_directories(aiie_unit_tests PRIVATE tests)
add_test(NAME unit_tests COMMAND aiie_unit_tests)

# ---------------------------------------------------------------------------
# Integration test: drives a real Notepad instance through the hook engine.
# ---------------------------------------------------------------------------
add_executable(aiie_integration_test
    tests/integration/NotepadTest.cpp
)
target_link_libraries(aiie_integration_test PRIVATE aiie_core ole32 uuid uiautomationcore)
add_test(NAME notepad_integration COMMAND aiie_integration_test)

# ---------------------------------------------------------------------------
# TSF registration / verification tool (console).
# ---------------------------------------------------------------------------
add_executable(aiie_tsfreg
    tools/tsfreg/main.cpp
)
target_link_libraries(aiie_tsfreg PRIVATE ole32 uuid)
add_test(NAME tsf_verify COMMAND aiie_tsfreg --verify)

# ---------------------------------------------------------------------------
# Live Gemini validation tool (console). Reads the key from GEMINI_API_KEY
# or the Windows Credential Manager; never prints the key.
# ---------------------------------------------------------------------------
add_executable(aiie_geminitest
    tools/geminitest/main.cpp
)
target_link_libraries(aiie_geminitest PRIVATE aiie_core)

# ---------------------------------------------------------------------------
# Live OpenAI validation tool (console). Reads the key from OPENAI_API_KEY
# or the Windows Credential Manager; never prints the key.
# ---------------------------------------------------------------------------
add_executable(aiie_openaitest
    tools/openaitest/main.cpp
)
target_link_libraries(aiie_openaitest PRIVATE aiie_core)

# ---------------------------------------------------------------------------
# TSF host proof tool (console): real TSF session that activates the TIP and
# verifies composition -> translation -> commit.
# ---------------------------------------------------------------------------
add_executable(aiie_tsfhost
    tools/tsfhost/main.cpp
    src/TSF/TsfGuids.cpp
)
target_link_libraries(aiie_tsfhost PRIVATE aiie_core ole32 uuid)

# ---------------------------------------------------------------------------
# WinHTTP diagnostic probe: replicates the provider's exact call sequence with
# per-phase timing (compares DEFAULT_PROXY vs NO_PROXY access types).
# ---------------------------------------------------------------------------
add_executable(aiie_winhttpprobe
    tools/winhttpprobe/main.cpp
)
target_link_libraries(aiie_winhttpprobe PRIVATE winhttp)

# ---------------------------------------------------------------------------
# Real-world validation (console): real Windows TSF + real Notepad + live
# Gemini API. Requires the TIP to be in the Windows TSF profile cache (refresh
# with a sign-out/reboot after registration) and the GEMINI_API_KEY env var.
# ---------------------------------------------------------------------------
add_executable(aiie_realtsf_test
    tools/realtsf/main.cpp
    src/TSF/TsfGuids.cpp
)
target_link_libraries(aiie_realtsf_test PRIVATE aiie_core ole32 uuid uiautomationcore)

enable_testing()