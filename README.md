# AI Input Engine

![AI Input Engine](resources/infographic.jfif)

AI Input Engine is a Windows-native input method that transparently transforms
text as you type in real time, before it is committed to the target application.

```
Physical Keyboard → Windows Input → AI Input Engine → Target Application
```

Type in any Windows application (Notepad, Word, browsers, VS Code, Discord, Slack, Teams...) and AI Input Engine automatically transforms the text in-place:
1. **Multilingual Live Translation**: Translate across languages (English, Spanish, Chinese Simplified, Chinese Traditional, French, German, Japanese, Portuguese, Italian).
2. **Same-Language Correction & Rewrite**: Clean up typos, missing accents, grammar errors, and style while preserving meaning and voice in the same language.
3. **Smart Paste (`Ctrl+Shift+V`)**: Instantly translate or rewrite text directly from the clipboard, synthetically paste into the active window, and seamlessly restore the user's original clipboard content.

## Architecture

```mermaid
flowchart TD
    subgraph Trigger["1. Input & Trigger Sources"]
        direction TB
        K["Keystroke Typed<br/><i>(Global Hook / TSF)</i>"] --> IE["InputEngine<br/><i>(Buffer & Composition)</i>"]
        SP["Smart Paste Hotkey<br/><i>(Ctrl+Shift+V)</i>"] --> SPM["SmartPasteManager<br/><i>(Clipboard Snapshot)</i>"]
    end

    IE -->|"Dispatch Request"| TR["TranslationRequest"]
    SPM -->|"Dispatch Request"| TR

    subgraph Pipeline["2. Pipeline & Provider Execution"]
        direction TB
        TR --> TP["TranslationPipeline<br/><i>(Exact Cache & Monotonic ID)</i>"]
        TP -->|"Cache Miss"| PROV["OpenAI / Gemini Provider"]
        
        subgraph Worker["Worker Thread (SSE Streaming Loop)"]
            direction TB
            PROV --> SSE["• 'stream': true & SSE Parser<br/>• Reasoning effort & Delta accumulation<br/>• TTFT (Time-To-First-Token) measurement<br/>• Instant cancellation on obsolete request<br/>• Terminal completion validation"]
        end
        
        SSE -->|"Single Final Callback"| TRES["TranslationResult"]
        TRES --> TP_RESP["TranslationPipeline<br/><i>(Drop Stale / Cache Store)</i>"]
    end

    subgraph Output["3. Safe Output & Target Execution"]
        direction TB
        TP_RESP -->|"Buffer Version Guard"| IE_OUT["InputEngine<br/><b>CommitReplacement()</b><br/><i>(Atomic In-Place Replacement)</i>"]
        TP_RESP -->|"Target PID/HWND Guard"| SPM_OUT["SmartPasteManager<br/><b>Safe Synthetic Paste</b><br/><i>(Paste & Clipboard Restore)</i>"]
    end

    style Trigger fill:#1e293b,stroke:#3b82f6,stroke-width:2px,color:#f8fafc
    style Pipeline fill:#0f172a,stroke:#6366f1,stroke-width:2px,color:#f8fafc
    style Worker fill:#1e1b4b,stroke:#818cf8,stroke-width:1px,stroke-dasharray: 5 5,color:#f8fafc
    style Output fill:#1e293b,stroke:#10b981,stroke-width:2px,color:#f8fafc
```

## Capabilities & Architecture

