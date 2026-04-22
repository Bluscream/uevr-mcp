<#
.SYNOPSIS
    One-time setup for uevr-mcp: verify deps, build plugin + MCP server,
    optionally register WER crash dumps.

.DESCRIPTION
    Idempotent. Safe to re-run. Does the following:
      1. Verifies .NET 9 SDK, CMake, Visual Studio build tools present
      2. Configures + builds the uevr_mcp plugin (plugin/build/Release/uevr_mcp.dll)
      3. Builds the MCP server (mcp-server/bin/Release/net9.0/UevrMcpServer.exe)
      4. Optionally registers per-exe WER dumps for the target game

    After this you can run tools\enable-dumper-mode.ps1 + tools\quick-dump.ps1
    against any UE4/UE5 game that UEVR supports.

.PARAMETER GameExe
    Optional game exe path. If set, also writes a dumper_mode sentinel
    and registers a per-exe WER LocalDumps entry for that exe.

.PARAMETER SkipPluginBuild
    Skip the plugin rebuild (use the existing plugin/build output).

.PARAMETER SkipServerBuild
    Skip the MCP server rebuild.

.EXAMPLE
    .\tools\setup.ps1

.EXAMPLE
    .\tools\setup.ps1 -GameExe "E:\SteamLibrary\steamapps\common\RoboQuest\RoboQuest\Binaries\Win64\RoboQuest-Win64-Shipping.exe"
#>
[CmdletBinding()]
param(
    [string]$GameExe,
    [switch]$SkipPluginBuild,
    [switch]$SkipServerBuild
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot

Write-Host "=== uevr-mcp setup ===" -ForegroundColor Cyan
Write-Host "Repo: $RepoRoot"
Write-Host ""

# 1. Verify deps
Write-Host "--- Verifying dependencies ---" -ForegroundColor Yellow
function Assert-Command {
    param([string]$Name, [string]$InstallHint)
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "$Name not found. $InstallHint"
    }
    Write-Host "  $Name ... found" -ForegroundColor Green
}
Assert-Command dotnet 'Install .NET 9 SDK from https://dotnet.microsoft.com/download/dotnet/9.0'
Assert-Command cmake  'Install CMake 3.21+ from https://cmake.org/download/ or via Visual Studio 2022'
# MSBuild is found implicitly by cmake's VS generator; we don't require it on PATH

$dotnetVer = (dotnet --version).Trim()
Write-Host "  .NET SDK $dotnetVer"

# 2. Build plugin
if (-not $SkipPluginBuild) {
    Write-Host ""
    Write-Host "--- Building plugin (uevr_mcp.dll) ---" -ForegroundColor Yellow
    $PluginDir  = Join-Path $RepoRoot 'plugin'
    $BuildDir   = Join-Path $PluginDir 'build'

    if (-not (Test-Path (Join-Path $BuildDir 'CMakeCache.txt'))) {
        Write-Host "  configuring..."
        & cmake -S $PluginDir -B $BuildDir 2>&1 | Select-Object -Last 3 | ForEach-Object { "    $_" } | Write-Host
    }
    Write-Host "  building..."
    & cmake --build $BuildDir --config Release --target uevr_mcp 2>&1 | Select-Object -Last 3 | ForEach-Object { "    $_" } | Write-Host

    $Dll = Join-Path $BuildDir 'Release\uevr_mcp.dll'
    if (Test-Path $Dll) {
        $size = [int]((Get-Item $Dll).Length / 1MB)
        Write-Host "  OK: $Dll ($size MB)" -ForegroundColor Green
    } else {
        throw "Build appeared to succeed but DLL not at $Dll"
    }
} else {
    Write-Host "--- Skipping plugin build (-SkipPluginBuild) ---" -ForegroundColor Yellow
}

# 3. Build MCP server
if (-not $SkipServerBuild) {
    Write-Host ""
    Write-Host "--- Building MCP server (UevrMcpServer.exe) ---" -ForegroundColor Yellow
    Push-Location (Join-Path $RepoRoot 'mcp-server')
    try {
        $out = dotnet build -c Release 2>&1
        # Match actual MSBuild errors (CSnnnn, MSBnnnn, NETSDKnnnn) — NOT the
        # "0 Error(s)" summary line which would false-positive on SimpleMatch.
        $errors = $out | Select-String -Pattern 'error\s+(CS|MSB|NETSDK|NU)\d+' | Select-Object -First 3
        if ($errors) {
            Write-Host "BUILD FAILED:" -ForegroundColor Red
            $errors | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
            throw "MCP server build failed"
        }
        $Exe = Join-Path (Get-Location) 'bin\Release\net9.0\UevrMcpServer.exe'
        if (Test-Path $Exe) {
            $size = [int]((Get-Item $Exe).Length / 1KB)
            Write-Host "  OK: $Exe ($size KB)" -ForegroundColor Green
        } else {
            throw "Build reported no errors but $Exe is missing"
        }
    } finally {
        Pop-Location
    }
} else {
    Write-Host "--- Skipping server build (-SkipServerBuild) ---" -ForegroundColor Yellow
}

# 4. Optional: configure dumper mode + WER for a game
if ($GameExe) {
    Write-Host ""
    Write-Host "--- Game-specific setup: $GameExe ---" -ForegroundColor Yellow
    if (-not (Test-Path $GameExe)) {
        Write-Host "  WARN: $GameExe not found; skipping" -ForegroundColor Yellow
    } else {
        # Dumper mode sentinel
        & (Join-Path $PSScriptRoot 'enable-dumper-mode.ps1') -GameExe $GameExe
        # WER dumps
        & (Join-Path $PSScriptRoot 'enable-wer-dumps.ps1') -GameExe $GameExe -NoElevateCheck
    }
}

Write-Host ""
Write-Host "=== Setup complete ===" -ForegroundColor Cyan
Write-Host "Next steps:"
Write-Host "  - tools\enable-dumper-mode.ps1 -GameExe <path>"
Write-Host "  - tools\quick-dump.ps1 -GameExe <path> -OutDir <dir>"
