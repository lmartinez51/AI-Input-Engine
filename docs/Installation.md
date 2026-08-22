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
| `AIInputEngine.exe` | Tray host (hook mode + UI) |
| `AIInputEngineTSF.dll` | TSF text service (IME mode) |
| `aiie_tsfreg.exe` | TSF registration/verification tool |
| `aiie_unit_tests.exe` | Unit tests |
| `aiie_integration_test.exe` | Notepad end-to-end test |

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

## Configure the Gemini provider (optional)

The mock provider works offline and needs no setup. For real translation:

1. Get an API key from Google AI Studio.
2. Open Settings (tray menu) and select provider "Gemini", enter the key, or
   set the `GEMINI_API_KEY` environment variable.
3. Save; the provider is rebuilt live without restarting.

## Configuration file

Settings are stored in `%APPDATA%\AIInputEngine\config.json`. Values: provider,
model, endpoint, API key, source/target languages, debounce milliseconds,
minimum/maximum buffer characters, hotkeys, enabled state. The file is
JSON UTF-8; corrupt values fall back to defaults and are reported.

## Hotkeys

| Action | Default |
|---|---|
| Toggle AI Input | `Ctrl+Alt+Space` |
| Swap source/target | `Ctrl+Alt+S` |
| Diagnostics | `Ctrl+Alt+D` |

## Uninstall

1. Exit the tray app (right-click tray icon -> Exit).
2. `aiie_tsfreg.exe --unregister` if TSF mode was used.
3. Delete `%APPDATA%\AIInputEngine\` (config and logs) and the build folder.