- **Transform Operations**: Authoritative `Translate` vs `CorrectAndRewrite`.
- **Writing Profiles**: `Neutral` (default), `Professional`, `Casual`, `Slang`, `Custom`.
- **Supported Providers**: `Mock` (deterministic local engine), `OpenAI` (SSE Streaming Responses API with structured system instructions and reasoning effort control), `Google Gemini` (`gemini-3.7-flash`, `gemini-3.6-flash`, `gemini-3.5-flash-lite`).
- **Language Registry**: First-class support for `zh-Hans` (Chinese Simplified), `zh-Hant` (Chinese Traditional), `en`, `es`, `fr`, `de`, `ja`, `pt`, `it`, and `auto` (for translation source).
- **Security & Privacy**: API keys are securely stored in Windows Credential Manager (DPAPI); never logged or stored in plaintext.
- **Safety & Anti-Conversational Guarantees**: Strict instruction guardrails ensure the AI only translates or rewrites and never answers questions or injects conversational filler. Preserves protected tokens (URLs, emails, mentions, hashtags, code).
- **Canonical O(1) LRU Cache with Lazy TTL & Payload Guards**: In-memory high-throughput translation cache backed by `std::list` pointer splicing and `std::unordered_map` indexing ($O(1)$ lookup, hit-recency refresh, and eviction). Features zero-overhead lazy TTL expiration (default 30 min, configurable) protecting sensitive text from prolonged RAM residency, bidirectional payload bounding (`maxTextLength` up to 4,000 chars) preventing memory bloat from massive Smart Paste documents, and strict isolation keys across provider, model, operation, languages, profile, custom instructions, and text.
- **Smart Paste Heuristics & Key Recovery**: High-fidelity content analysis distinguishing code snippets and secrets from natural prose containing programming symbols (`==`, `!=`, `->`, `++`). Synthetic paste uses full modifier state recovery to prevent stuck Ctrl or Shift keys.
- **Speculative Translation (PCHP)**: Pre-Commit Hesitation Prediction preemptively streams translations during natural typing pauses (canonical gates: $\ge 450\text{ ms}$ hesitation, $\ge 10\text{ chars}$ buffer, $\ge 8\text{ chars}$ growth, $\ge 750\text{ ms}$ cooldown), achieving near 0-ms commit latency while guaranteeing single physical concurrency and candidate ready suppression.
- **Resilient Composition & Replacement**: Serialized commit thread lifecycle with clean shutdown joining; dual-layer target foreground verification protecting active applications during task switching; collision guards suppressing physical typing collisions during human-paced text injection; adaptive paste synchronization wait (up to 250ms) for heavy desktop applications (Electron, Teams, Word); single-session UI Automation caching.
- **High-Throughput Streaming & Zero-Allocation Net I/O**: Robust SSE streaming parser handling fragmented network chunks without line drops; stack-allocated HTTP buffers eliminating per-chunk heap allocations; cancelable WinHTTP request pipes.

## Components

| Component | Purpose |
|---|---|
| `AIInputEngine.exe` | System tray host, hook-based input engine, Smart Paste manager, modern settings & diagnostics UI |
| `AIInputEngineTSF.dll` | Text Services Framework (TSF) text service (native IME path) |
| `aiie_unit_tests.exe` | Comprehensive unit & simulation test suite (333 deterministic tests, 6,408 checks, 0 failures) |
| `aiie_openaitest.exe` | Live OpenAI connectivity, translation, streaming, and rewrite validation tool |
| `aiie_geminitest.exe` | Live Gemini diagnostic tool |
| `aiie_tsfhost.exe` | TSF host proof tool verifying composition -> transformation -> commit |
| `aiie_tsfreg.exe` | Per-user TSF registration / unregistration / verification tool |
| `aiie_integration_test.exe` | Automated Notepad hook integration test |

## Quick Start

Requirements: Windows 10/11 x64, Visual Studio 2026 (or 2022) Build Tools with "Desktop development with C++", CMake 3.24+.

```powershell
# Build in Release mode
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release --parallel

# Run unit tests
.\build\Release\aiie_unit_tests.exe

# Run live OpenAI smoke test matrix
.\build\Release\aiie_openaitest.exe --matrix

# Create portable distribution package
.\tools\package_dist.ps1
```

## Hotkeys (Configurable)

| Action | Default |
|---|---|
| Toggle AI Input | `Ctrl+Shift+Space` |
| Smart Paste | `Ctrl+Shift+V` |
| Swap Source ↔ Target | `Ctrl+Shift+S` |
| Diagnostics Window | `Ctrl+Shift+D` |

## License

MIT — see [LICENSE](LICENSE).