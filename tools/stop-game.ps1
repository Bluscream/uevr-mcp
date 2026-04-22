<#
.SYNOPSIS
    Cleanly close a running UE game that uevr-mcp injected into.

.PARAMETER GameExe
    Path to the game exe (used to match the running process).

.PARAMETER Force
    Use taskkill /F instead of graceful WM_CLOSE.

.EXAMPLE
    .\tools\stop-game.ps1 -GameExe "E:\...\RoboQuest-Win64-Shipping.exe"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$GameExe,

    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
$Mcp      = Join-Path $RepoRoot 'mcp-server\bin\Release\net9.0\UevrMcpServer.exe'

if (Test-Path $Mcp) {
    & $Mcp stop-game $GameExe
} else {
    $name = [System.IO.Path]::GetFileNameWithoutExtension($GameExe)
    $proc = Get-Process -Name $name -ErrorAction SilentlyContinue
    if (-not $proc) { Write-Host "Not running: $name"; return }
    if ($Force) {
        $proc | Stop-Process -Force
        Write-Host "Force-killed $name"
    } else {
        $proc | ForEach-Object { $_.CloseMainWindow() | Out-Null }
        Start-Sleep -Seconds 5
        Get-Process -Name $name -ErrorAction SilentlyContinue | Stop-Process -Force
        Write-Host "Closed $name"
    }
}
