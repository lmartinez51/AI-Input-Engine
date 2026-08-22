# Troubleshooting

## No tray icon

- Run `AIInputEngine.exe` directly from a console and check for error output.
- Check that no other instance is running: the app is single-instance
  (named mutex `Local\AIInputEngine.SingleInstance`); if a previous instance
  is hung, kill it from Task Manager and retry.
- Check `%APPDATA%\AIInputEngine\aiie.log` for startup errors.

## Nothing is being translated

1. **Enabled?** Right-click the tray icon; "AI Input enabled" must be
   checked. Default hotkey to toggle: `Ctrl+Alt+Space`.
2. **Provider?** Open Settings (tray menu). "Mock (offline dictionary)" works
   without configuration. For Gemini, the API key must be set and the model
   reachable.
3. **Language direction?** Settings source/target languages, or swap with
   `Ctrl+Alt+S`. The engine only processes text matching the source language
   pattern (Spanish characters for es->en).
4. **Buffer limits?** A very short debounce or a low `minChars` can commit
   text before a translation is ready. Defaults: debounce 300 ms,
   minChars 2, maxBufferChars 120.
5. **Focus?** The hook path needs the target window to have keyboard focus.
   The status popup shows "translating..." while a request is in flight.
6. **Diagnostics:** press `Ctrl+Alt+D` — the diagnostics window shows the log
   ring buffer and the provider status.

## Hotkeys do nothing

- Another application may have registered the same hotkey first
  (`RegisterHotKey` fails silently in that case). Change the hotkeys in
  Settings.
- Hotkeys work only while the tray app is running.

## TSF mode problems

- **`aiie_tsfreg.exe --verify` fails:** the service is not registered for the
  current user. Run `--register`, then sign out and sign back in.
- **Service not offered in applications:** some applications (e.g., older
  Win32 apps, elevated apps) do not load in-proc TSF services, or the service
  may not appear until the next sign-in. Use hook mode for those apps.
- **DLL moved:** the registration points at the old DLL path; re-register
  from the new location (`--unregister` first if needed).
- **DLL not loadable:** the TSF host requires the DLL (and its dependencies)
  to be x64 if the app is x64. The build produces x64 binaries.

## Translations never commit / text jumps back

- The commit is version-guarded: if you keep typing while the translation is
  in flight, the result for the old text is discarded (latest-wins). Pause
  briefly after the sentence, or end with `.` `!` `?` to force an immediate
  submit.
- If the text reverts to what you typed, the translation failed or was marked
  stale — check the diagnostics window and the log.

## API key problems (Gemini provider)

- The key is read from Settings or the `GEMINI_API_KEY` environment variable.
- The provider status in the Diagnostics window shows the last error
  (network, HTTP status, JSON parse).
- Keys are redacted in logs; if the Diagnostics window shows
  `REDACTED`/empty, the key is present but requests fail — check the
  endpoint/model in Settings.

## Notepad integration test issues

- The test steals keyboard focus; don't type while it runs, and close other
  windows with Notepad documents it might close (it only closes windows whose
  titles match its markers).
- If it reports "could not focus notepad", re-run after moving focus away and
  back; Win11 focus rules occasionally reject programmatic focus.
- If the document contains leftover debris, the test clears the target
  document before typing (Ctrl+A, Backspace) — debris in the *wrong* window
  is harmless.

## Crash reports

`aiie_unit_tests.exe` prints `CRASH: code=... at ... in <module> +0x...` and
continues. Resolve the RVA with the matching PDB (see Development.md). If a
crash is intermittent, it is likely a test-lifecycle race: every test must
wait for all in-flight provider callbacks before returning.

## Logs

- Log file: `%APPDATA%\AIInputEngine\aiie.log` (UTF-8, time-stamped, ring of
  the last 512 entries).
- The Diagnostics window shows the in-memory ring without touching the file.