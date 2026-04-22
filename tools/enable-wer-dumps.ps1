<#
.SYNOPSIS
    Register a per-exe Windows Error Reporting full-memory crash dump.

.DESCRIPTION
    Configures HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\
    LocalDumps\<exeName> so if the game crashes, Windows writes a full
    process dump to C:\dumps\wer\<exeName>.<pid>.dmp that you can open
    in WinDbg / Visual Studio.

    Useful for diagnosing UEVR + plugin crashes. UEVR installs its own
    SetUnhandledExceptionFilter (crash.dmp in the UnrealVRMod folder)
    but it misses some crash modes — the WER entry catches anything
    that reaches the OS.

    Requires admin rights.

.PARAMETER GameExe
    Path to the game exe.

.PARAMETER ExeName
    Alternative: just the basename (e.g. "RoboQuest-Win64-Shipping.exe").

.PARAMETER DumpDir
    Where dumps land. Default C:\dumps\wer.

.PARAMETER DumpCount
    How many dumps to retain (WER rotates). Default 10.

.PARAMETER DumpType
    1 = minidump, 2 = full memory dump (much larger but more useful).
    Default 2.

.PARAMETER NoElevateCheck
    Skip the "am I admin" check. Used by setup.ps1 when it already
    knows the caller is elevated.

.EXAMPLE
    .\tools\enable-wer-dumps.ps1 -GameExe "D:\Steam\MyGame\MyGame.exe"
#>
[CmdletBinding(DefaultParameterSetName = 'Exe')]
param(
    [Parameter(Mandatory, ParameterSetName='Exe')]
    [string]$GameExe,

    [Parameter(Mandatory, ParameterSetName='Name')]
    [string]$ExeName,

    [string]$DumpDir = 'C:\dumps\wer',
    [int]$DumpCount  = 10,
    [int]$DumpType   = 2,
    [switch]$NoElevateCheck
)

$ErrorActionPreference = 'Stop'

if ($PSCmdlet.ParameterSetName -eq 'Exe') {
    $ExeName = [System.IO.Path]::GetFileName($GameExe)
}

# Elevation check
if (-not $NoElevateCheck) {
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        Write-Host "Re-launching as admin to write to HKLM..." -ForegroundColor Yellow
        $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath)
        if ($GameExe)   { $argList += @('-GameExe', "`"$GameExe`"") }
        if ($ExeName)   { $argList += @('-ExeName', "`"$ExeName`"") }
        if ($DumpDir)   { $argList += @('-DumpDir', "`"$DumpDir`"") }
        $argList += '-NoElevateCheck'
        Start-Process -Verb RunAs -FilePath pwsh -ArgumentList $argList -Wait
        return
    }
}

New-Item -Path $DumpDir -ItemType Directory -Force | Out-Null
$RegPath = "HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\$ExeName"
if (-not (Test-Path $RegPath)) {
    New-Item -Path $RegPath -Force | Out-Null
}
New-ItemProperty -Path $RegPath -Name 'DumpFolder' -PropertyType ExpandString -Value $DumpDir -Force | Out-Null
New-ItemProperty -Path $RegPath -Name 'DumpType'   -PropertyType DWord        -Value $DumpType   -Force | Out-Null
New-ItemProperty -Path $RegPath -Name 'DumpCount'  -PropertyType DWord        -Value $DumpCount  -Force | Out-Null

Write-Host "WER dumps registered for $ExeName" -ForegroundColor Green
Write-Host "  Folder: $DumpDir"
Write-Host "  Type:   $DumpType (1=mini, 2=full)"
Write-Host "  Count:  $DumpCount"
