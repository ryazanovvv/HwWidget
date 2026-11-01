<#
.SYNOPSIS
  Builds bin\HwWidget.exe from src\ with Zig's bundled clang and MinGW-w64 headers:
  no Visual Studio or Windows SDK needed.
.PARAMETER Zig
  Path to zig.exe; by default the one under %LOCALAPPDATA%\Programs\zig, then PATH.
#>
param([string]$Zig)

$ErrorActionPreference = 'Stop'
if (-not $Zig) {
    $Zig = Get-ChildItem (Join-Path $env:LOCALAPPDATA 'Programs\zig') -Filter zig.exe -Recurse -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $Zig) { $Zig = (Get-Command zig.exe -ErrorAction SilentlyContinue).Source }
if (-not $Zig) { throw 'zig.exe not found: install Zig or pass -Zig <path>' }

$bin = Join-Path $PSScriptRoot 'bin'
New-Item $bin -ItemType Directory -Force | Out-Null
Push-Location (Join-Path $PSScriptRoot 'src') # hwwidget.rc names its files relative to src
try {
    & $Zig build-exe -target x86_64-windows-gnu -O ReleaseSmall -fstrip -lc --subsystem windows `
        -cflags -std=gnu11 -Wall -Wextra -Wno-missing-field-initializers -- main.c render.c sensors.c desktop.c settings.c hwwidget.rc `
        -luser32 -lgdi32 -ladvapi32 -ldwmapi -limm32 `
        --cache-dir (Join-Path $PSScriptRoot 'obj') --name HwWidget "-femit-bin=$(Join-Path $bin 'HwWidget.exe')"
    if ($LASTEXITCODE) { throw "zig build-exe failed with exit code $LASTEXITCODE" }
}
finally {
    Pop-Location
}
Get-Item (Join-Path $bin 'HwWidget.exe') | Select-Object Name, Length
