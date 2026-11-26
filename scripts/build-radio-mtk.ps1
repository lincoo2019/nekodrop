# Builds the MediaTek MT7922/MT7921 user-mode radio daemon (nekodrop-radio-mtk.exe,
# mtkradio\) into build\mtkradio, and copies linux-firmware's MT7922/MT7921 blobs
# to build\mtkradio\firmware.
# Links statically against the MSVC runtime (/MT); no other dependencies.
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\toolchain.ps1"

$repo = Split-Path $PSScriptRoot -Parent
$tools = Initialize-Toolchain
$src = Join-Path $repo 'mtkradio'
$fwSource = Join-Path $repo 'third_party\linux-firmware-mt7922'
foreach ($f in 'WIFI_MT7922_patch_mcu_1_1_hdr.bin', 'WIFI_RAM_CODE_MT7922_1.bin',
               'WIFI_MT7961_patch_mcu_1_2_hdr.bin', 'WIFI_RAM_CODE_MT7961_1.bin',
               'LICENCE.mediatek') {
  if (-not (Test-Path (Join-Path $fwSource $f))) { throw "$f missing in $fwSource`: run scripts\fetch-deps.ps1 first." }
}

$build = Join-Path $repo 'build\mtkradio'
& $tools.CMake -S $src -B $build -G Ninja `
  "-DCMAKE_MAKE_PROGRAM=$($tools.Ninja)" `
  "-DCMAKE_CXX_COMPILER=$($tools.Cl)" `
  -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }

& $tools.CMake --build $build
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
& (Join-Path $build 'ccmp_test.exe')
if ($LASTEXITCODE -ne 0) { throw "ccmp_test failed ($LASTEXITCODE)" }

$fwDest = Join-Path $build 'firmware'
New-Item -ItemType Directory -Force $fwDest | Out-Null
Copy-Item "$fwSource\*" $fwDest -Force

$exe = Join-Path $build 'nekodrop-radio-mtk.exe'
if (-not (Test-Path $exe)) { throw "expected binary missing: $exe" }
Get-ChildItem $build -Filter *.exe | Select-Object Name, Length
