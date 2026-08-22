# Runs the unit tests and the TSF registration verification. The Notepad
# integration test is opt-in (-WithIntegration) because it opens windows and
# steals keyboard focus while it runs.
param(
    [ValidateSet("Release", "Debug")][string]$Config = "Release",
    [switch]$WithIntegration,
    [switch]$RequireTsfRegistered
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path (Join-Path $root "build") $Config

Write-Host "== Unit tests ==" -ForegroundColor Cyan
& (Join-Path $bin "aiie_unit_tests.exe")
if ($LASTEXITCODE -ne 0) { Write-Host "Unit tests FAILED" -ForegroundColor Red; exit $LASTEXITCODE }

Write-Host "`n== TSF registration verify ==" -ForegroundColor Cyan
$verifyOut = & (Join-Path $bin "aiie_tsfreg.exe") --verify 2>&1
$verifyOut | ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -ne 0) {
    if ($RequireTsfRegistered) {
        Write-Host "tsfreg verify FAILED (registration required)" -ForegroundColor Red
        exit $LASTEXITCODE
    }
    Write-Host "TSF service is not registered - expected before setup. Run:" -ForegroundColor DarkGray
    $tsfreg = Join-Path $bin "aiie_tsfreg.exe"
    Write-Host "  & '$tsfreg' --register" -ForegroundColor DarkGray
}

Write-Host "`n== TSF host proof harness (managed store) ==" -ForegroundColor Cyan
& (Join-Path $bin "aiie_tsfhost.exe") --mock
if ($LASTEXITCODE -ne 0) { Write-Host "TSF host harness FAILED" -ForegroundColor Red; exit $LASTEXITCODE }

Write-Host "`n== Notepad integration test ==" -ForegroundColor Cyan
if ($WithIntegration) {
    & (Join-Path $bin "aiie_integration_test.exe")
    if ($LASTEXITCODE -ne 0) { Write-Host "Integration test FAILED" -ForegroundColor Red; exit $LASTEXITCODE }
} else {
    Write-Host "Skipped (pass -WithIntegration to run; it steals focus and opens windows)." -ForegroundColor DarkGray
}

Write-Host "`nAll tests passed." -ForegroundColor Green