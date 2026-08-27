# Development

## Prerequisites

- Windows 10/11 x64.
- Visual Studio Build Tools with "Desktop development with C++" (MSVC 14.51+ or 2022+).
- CMake 3.24+.

## Build

```powershell
# configure once
cmake -S . -B build -G "Visual Studio 18 2026" -A x64

# build a configuration
cmake --build build --config Debug --parallel
cmake --build build --config Release --parallel

# or via the helper script
.\scripts\build.ps1 -Config Debug
```

Warnings are errors (`/W4 /WX`). The codebase is C++20, Unicode-only
(`UNICODE`, `WIN32_LEAN_AND_MEAN`, `NOMINMAX`, `WINVER=0x0A00`), and has zero
third-party dependencies (custom JSON parser, custom test harness).

## Tests

```powershell
# unit tests + simulation benchmarks (headless, safe, fast)
.\scripts\run_tests.ps1 -Config Debug

# or run the executable directly
.\build\Debug\aiie_unit_tests.exe

# + Notepad integration test (opens windows and steals focus)
.\scripts\run_tests.ps1 -Config Debug -WithIntegration
```

### Unit & Simulation tests

`aiie_unit_tests.exe` — 277 tests, ~1206 checks, no external dependencies.
Sources in `tests/unit/`, harness in `tests/mini_test.h`.

Includes:
- **Core State Machine & Hook Tests**: Buffer version guards, backspace surrogate pair handling, terminator immediate submissions.
- **Provider & SSE Parser Tests**: Chunk accumulation, TTFT measurement, error events, reasoning effort serialization.
- **Smart Paste Tests**: Sequence snapshot verification, target window mismatch aborts, clipboard restoration.
- **Diagnostics Window Tests**: Control creation, log ring buffer search/filtering, selection preservation.
- **PCHP Statistical Simulations** (`speculative_phase2_sim.cpp`): Multi-seed human typing behavior distribution models and hit-rate audits.
- **Trigger Optimization Matrix** (`trigger_optimizer_sim.cpp`): 12-scenario typing benchmark verifying up to 77% request reduction.

### Integration test

`aiie_integration_test.exe` — launches real Notepad (WinUI3), types
`hola mundo.` through the real hook backend, and expects `hello world.` in the
document (read via UI Automation `ValuePattern`).

Known quirks:

- Win11 Notepad hands off to a pre-existing process; the test snapshots the
  pre-existing "Notepad" windows and targets only **new** windows.
- The edit control is `RichEditD2DPT`; `WM_GETTEXT`/clipboard reads are
  unreliable there, which is why the test uses UI Automation.
- The test must own keyboard focus; do not type while it runs.
- Leftover test windows are closed at start and end (title markers:
  `hola`, `hello`, `PROBE`, `PROBE2`, `ooooo`, `undo`, `.........`,
  `Untitled`); the target document is cleared before typing.

### TSF registration check

`aiie_tsfreg.exe --verify` reports whether the TSF service is registered for
the current user. `run_tests.ps1` warns (not fails) when unregistered unless
`-RequireTsfRegistered` is passed.

## Conventions

- Follow the existing code style: 4-space indent, `CamelCase` types,
  `camelCase_` members, `namespace aiie`, `//` comments only.
- No comments unless they explain *why* (not *what*).
- Never add third-party dependencies; extend the custom utilities instead.
- Keep the safety contract: failure must never delete user text.
- Secrets (API keys) must never be logged or committed; use
  `Logger::Redact` when logging provider data.

## Useful CLI Tools

```powershell
# Live OpenAI smoke test matrix (responses API, streaming, reasoning)
.\build\Release\aiie_openaitest.exe --matrix

# Live Gemini diagnostic probe
.\build\Release\aiie_geminitest.exe

# Register/unregister/verify TSF text service
.\build\Release\aiie_tsfreg.exe --register
.\build\Release\aiie_tsfreg.exe --verify
.\build\Release\aiie_tsfreg.exe --unregister

# Package portable distribution zip
.\tools\package_dist.ps1

# Application config and logs location
Get-ChildItem $env:APPDATA\AIInputEngine
```