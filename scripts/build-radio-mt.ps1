# Builds the MediaTek MT7902 user-mode radio daemon (nekodrop-radio-mt.exe,
# mtradio\, ported from Linux mt76) and its probe (mtprobe.exe) into
# build\mtradio, and copies linux-firmware's MT7902 blobs to build\mtradio\firmware.
# Links statically against the MSVC runtime (/MT); no other dependencies.
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\toolchain.ps1"

$repo = Split-Path $PSScriptRoot -Parent
$tools = Initialize-Toolchain
$src = Join-Path $repo 'mtradio'
$fwSource = Join-Path $repo 'third_party\linux-firmware-mt7902'
foreach ($f in 'WIFI_RAM_CODE_MT7902_1.bin', 'WIFI_MT7902_patch_mcu_1_1_hdr.bin', 'LICENCE.mediatek') {
  if (-not (Test-Path (Join-Path $fwSource $f))) { throw "$f missing in $fwSource`: run scripts\fetch-deps.ps1 first." }
}

$build = Join-Path $repo 'build\mtradio'
& $tools.CMake -S $src -B $build -G Ninja `
  "-DCMAKE_MAKE_PROGRAM=$($tools.Ninja)" `
  "-DCMAKE_CXX_COMPILER=$($tools.Cl)" `
  -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }

& $tools.CMake --build $build
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
& (Join-Path $build 'ccmp_test.exe')
if ($LASTEXITCODE -ne 0) { throw "ccmp_test failed ($LASTEXITCODE)" }

# The MediaTek Windows driver's copy of this firmware answers no unified (UNI)
# commands, so the daemon needs linux-firmware's.
$fwDest = Join-Path $build 'firmware'
New-Item -ItemType Directory -Force $fwDest | Out-Null
Copy-Item "$fwSource\*" $fwDest -Force

$exe = Join-Path $build 'nekodrop-radio-mt.exe'
if (-not (Test-Path $exe)) { throw "expected binary missing: $exe" }
Get-ChildItem $build -Filter *.exe | Select-Object Name, Length
