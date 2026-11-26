# Builds winawdl's AWDL daemon (awdld.exe, OWL on Windows) with MinGW-w64 GCC,
# into third_party\winawdl\build\awdl. OWL is GNU C, which MSVC does not take.
# -MinGW is the bin directory of a MinGW-w64 GCC (WinLibs or Strawberry).
param([string]$MinGW = '')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\toolchain.ps1"
$repo = Split-Path $PSScriptRoot -Parent
$winawdl = "$repo\third_party\winawdl"
$tools = Initialize-Toolchain

if (-not $MinGW) {
  $MinGW = @('E:\mingw64\bin', 'C:\Strawberry\c\bin') | Where-Object { Test-Path "$_\gcc.exe" } |
           Select-Object -First 1
}
if (-not $MinGW -or -not (Test-Path "$MinGW\gcc.exe")) { throw 'MinGW-w64 GCC not found; pass -MinGW <bin dir>.' }
if (-not (Test-Path "$winawdl\upstream\owl\radiotap\radiotap.c")) {
  git -C "$winawdl" submodule update --init upstream/owl
  git -C "$winawdl\upstream\owl" submodule update --init radiotap
}
& python -I "$PSScriptRoot\patch-awdl.py" $winawdl
if ($LASTEXITCODE -ne 0) { throw 'awdld patch failed' }
$env:PATH = "$MinGW;$env:PATH"
$build = "$winawdl\build\awdl"
$make = if (Test-Path "$MinGW\mingw32-make.exe") { "$MinGW\mingw32-make.exe" } else { "$MinGW\make.exe" }
& $tools.CMake -S "$winawdl\awdl" -B $build -G 'MinGW Makefiles' "-DCMAKE_C_COMPILER=$MinGW\gcc.exe" `
  "-DCMAKE_MAKE_PROGRAM=$make" -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }
& $tools.CMake --build $build
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

$wintun = "$winawdl\deps\wintun\wintun.dll"
if (-not (Test-Path $wintun)) {
  "Wintun is missing: run third_party\winawdl\scripts\fetch-wintun.ps1 (checks WireGuard's signature)."
} else {
  Copy-Item $wintun $build -Force
}
Get-ChildItem $build -Filter *.exe | Select-Object Name, Length
