# Development

## Prerequisites

- Windows 10/11 x64.
- Visual Studio Build Tools with "Desktop development with C++" (MSVC 14.51+).
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
# unit tests + TSF registration check (safe, no windows)
.\scripts\run_tests.ps1 -Config Debug

# + Notepad integration test (opens windows and steals focus)
.\scripts\run_tests.ps1 -Config Debug -WithIntegration
```

### Unit tests

`aiie_unit_tests.exe` — 76 tests, ~247 checks, no external dependencies.
Sources in `tests/unit/`, harness in `tests/mini_test.h`.

Notes:

- The harness installs a crash filter that prints the crashing module + RVA
  (`CRASH: code=... at ... in <module> +0x...`) — use `dumpbin /disasm` or a
  debugger with the matching PDB to resolve the offset.
- Provider tests use artificial delays and **must wait for every in-flight
  callback** before the test returns (see the use-after-free history in
  DesignDecisions.md). `FakeProvider`/`MockProvider` workers capture only a
  shared cancellation flag, never `this`.

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

## Useful commands

```powershell
# resolve a crash RVA to a function
dumpbin /disasm build\Debug\aiie_unit_tests.exe > disasm.txt   # then search the offset

# register/unregister/verify TSF
.\build\Release\aiie_tsfreg.exe --register
.\build\Release\aiie_tsfreg.exe --verify
.\build\Release\aiie_tsfreg.exe --unregister

# log location (config + logs)
Get-ChildItem $env:APPDATA\AIInputEngine
```