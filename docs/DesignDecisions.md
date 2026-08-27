# Design decisions

This file records *why* the code is the way it is. Read it before changing
anything architectural.

## Two input paths: TSF (primary) + low-level hook (fallback)

- **TSF** is the only sanctioned way to do IME-style text replacement; commits
  are native edit sessions inside the target application, so they behave like
  the user's own typing (undo, autocorrect, screen readers all work). It is
  in-proc, which means the text service must be registered per-user and loaded
  by each application.
- **WH_KEYBOARD_LL hook** works everywhere, needs no registration, and is what
  the tray host uses by default. Commits are synthetic keystrokes, which are
  slightly less "native" but robust across all legacy and modern Win32/UWP applications.
- Both feed the same backend-agnostic engine so behavior is identical.

## Commits via SendInput use VK + scan codes, not KEYEVENTF_UNICODE

Empirically found on Windows 11 Notepad (WinUI3): text injected with
`KEYEVENTF_UNICODE` is routed through the application's TSF input pipeline and
mangled (`"hello world."` arrived as `"oooooooorld."`). Injecting real
virtual-key/scan-code events with shift handling — exactly like typing —
survives every application tested. `InjectText` maps each character with
`VkKeyScanW` + `MapVirtualKeyW` and falls back to `KEYEVENTF_UNICODE` only for
characters with no keyboard mapping (e.g., exotic symbols).

## The engine never loses user text

- Keys always pass through the hook; the engine only *observes*.
- A replacement is committed only when the current buffer still equals the
  snapshot the result was computed for (`buffer_ == inflightSnapshot_`), and
  the replacement deletes exactly `sourceLength` characters.
- On any failure (provider error, network, stale result) the buffer is left
  untouched and the user's text stays.
- This is a hard safety contract; tests assert it
  (`engine_failure_keeps_user_text`).

## Latest-wins concurrency & monotonic IDs

Translation is async; users keep typing. Every request has a monotonic ID;
`TranslationPipeline` tracks the latest requested ID and drops results for
older requests (they are delivered as `ok=false, error="stale"` and never
committed). Combined with the buffer snapshot check, this makes
"type fast + slow network" safe: at most one replacement ever lands, for the
text that is really on screen.

## Pre-Commit Hesitation Prediction (PCHP) Speculative Translation

Typing cadence has natural pauses (hesitations) where the user reviews what
they wrote before typing terminal punctuation (`.` `!` `?`) or moving to the
next thought.
- **PCHP Constants**: 450 ms hesitation threshold, 10-character minimum buffer,
  8-character minimum delta growth, and 750 ms cooldown between speculative requests.
- **Speculative Streaming**: When a hesitation is detected, the engine fires a
  background speculative request. If the user stops or enters terminal punctuation,
  the pre-computed candidate is consumed instantly via `TryConsumeExactCandidate`.
- **Policy 2 Terminal Normalization**: If the candidate was computed for `"Hola mundo"`
  and the user commits `"Hola mundo."`, the manager matches the root text and
  safely normalizes the terminal punctuation, achieving near 0-ms commit latency.
- **Strict Invalidation**: If the user edits or backspaces, the candidate is
  instantly invalidated without blocking or altering input.

## Smart Paste (`SmartPasteManager`)

Users frequently copy text in one language and wish to paste the translated version
directly into an active document or chat window (`Ctrl+Shift+V`):
- **Target Context Verification**: Captures `targetHwnd` and `processId` upon hotkey
  trigger. If the active window changes before translation completes, the paste is
  aborted to avoid pasting into the wrong application.
- **Clipboard Restoration & Sequence Numbers**: The clipboard is temporarily updated
  with the translation, synthetically pasted (`Ctrl+V`), and then restored to the
  user's original content. To avoid clobbering concurrent clipboard updates,
  restoration uses `GetClipboardSequenceNumber` — if the user or another app copied
  new content in the interim, the restoration is safely skipped.

## SSE Streaming Loop & TTFT Tracking (`OpenAIProvider`)

- Instead of waiting for the full HTTP response, the OpenAI provider uses
  Server-Sent Events (`stream: true`) to stream delta tokens in real time.
- Measures **Time-To-First-Token (TTFT)** for diagnostics and telemetry.
- Supports reasoning effort configuration (`low`, `medium`, `high`) on reasoning
  models, automatically sanitized for models that do not support it.
- In-flight streams are instantly aborted via WinHTTP handles when a newer
  request arrives.

## Detached provider workers must never capture `this`

`MockProvider` and test providers simulate latency with detached worker threads.
A worker may still be sleeping after the owning provider is destroyed, so workers
capture **only a shared `std::shared_ptr<atomic<bool>>` cancellation flag**
(and copies of request/callback), never `this`.

All tests and components use shared `alive_` tokens to ensure callbacks arriving
after object destruction safely no-op without use-after-free crashes.

## Security & DPAPI Credential Storage

- API keys are stored in Windows Credential Manager using Windows DPAPI encryption.
- Keys are never stored in plaintext JSON files or checked into repositories.
- `Logger::Redact` scrubs all known API key strings from logs and diagnostics snapshots.

## No third-party dependencies

Everything is hand-rolled: JSON parser, UTF-8/16 conversion, test harness,
tray UI. Rationale: zero supply-chain surface, no license issues, full control
over edge cases (e.g., surrogate pairs in JSON). The JSON parser is strict and
returns defaults on any malformed input.

## Per-user TSF registration (HKCU)

No admin rights, no machine-wide pollution. Cost: each Windows user must
register their own copy, and the DLL path must stay stable. The
`aiie_tsfreg` tool (register/unregister/verify) keeps it scriptable.

## Status popup

A small transient window shows "translating..." / errors. It exists because
commit latency can exceed a debounce pause, and silent transformation feels
like the machine ate the text. The popup never takes focus.

## What is intentionally NOT in v1

- Undo of a *translation* (native apps undo the injected keystrokes, but
  TSF-mode undo semantics are not mapped to a single "undo translation").
- Autocorrect-style mid-word replacement (buffer snapshots require commit
  boundaries; mid-word replacement is a future refinement).
- Machine-wide configuration or multi-user policy.
- macOS/Linux (Windows input stack is the product surface).