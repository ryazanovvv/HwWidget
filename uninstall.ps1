<#
.SYNOPSIS
  Removes HW Widget: the logon task, the program folder, the Start menu shortcut and the saved settings.
  The PawnIO driver stays installed; remove it in Settings > Apps if nothing else uses it.
#>
$ErrorActionPreference = 'Stop'

$identity = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $identity.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Start-Process powershell.exe -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    exit
}

Set-Location $env:TEMP # the program folder, possibly this script's own, is about to go
Unregister-ScheduledTask -TaskName 'HwWidget' -Confirm:$false -ErrorAction SilentlyContinue
Get-Process HwWidget -ErrorAction SilentlyContinue | Stop-Process -Force
Wait-Process -Name HwWidget -Timeout 5 -ErrorAction SilentlyContinue
Remove-Item (Join-Path ([Environment]::GetFolderPath('Programs')) 'HW Widget.lnk') -ErrorAction SilentlyContinue
Remove-Item (Join-Path $env:LOCALAPPDATA 'HwWidget') -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $env:ProgramFiles 'HwWidget') -Recurse -Force -ErrorAction SilentlyContinue

Write-Host 'HW Widget removed. The PawnIO driver is still installed (Settings > Apps).'
Start-Sleep -Seconds 3
