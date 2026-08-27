# Troubleshooting

## No tray icon

- Run `AIInputEngine.exe` directly from a console and check for error output.
- Check that no other instance is running: the app is single-instance
  (named mutex `Local\AIInputEngine.SingleInstance`); if a previous instance
  is hung, kill it from Task Manager and retry.
- Check `%APPDATA%\AIInputEngine\aiie.log` for startup errors.

## Nothing is being translated

1. **Enabled?** Right-click the tray icon; "AI Input enabled" must be
   checked. Default hotkey to toggle: `Ctrl+Shift+Space`.
2. **Provider?** Open Settings (tray menu). "Mock (offline dictionary)" works
   without configuration. For OpenAI or Gemini, verify the API key is entered
   and that the selected model is supported.
3. **Language direction?** Check Settings for source/target languages, or swap
   with `Ctrl+Shift+S`.
4. **Buffer limits & Debounce?** A very short debounce or a low `minChars` can
   commit text before a translation is ready. Defaults: debounce 300 ms,
   minChars 2, maxBufferChars 120.
5. **Focus?** The hook path needs the target window to have active keyboard focus.
   The status popup shows "translating..." while a request is in flight.
6. **Diagnostics:** Press `Ctrl+Shift+D` — the diagnostics window displays the
   live log ring buffer, provider status, and PCHP speculative telemetry metrics.

## Smart Paste (`Ctrl+Shift+V`) issues

- **"Smart Paste: clipboard is empty or unsupported"**: The clipboard contains non-text
  data (e.g. image, file path, empty string) or is inaccessible. Copy valid Unicode text.
- **"Smart Paste cancelled: target window changed"**: If you switch windows or focus
  away while a Smart Paste translation is in flight, the paste is aborted to prevent
  accidental data injection into the wrong window.
- **"Smart Paste cancelled: clipboard was modified externally"**: If the clipboard
  sequence changed during the translation, the engine skips clipboard restoration to
  avoid overwriting newly copied user data.

## Hotkeys do nothing

- Another application may have registered the same hotkey first
  (`RegisterHotKey` fails in that case). Change the hotkeys in Settings.
- Hotkeys work only while `AIInputEngine.exe` is running.

## TSF mode problems

- **`aiie_tsfreg.exe --verify` fails:** The service is not registered for the
  current user. Run `--register`, then sign out and sign back in.
- **Service not offered in applications:** Some applications (e.g., older
  Win32 apps, elevated apps) do not load in-proc TSF services, or the service
  may not appear until the next sign-in. Use hook mode for those apps.
- **DLL moved:** The registration points at the old DLL path; re-register
  from the new location (`--unregister` first if needed).
- **DLL not loadable:** The TSF host requires the DLL to match the architecture (x64).

## Translations never commit / text jumps back

- The commit is version-guarded: if you keep typing while the translation is
  in flight, the result for the old text is discarded (latest-wins). Pause
  briefly after the sentence, or end with `.` `!` `?` to force an immediate
  submit.
- If the text reverts to what you typed, the translation failed or was marked
  stale — check the diagnostics window (`Ctrl+Shift+D`) and the log.

## API key & Provider problems (OpenAI / Gemini)

- API keys are securely stored in Windows Credential Manager under `AIInputEngine/<Provider>`.
- The provider status in the Diagnostics window (`Ctrl+Shift+D`) shows the latest
  connection state, HTTP status, and detailed error messages.
- Keys are automatically redacted in logs; if the Diagnostics window shows
  `REDACTED`, the key is loaded in memory.
- For OpenAI reasoning models (`o1`, `o3-mini`), temperature parameter is omitted
  automatically in compliance with API constraints.

## Notepad integration test issues

- The test steals keyboard focus; don't type while it runs, and close other
  Notepad windows that might conflict.
- If it reports "could not focus notepad", re-run after moving focus away and
  back; Win11 focus rules occasionally reject programmatic focus.

## Logs & Diagnostics

- Log file: `%APPDATA%\AIInputEngine\aiie.log` (UTF-8, time-stamped, ring buffer).
- The Diagnostics window (`Ctrl+Shift+D`) allows searching, filtering, and copying
  the active diagnostic snapshot to the clipboard with one click.