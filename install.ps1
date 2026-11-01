<#
.SYNOPSIS
  Installs HW Widget: copies it to Program Files, starts it at every logon with administrator
  rights (needed to read the CPU temperature, without a UAC prompt) and adds a Start menu shortcut.
.PARAMETER Source
  Folder with the built HwWidget.exe (build.ps1 output).
.PARAMETER InstallPawnIO
  Also install the PawnIO driver through winget when it is missing; the CPU temperature needs it.
.PARAMETER LogPath
  Optional transcript file.
#>
param(
    [string]$Source = (Join-Path $PSScriptRoot 'bin'),
    [switch]$InstallPawnIO,
    [string]$LogPath
)

$ErrorActionPreference = 'Stop'

$identity = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $identity.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Source', "`"$Source`"")
    if ($InstallPawnIO) { $argList += '-InstallPawnIO' }
    if ($LogPath) { $argList += @('-LogPath', "`"$LogPath`"") }
    $elevated = Start-Process powershell.exe -Verb RunAs -ArgumentList $argList -Wait -PassThru
    exit $elevated.ExitCode
}

if ($LogPath) { Start-Transcript -Path $LogPath -Force | Out-Null }
try {
    $taskName = 'HwWidget'
    $dest = Join-Path $env:ProgramFiles 'HwWidget'
    $exe = Join-Path $dest 'HwWidget.exe'
    $user = "$env:USERDOMAIN\$env:USERNAME"
    if (-not (Test-Path (Join-Path $Source 'HwWidget.exe'))) { throw "HwWidget.exe not found in $Source" }

    # 1. PawnIO driver (CPU temperature)
    $pawnKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PawnIO'
    if ($InstallPawnIO -and -not (Test-Path $pawnKey)) {
        Write-Host 'Installing PawnIO...'
        winget install --id namazso.PawnIO --exact --silent --accept-package-agreements --accept-source-agreements --disable-interactivity
        if (-not (Test-Path $pawnKey)) { throw "PawnIO was not installed (winget exit code $LASTEXITCODE)" }
    }

    # 2. Stop a running copy and replace the files
    Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue | Stop-ScheduledTask
    Get-Process HwWidget -ErrorAction SilentlyContinue | Stop-Process -Force
    Wait-Process -Name HwWidget -Timeout 5 -ErrorAction SilentlyContinue
    if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
    New-Item $dest -ItemType Directory | Out-Null
    Copy-Item (Join-Path $Source '*') $dest -Recurse
    Copy-Item (Join-Path $PSScriptRoot 'uninstall.ps1') $dest

    # 3. Logon task: elevated without a UAC prompt, no time limit, keeps running on battery
    $action = New-ScheduledTaskAction -Execute $exe -Argument '--from-task' -WorkingDirectory $dest
    $trigger = New-ScheduledTaskTrigger -AtLogOn -User $user
    $trigger.Delay = 'PT3S'
    $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
        -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew
    Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger -Principal $principal -Settings $settings `
        -Description 'HW Widget: CPU/GPU temperature and RAM/VRAM load on the desktop.' -Force | Out-Null

    # 4. Start menu shortcut; the exe hands itself over to the task, so no UAC prompt there either
    $shortcut = (New-Object -ComObject WScript.Shell).CreateShortcut(
        (Join-Path ([Environment]::GetFolderPath('Programs')) 'HW Widget.lnk'))
    $shortcut.TargetPath = $exe
    $shortcut.WorkingDirectory = $dest
    $shortcut.Description = 'CPU/GPU temperature and RAM/VRAM load'
    $shortcut.Save()

    Start-ScheduledTask -TaskName $taskName
    Write-Host "Installed to $dest"
}
catch {
    Write-Host "ERROR: $_"
    exit 1
}
finally {
    if ($LogPath) { Stop-Transcript | Out-Null }
}
