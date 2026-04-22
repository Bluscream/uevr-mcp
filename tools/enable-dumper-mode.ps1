<#
.SYNOPSIS
    Enable UEVR dumper mode for a specific game by writing a sentinel file.

.DESCRIPTION
    Dumper mode tells UEVRBackend.dll to skip the render-pipeline hooks
    (FFakeStereoRenderingHook, D3D hooks, IXRTrackingSystemHook, VR mod
    init) while keeping the plugin loader + engine tick hook + UObject
    reflection. Used when you only need reflection dumps — no VR.

    Activation happens via a zero-byte sentinel file at:
      %APPDATA%\UnrealVRMod\<GameExeStem>\dumper_mode

    UEVR checks for this once at process start (cached for the session).

.PARAMETER GameExe
    Path to the game exe. Used to compute the UnrealVRMod folder name.

.PARAMETER GameName
    Alternative to GameExe — pass the UnrealVRMod folder name directly
    (e.g. "RoboQuest-Win64-Shipping"). Useful when the exe isn't on this
    machine yet.

.EXAMPLE
    .\tools\enable-dumper-mode.ps1 -GameExe "D:\Steam\MyGame\MyGame-Win64-Shipping.exe"

.EXAMPLE
    .\tools\enable-dumper-mode.ps1 -GameName "HogwartsLegacy"
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
    if (-not (Test-Path $GameExe)) {
        Write-Warning "$GameExe not found — continuing with derived name anyway."
    }
    $GameName = [System.IO.Path]::GetFileNameWithoutExtension($GameExe)
}

$UnrealVRModDir = Join-Path $env:APPDATA 'UnrealVRMod'
$GameDir        = Join-Path $UnrealVRModDir $GameName
$Sentinel       = Join-Path $GameDir 'dumper_mode'

New-Item -ItemType Directory -Path $GameDir -Force | Out-Null
New-Item -ItemType File      -Path $Sentinel -Force | Out-Null

Write-Host "Dumper mode ENABLED for $GameName" -ForegroundColor Green
Write-Host "Sentinel: $Sentinel"
Write-Host ""
Write-Host "Next injection of UEVR will skip render hooks. Disable with:"
Write-Host "  tools\disable-dumper-mode.ps1 -GameName $GameName"
