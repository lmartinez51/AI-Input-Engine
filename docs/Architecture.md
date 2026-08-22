# Architecture

AI Input Engine intercepts keyboard input at the Windows input layer, holds the
typed text in a composition buffer, translates it asynchronously, and replaces
the text in the target application before it is committed. The user sees their
text transform in place; the target application receives the translation.

## Input paths

Two independent input paths feed the same engine core:

```
Physical keyboard
   |
   +--> [TSF text service]  (AIInputEngineTSF.dll, in-proc in the target app)
   |        composition-based, native IME path
   |
   +--> [WH_KEYBOARD_LL hook]  (AIInputEngine.exe, tray host)
            keystroke-based fallback, works in every app
   |
   v
   InputEngine (composition state machine, backend-agnostic)
   v
   TranslationPipeline (debounce, submit, latest-wins, cache)
   v
   ITranslationProvider (Mock | Gemini)
   v
   InputEngine (version-guarded commit)
   v
   backend->CommitReplacement()  (TSF edit session | SendInput)
```

### TSF path (primary, `src/TSF/`)

- In-process COM text service loaded by the target application.
- Intercepts `OnStartComposition`/`OnEndEdit`; captures composition text,
  translates it, and replaces it inside an edit session.
- Registered per-user (HKCU) via `aiie_tsfreg.exe`; no admin rights required.
- Commits via TSF edit session: fully native, no synthetic keystrokes, works
  with any application that supports IME composition (Notepad, Office,
  browsers, VS Code, Discord).

### Hook path (fallback, `src/Input/`)

- `WH_KEYBOARD_LL` low-level keyboard hook in the tray process.
- Every key event is seen twice: once at the hook and once when the engine
  synthesizes the commit; a self-injection guard (with a 500 ms grace window)
  ensures injected keys never re-enter the engine.
- Keys always pass through; the hook never swallows keystrokes. The buffer is
  built from text keys, and on commit the engine deletes the last N characters
  and types the translation (`SendInput`, VK + scan-code based, see
  DesignDecisions.md).
- Because delivery of synthetic input is asynchronous, commits that arrive
  while a new buffer is being typed are version-guarded and dropped.

## Engine core (`src/Composition/InputEngine.cpp`)

State machine: `Idle -> Composing -> Translating -> Error`.

- `HandleCharacter` appends to the buffer, arms the debouncer, and submits a
  translation when the debounce window elapses or a terminator (`. ! ?`) is
  typed.
- `HandleBackspace` edits the buffer; results for text no longer present are
  discarded.
- Every request carries a monotonically increasing id; `TranslationPipeline`
  tracks `latestRequested_` and marks stale results, which are never committed.
- Safety contract: a result is committed only if the current buffer still
  exactly matches the text the result was computed for
  (`buffer_ == inflightSnapshot_`), and even then the replacement deletes only
  `sourceLength` characters. On any failure, the user's typed text stays
  untouched.

## Threading model

| Thread | Owner | Role |
|---|---|---|
| Input thread | tray message loop / TSF thread | receives keys, edits buffer |
| Debouncer thread | engine | fires after idle window; hops back to input thread via `IDispatcher` |
| Provider worker(s) | providers | async translation (network or sleep) |
| Dispatcher thread | `MessageDispatcher` | runs posted tasks on the input thread (message-window based) |

Cross-thread communication always goes through `IDispatcher::Post` or
`std::atomic`. Providers are invoked asynchronously and never block the input
thread; their callbacks only marshal data.

## Configuration and secrets

- `%APPDATA%\AIInputEngine\config.json` — user settings (provider, languages,
  debounce, buffer limits, hotkeys).
- API key from config or the `GEMINI_API_KEY` environment variable.
- Secrets are never written to logs; `Logger::Redact` scrubs known secret
  values from log lines.

## UI (`src/UI/`)

- `TrayApp`: hidden message window + tray icon, hotkeys, popup menu, engine
  wiring.
- `SettingsDialog`: modal settings dialog (built programmatically, no .rc).
- `DiagnosticsWindow`: modeless log viewer (ring buffer + provider status).
- `StatusWindow`: transient on-screen status popup while translating.

## Test layout

- `tests/unit/` — 76 unit tests (no external dependencies), custom harness
  `tests/mini_test.h`.
- `tests/integration/NotepadTest.cpp` — end-to-end test against real Notepad
  (WinUI3): launches Notepad, targets only newly created windows, types
  Spanish through the real hook, reads the document via UI Automation, expects
  the English translation.
- `tools/tsfreg/` — TSF registration tool with `--register`, `--unregister`,
  `--verify`.