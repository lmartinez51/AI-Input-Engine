# Architecture

AI Input Engine intercepts keyboard input at the Windows input layer, holds the
typed text in a composition buffer, translates it asynchronously, and replaces
the text in the target application before it is committed. The user sees their
text transform in place; the target application receives the translation.

Additionally, the engine provides **Smart Paste (`Ctrl+Shift+V`)** to transform
clipboard contents in-place, and **Pre-Commit Hesitation Prediction (PCHP)**
speculative streaming to achieve near 0-ms commit latency.

## Input paths

Multiple independent input triggers feed the same engine core:

```
[Physical Keyboard]                             [Smart Paste (Ctrl+Shift+V)]
        |                                                    |
        +--> [TSF text service] (AIInputEngineTSF.dll)       |
        |        composition-based, native IME path          |
        |                                                    |
        +--> [WH_KEYBOARD_LL hook] (AIInputEngine.exe)       |
        |        keystroke-based fallback, works everywhere  |
        |                                                    |
        v                                                    v
   InputEngine                                       SmartPasteManager
   (Composition buffer & version guard)              (Target PID/HWND & clipboard guard)
        \                                                   /
         +-------------------------+-----------------------+
                                   |
                                   v
                          TranslationRequest
                                   |
                                   v
                          TranslationPipeline
                   (Exact Cache & Monotonic IDs)
                   + SpeculativeTranslationManager (PCHP)
                                   |
                                   v
                          ITranslationProvider
                       (Mock | Gemini | OpenAI SSE)
                                   |
                                   v
                          TranslationResult
                                   |
                                   v
             +---------------------+---------------------+
             |                                           |
             v                                           v
        InputEngine                              SmartPasteManager
   CommitReplacement()                         Safe Synthetic Paste
 (TSF Edit Session | SendInput)                & Clipboard Restore
```

### TSF path (primary, `src/TSF/`)

- In-process COM text service loaded by the target application.
- Intercepts `OnStartComposition`/`OnEndEdit`; captures composition text,
  translates it, and replaces it inside an edit session.
- Registered per-user (HKCU) via `aiie_tsfreg.exe`; no admin rights required.
- Commits via TSF edit session: fully native, no synthetic keystrokes, works
  with any application that supports IME composition (Notepad, Office,
  browsers, VS Code, Discord, Slack, Teams).

### Hook path (fallback, `src/Input/`)

- `WH_KEYBOARD_LL` low-level keyboard hook in the tray process.
- Every key event is seen twice: once at the hook and once when the engine
  synthesizes the commit; a self-injection signature guard (with a grace window)
  ensures injected keys never re-enter the engine.
- Keys always pass through; the hook never swallows keystrokes. The buffer is
  built from text keys, and on commit the engine deletes the last N characters
  and types the translation (`SendInput`, VK + scan-code based, see
  DesignDecisions.md).
- Because delivery of synthetic input is asynchronous, commits that arrive
  while a new buffer is being typed are version-guarded and dropped.

### Smart Paste path (`src/Composition/SmartPasteManager.cpp`)

- Triggered via dedicated hotkey (`Ctrl+Shift+V`).
- Ingests clipboard text, captures window/process context (`targetHwnd`, `processId`),
  and dispatches the translation through `TranslationPipeline`.
- On completion, verifies that target window focus and clipboard sequence are
  unchanged, performs a safe synthetic paste, and automatically restores the
  original clipboard content without leaving persistent traces.

## Speculative Translation Engine (`src/Translation/SpeculativeTranslationManager.cpp`)

- **Pre-Commit Hesitation Prediction (PCHP)**: Detects natural typing pauses
  (default 450 ms threshold, 10-char minimum, 8-char minimum growth, 750 ms cooldown)
  and preemptively streams translation candidates in the background.
- **Near-Zero Latency Commits**: When the user finishes typing or enters a terminator
  (`. ! ?`), the pipeline consumes the ready or streaming candidate with exact-match
  verification or Policy 2 terminal punctuation normalization, reducing perceived commit
  latency to ~0 ms.
- **Zero Regression & Safety**: If the user continues typing, stale speculative
  requests are automatically cancelled and discarded without touching the screen
  or interrupting the keystroke stream.

## Engine core (`src/Composition/InputEngine.cpp`)

State machine: `Idle -> Composing -> Translating -> Error`.

- `HandleCharacter` appends to the buffer, arms the debouncer / speculative engine,
  and submits a translation when the debounce window elapses or a terminator (`. ! ?`)
  is typed.
- `HandleBackspace` edits the buffer; results for text no longer present are
  discarded.
- Every request carries a monotonically increasing ID; `TranslationPipeline`
  tracks `latestRequested_` and marks stale results, which are never committed.
- Safety contract: a result is committed only if the current buffer still
  exactly matches the text the result was computed for
  (`buffer_ == inflightSnapshot_`), and even then the replacement deletes only
  `sourceLength` characters. On any failure, the user's typed text stays
  untouched.

## Threading model

| Thread | Owner | Role |
|---|---|---|
| Input thread | tray message loop / TSF thread | receives keys, edits buffer, executes commits |
| Debouncer thread | engine | fires after idle window; hops back to input thread via `IDispatcher` |
| Speculative manager | pipeline | evaluates PCHP gates and coordinates background candidate pre-fetching |
| Provider worker(s) | providers | async translation (SSE streaming loop, WinHTTP, or local mock) |
| Dispatcher thread | `MessageDispatcher` | runs posted tasks on the input thread (message-window based) |

Cross-thread communication always goes through `IDispatcher::Post` or
`std::atomic`. Providers are invoked asynchronously and never block the input
thread; their callbacks only marshal data.

## Configuration and secrets

- `%APPDATA%\AIInputEngine\config.json` — user settings (provider, models,
  languages, tone profile, debounce, buffer limits, hotkeys, speculative execution).
- API keys are securely stored and encrypted in **Windows Credential Manager (DPAPI)**
  under target name `AIInputEngine/<Provider>`.
- Environment variables (`OPENAI_API_KEY`, `GEMINI_API_KEY`) serve as seamless fallbacks.
- Secrets are never written to logs; `Logger::Redact` scrubs known secret
  values from all log lines and diagnostics buffers.

## UI (`src/UI/`)

- `TrayApp`: hidden message window + tray icon, hotkeys, popup menu, engine
  wiring.
- `SettingsDialog`: modern tabbed/expandable settings dialog (model capabilities,
  parameters, API key management, tone profiles, custom instructions).
- `DiagnosticsWindow`: live log viewer with search/filter, ring buffer snapshot,
  PCHP speculative telemetry metrics, and provider connection status.
- `StatusWindow`: transient on-screen status popup while translating.

## Test layout

- `tests/unit/` — 278 unit tests, ~1224 checks, no external dependencies, custom harness
  `tests/mini_test.h`.
- `tests/unit/speculative_phase2_sim.cpp` & `tests/unit/trigger_optimizer_sim.cpp` —
  multi-seed statistical simulations and 12-scenario typing benchmark matrices.
- `tests/integration/NotepadTest.cpp` — end-to-end test against real Notepad
  (WinUI3): launches Notepad, targets newly created windows, types
  Spanish through the real hook, reads the document via UI Automation, and expects
  the English translation.
- `tools/tsfreg/` — TSF registration tool with `--register`, `--unregister`,
  `--verify`.
- `tools/openaitest/` & `tools/geminitest/` — live provider connectivity and streaming CLI tools.
- `tools/tsfhost/` & `tools/realtsf/` — standalone TSF text service composition proof tools.