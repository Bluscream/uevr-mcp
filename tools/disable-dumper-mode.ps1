<#
.SYNOPSIS
    Disable UEVR dumper mode for a specific game.

.DESCRIPTION
    Removes the dumper_mode sentinel file. Next UEVR injection will go
    back to normal VR-mode behaviour (render hooks active, stereo hook,
    IXRTrackingSystemHook, etc.).

.PARAMETER GameExe
    Path to the game exe.

.PARAMETER GameName
    Alternative: pass the UnrealVRMod folder name directly.

.EXAMPLE
    .\tools\disable-dumper-mode.ps1 -GameName "RoboQuest-Win64-Shipping"
#>
[CmdletBinding(DefaultParameterSetName = 'Exe')]
param(
    [Parameter(Mandatory, ParameterSetName='Exe')]
    [string]$GameExe,

    [Parameter(Mandatory, ParameterSetName='Name')]
    [string]$GameName
)

$ErrorActionPreference = 'Stop'

if ($PSCmdlet.ParameterSetName -eq 'Exe') {
    $GameName = [System.IO.Path]::GetFileNameWithoutExtension($GameExe)
}

$Sentinel = Join-Path $env:APPDATA "UnrealVRMod\$GameName\dumper_mode"

if (Test-Path $Sentinel) {
    Remove-Item -Force $Sentinel
    Write-Host "Dumper mode DISABLED for $GameName" -ForegroundColor Green
} else {
    Write-Host "Dumper mode was already disabled for $GameName (no sentinel at $Sentinel)"
}
