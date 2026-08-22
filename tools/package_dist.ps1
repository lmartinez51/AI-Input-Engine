# packaging script for AIInputEngine portable distribution
param(
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $projectRoot "build\$Configuration"
$distDir = Join-Path $projectRoot "dist\AIInputEngine"

Write-Host "Packaging AIInputEngine ($Configuration)..." -ForegroundColor Cyan

if (-not (Test-Path "$buildDir\AIInputEngine.exe")) {
    Write-Error "AIInputEngine.exe not found at $buildDir. Please build the project first."
}

# Clean and recreate dist directory
if (Test-Path $distDir) {
    Remove-Item -Path $distDir -Recurse -Force
}
New-Item -Path $distDir -ItemType Directory -Force | Out-Null

# 1. Copy executable
Copy-Item -Path "$buildDir\AIInputEngine.exe" -Destination "$distDir\AIInputEngine.exe" -Force
Write-Host "Copied: AIInputEngine.exe" -ForegroundColor Green

# 2. Locate and copy Microsoft Visual C++ 2015-2022 x64 Runtime DLLs for portable execution
$redistDir = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Redist\MSVC\14.51.36231\x64\Microsoft.VC145.CRT"
if (Test-Path $redistDir) {
    $runtimeDlls = @("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll")
    foreach ($dll in $runtimeDlls) {
        $srcDll = Join-Path $redistDir $dll
        if (Test-Path $srcDll) {
            Copy-Item -Path $srcDll -Destination "$distDir\$dll" -Force
            Write-Host "Copied runtime DLL: $dll" -ForegroundColor Green
        }
    }
} else {
    Write-Warning "Redist directory not found: $redistDir"
}

# 3. Create tester README.txt
$readmeContent = @"
===============================================================================
                          AI INPUT ENGINE (v0.4.0)
     Windows-Native Live AI Translation & Same-Language Rewrite Engine
===============================================================================

Welcome to AI Input Engine! This portable package enables real-time AI-powered
text transformation inside ANY Windows application (Notepad, Word, Discord,
Slack, Teams, web browsers, etc.).

AI Input Engine supports two core operations:
1. Multilingual Live Translation (e.g. Spanish -> English, English -> Chinese Simplified)
2. Same-Language Correction & Rewrite (e.g. Fix grammar, punctuation, and style in Spanish)

-------------------------------------------------------------------------------
1. QUICK START
-------------------------------------------------------------------------------
1. Double-click 'AIInputEngine.exe' to start the application.
2. AI Input Engine runs as a background service and appears in your Windows
   System Tray (the notification area at the bottom-right of your taskbar,
   near the clock).
3. Look for the AI Input Engine icon (stylized keyboard with an AI starburst).
   (Note: If hidden, click the small '^' arrow on your taskbar).

-------------------------------------------------------------------------------
2. CONFIGURING YOUR AI PROVIDER & API KEY (FIRST-RUN SETUP)
-------------------------------------------------------------------------------
1. Right-click the System Tray icon and select 'Settings...'.
2. In the 'Operation' section:
   - Choose 'Translate' (for multilingual translation) or
     'Correct & Rewrite' (for same-language writing enhancement).
3. In the 'AI Provider & Credentials' section:
   - Select your preferred provider: 'OpenAI' or 'Google Gemini' (or 'Mock'
     for offline demo testing).
   - Select the model (e.g., 'gpt-5.6-luna' for OpenAI, 'gemini-3.6-flash'
     for Gemini).
   - Enter your personal API key in the 'API Key' field.
4. Click the '[ Save Key ]' button.
   - Your key is encrypted and stored securely in Windows Credential Manager.
   - API keys are NEVER saved in plaintext, NEVER written to config files,
     and NEVER exposed in logs.
5. (Optional) Click '[ Test Connection ]' to verify connectivity with the provider.
6. Click '[ Save & Apply ]' at the bottom of the dialog. Settings will be
   applied immediately without restarting.

-------------------------------------------------------------------------------
3. HOW TO USE
-------------------------------------------------------------------------------
A. TRANSLATE MODE:
   1. In Settings, choose Operation: 'Translate', Source: e.g. 'Spanish', Target: e.g. 'English'
      (Supported languages include English, Spanish, Chinese Simplified, Chinese Traditional, French, German, Japanese, Portuguese, Italian).
   2. Open any text application (e.g. Notepad, Word, chat).
   3. Type a sentence (e.g., "Hola mundo.").
   4. Upon typing sentence punctuation (., !, ?) or pausing, AI Input Engine
      transforms the text into your target language (e.g., "Hello world.").

B. CORRECT & REWRITE MODE:
   1. In Settings, choose Operation: 'Correct & Rewrite', Language: e.g. 'Spanish', Writing Profile: e.g. 'Neutral' or 'Professional'.
   2. Open any text application.
   3. Type informal or unedited text (e.g. "Ola ayer fui al super aver si comprava leche pero no avia.").
   4. The engine automatically corrects spelling, accents, and grammar in the SAME language
      (e.g., "Hola, ayer fui al súper a ver si compraba leche, pero no había.").

C. GLOBAL SHORTCUTS:
   - Toggle Engine On/Off: Ctrl + Shift + Space
   - Swap Languages (in Translate mode): Ctrl + Shift + S

-------------------------------------------------------------------------------
4. WRITING PROFILES & ADVANCED SETTINGS
-------------------------------------------------------------------------------
Open 'Settings...' to customize:
- Writing Profiles:
  * Neutral (Default): Natural, clean, and accurate correction/translation.
  * Professional: Formal corporate / business tone.
  * Casual: Friendly and relaxed tone for informal conversations.
  * Slang: Preserves colloquial / chat voice.
  * Custom: Allows custom instructions.
- Advanced Settings (Click 'Advanced Settings v'):
  * Debounce (ms): Wait time after typing before transforming (default: 500ms).
  * Min Characters: Minimum characters needed before triggering (default: 5).
  * Temperature / Top P / Top K / Max Tokens: Model parameter overrides.

-------------------------------------------------------------------------------
5. HOW TO EXIT
-------------------------------------------------------------------------------
Right-click the System Tray icon and select 'Exit'.

-------------------------------------------------------------------------------
6. REPORTING BUGS / FEEDBACK
-------------------------------------------------------------------------------
If you encounter any unexpected behavior, translation errors, UI glitches,
or crashes, please send a bug report with the following details:

=========================== BUG REPORT TEMPLATE ===========================
- Windows Version / Build : (Run 'winver' to check, e.g. Windows 11 23H2)
- Target Application     : (e.g. Notepad, Discord, MS Word, Chrome, etc.)
- Provider & Model       : (e.g. OpenAI - gpt-5.6-luna / Gemini - gemini-3.6-flash)
- Translation Profile    : (e.g. Neutral, Professional, Casual, Slang, Custom)
- Source / Target Lang   : (e.g. Spanish -> English)
- Exact Input Typed      : (The exact sentence or characters typed)
- Expected Output        : (What you expected to see)
- Actual Output          : (What actually appeared on screen)
- Reproducible?          : (Always / Sometimes / Only once)
- Log Snippet            : (Optional: Copy relevant lines from log file)
===========================================================================

Log File Location:
%LOCALAPPDATA%\AIInputEngine\logs\aiie.log
(Press Win+R, paste the line above, and press Enter).
Note: Logs never contain API keys or private credentials.
"@

$readmePath = Join-Path $distDir "README.txt"
[System.IO.File]::WriteAllText($readmePath, $readmeContent, [System.Text.Encoding]::UTF8)
Write-Host "Created: README.txt" -ForegroundColor Green

Write-Host "`nDistribution package created successfully at: $distDir" -ForegroundColor Cyan
Get-ChildItem $distDir | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize
