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
  slightly less "native" but acceptable for a v1.
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

## Latest-wins concurrency

Translation is async; users keep typing. Every request has a monotonic id;
`TranslationPipeline` tracks the latest requested id and drops results for
older requests (they are delivered as `ok=false, error="stale"` and never
committed). Combined with the buffer snapshot check, this makes
"type fast + slow network" safe: at most one replacement ever lands, for the
text that is really on screen.

## Detached provider workers must never capture `this`

`MockProvider` and the test `FakeProvider` simulate latency with detached
worker threads. A worker may still be sleeping after the owning provider is
destroyed, so workers capture **only a shared `std::shared_ptr<atomic<bool>>`
cancellation flag** (and copies of request/callback), never `this`.

Lesson learned the hard way: a Release-only intermittent crash (0xC0000005 in
ntdll at random addresses) turned out to be test-lifecycle use-after-free —
the `pipeline_cancel_all` test ended while its 100 ms worker was still
sleeping; the worker then read `cancelled_` on a destroyed provider. The
fix had two halves: workers never touch the provider after the sleep, and
tests always wait for every in-flight callback before returning.

## Debounce + terminator submit

Translation is submitted after an idle window (debounce, default 300 ms) or
immediately on a terminator (`. ! ?`). This balances responsiveness against
not flooding the provider while typing. The debouncer thread only schedules;
the actual engine work runs on the input thread via `IDispatcher`.

## No third-party dependencies

Everything is hand-rolled: JSON parser, UTF-8/16 conversion, test harness,
tray UI. Rationale: no supply-chain surface, no license issues, full control
over edge cases (e.g., surrogate pairs in JSON). The JSON parser is strict and
returns defaults on any malformed input.

## Per-user TSF registration (HKCU)

No admin rights, no machine-wide pollution. Cost: each Windows user must
register their own copy, and the DLL path must stay stable. The
`aiie_tsfreg` tool (register/unregister/verify) keeps it scriptable.

## Custom test harness

`tests/mini_test.h` is a ~170-line dependency-free framework (register,
run, CHECK macros, failure counting). Integration with the main binary is
nonexistent — which is exactly the point: unit tests run headless, fast, and
deterministically.

## UI without resource files

Tray icon, settings dialog, and diagnostics window are built
programmatically (no `.rc`/resource compiler). Cost: more code for dialogs;
benefit: no extra build steps, everything is visible in source, and the
project stays a plain CMake build.

## Status popup

A small transient window shows "translating..." / errors. It exists because
commit latency can exceed a debounce pause, and silent transformation feels
like the machine ate the text. The popup never takes focus.

## Security and secrets

- API key from config or environment; never hard-coded, never logged
  (`Logger::Redact`).
- Single-instance mutex prevents two hooks fighting over the same keys.
- The hook passes every key through — it never swallows input, so a crash can
  at worst stop translations, never block typing.

## What is intentionally NOT in v1

- Undo of a *translation* (native apps undo the injected keystrokes, but
  TSF-mode undo semantics are not mapped to a single "undo translation").
- Autocorrect-style mid-word replacement (buffer snapshots require commit
  boundaries; mid-word replacement is a future refinement).
- Machine-wide configuration or multi-user policy.
- macOS/Linux (Windows input stack is the product surface).