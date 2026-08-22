# Builds AI Input Engine with the Visual Studio generator (no PATH setup needed).
param(
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release",
    [string]$Generator = "Visual Studio 18 2026"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

cmake -S $root -B (Join-Path $root "build") -G $Generator -A x64
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build (Join-Path $root "build") --config $Config --parallel
exit $LASTEXITCODE