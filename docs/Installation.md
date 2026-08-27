# Installation

## Requirements

- Windows 10 or Windows 11, x64.
- To build from source: Visual Studio 2026 (or 2022) Build Tools with the
  "Desktop development with C++" workload, and CMake 3.24+.
- No runtime dependencies: the binaries use only the Windows API and the C++
  standard library. No third-party libraries are bundled.

## Build

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release --parallel
```

Output binaries land in `build\Release\` (or `build\Debug\`):

| Binary | Purpose |
|---|---|
| `AIInputEngine.exe` | Tray host (hook mode, Smart Paste, settings & diagnostics UI) |
| `AIInputEngineTSF.dll` | TSF text service (native IME mode) |
| `aiie_tsfreg.exe` | TSF registration/verification tool |
| `aiie_unit_tests.exe` | Comprehensive unit & simulation test suite (277 tests) |
| `aiie_openaitest.exe` | Live OpenAI connectivity, streaming, and model validation tool |
| `aiie_geminitest.exe` | Live Gemini diagnostic tool |
| `aiie_tsfhost.exe` | TSF host proof tool verifying composition -> transformation -> commit |
| `aiie_integration_test.exe` | Notepad end-to-end hook integration test |

## Run in hook mode (no registration)

```powershell
.\build\Release\AIInputEngine.exe
```

A tray icon appears. The engine is enabled by default with the **mock**
provider (offline dictionary, no API key needed). Type a sentence ending in
`.` `!` `?`, or pause while typing, and the text is translated in place.

## Run in TSF mode (native IME integration)

```powershell
.\build\Release\aiie_tsfreg.exe --register
```

- Registration is per-user (HKCU); no administrator rights are required.
- Sign out and sign back in (or restart the session) so the system loads the
  service.
- The TSF path provides native composition inside applications that support
  IME input. The hook path remains available as a fallback and is what the
  tray host uses.
- To remove: `aiie_tsfreg.exe --unregister`, then sign out and back in.

### TSF caveats

- The DLL must stay at the path it was registered from; moving or deleting it
  breaks the registration (re-register after moving).
- The service is in-proc inside each application that loads it; per-user
  registration only affects the user that registered it.
- Verify registration: `aiie_tsfreg.exe --verify`.

## Configure Providers (OpenAI & Gemini)

The mock provider works offline and needs no setup. For real AI translation & rewrite:

1. **OpenAI**: Enter your API key in Settings (stored securely in Windows Credential Manager DPAPI) or set `OPENAI_API_KEY`. Supports `gpt-5.6-luna`, `gpt-5.4`, `gpt-4o`, `gpt-4o-mini`, `o1`, `o3-mini`.
2. **Gemini**: Enter your API key in Settings or set `GEMINI_API_KEY`. Supports `gemini-3.7-flash`, `gemini-3.6-flash`, `gemini-3.5-flash-lite`.
3. Save; the active provider is rebuilt live without restarting.

## Configuration file & Credential Storage

- Settings are stored in `%APPDATA%\AIInputEngine\config.json`. Values: provider,
  model, endpoint, source/target languages, tone profile, debounce milliseconds,
  minimum/maximum buffer characters, hotkeys, speculative execution enabled state.
- API keys are encrypted via **Windows DPAPI** and kept in Windows Credential Manager.

## Hotkeys (Configurable)

| Action | Default |
|---|---|
| Toggle AI Input | `Ctrl+Shift+Space` |
| Smart Paste | `Ctrl+Shift+V` |
| Swap source/target | `Ctrl+Shift+S` |
| Diagnostics Window | `Ctrl+Shift+D` |

## Uninstall

1. Exit the tray app (right-click tray icon -> Exit).
2. `aiie_tsfreg.exe --unregister` if TSF mode was used.
3. Delete `%APPDATA%\AIInputEngine\` (config and logs) and the build folder.
4. Remove stored credentials via Windows Credential Manager if desired.