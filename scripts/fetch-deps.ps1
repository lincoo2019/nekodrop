# Fetches what the build needs into third_party\ (not in this repository):
#   winawdl         radio pipe, RTL8852CE radio, awdld (OWL), Wintun, AirDrop engine (MIT/GPL-2.0/GPL-3.0)
#   devourer        winawdl's Realtek RTL8852CE user-mode radio library (GPL-2.0)
#   ForbiddenItlwm  iwlwifi firmware and its licence (GPL-2.0 / Intel redistribution)
#   linux-firmware  the MediaTek MT7902/MT7921/MT7922 and Intel BE200 firmware and their licences
#   WirDrop         its Apple ID sign-in (appleid.exe, appleid-anisette.exe) for Contacts Only (MIT)
#   winlibs MinGW   GCC for awdld
#   WDK (NuGet)     UMDF headers and tools, only when no WDK is installed
# Pinned to the versions nekodrop is tested with; the downloads are checked by SHA-256.
$ErrorActionPreference = 'Stop'
$tp = Join-Path (Split-Path $PSScriptRoot -Parent) 'third_party'
New-Item -ItemType Directory -Force $tp | Out-Null

function Get-Repo($name, $url, $commit) {
  $dir = Join-Path $tp $name
  if (-not (Test-Path "$dir\.git")) { git clone --quiet $url $dir; if ($LASTEXITCODE) { throw "clone $url failed" } }
  git -C $dir fetch --quiet origin
  git -C $dir checkout --quiet $commit
  if ($LASTEXITCODE) { throw "$name`: commit $commit not found" }
  "$name @ $($commit.Substring(0, 7))"
}

function Get-File($url, $out, $sha256) {
  if (-not (Test-Path $out)) { Invoke-WebRequest $url -OutFile $out -UseBasicParsing }
  $got = (Get-FileHash $out -Algorithm SHA256).Hash
  if ($sha256 -and $got -ne $sha256) { Remove-Item $out; throw "$out`: SHA-256 $got, expected $sha256" }
}

Get-Repo winawdl https://github.com/raph559/winawdl.git 0db7031589dc2bca03c0893db341176376962047
Get-Repo 'winawdl\upstream\devourer' https://github.com/raph559/devourer.git 8d936de2a3805ce126e5236349afe40e645aaf9b
Get-Repo ForbiddenItlwm https://github.com/retropineaple/ForbiddenItlwm.git 96ff71e0211789239f6fbf6b64ac8e7fd22b41aa
Get-Repo WirDrop https://github.com/JungyoKim/WirDrop.git 7c5308ca7fb9ee3d375e4c4ca93f7f9d6a7542a6

# Bz-hr firmware for the AX203 on Arrow Lake (8086:7740), which ForbiddenItlwm
# does not carry: iwlwifi-bz-b0-hr-b0-100 (core 97, the newest for this RF; the
# file's "100" is the packaging version) from linux-firmware (Intel licence).
$fwDir = Join-Path $tp 'firmware'
New-Item -ItemType Directory -Force $fwDir | Out-Null
Get-File 'https://gitlab.com/kernel-firmware/linux-firmware/-/raw/main/intel/iwlwifi/iwlwifi-bz-b0-hr-b0-100.ucode' `
         "$fwDir\iwlwifi-bz-b0-hr-b0-100.ucode" '589ACC51DD2753BE24C0355AF8074A72A43DBABCBF2A34D709F7D8AAE4FFC2ED'
Get-File 'https://gitlab.com/kernel-firmware/linux-firmware/-/raw/main/intel/iwlwifi/iwlwifi-bz-b0-hr-b0.pnvm' `
         "$fwDir\iwlwifi-bz-b0-hr-b0.pnvm" '9C6BFCD07D6E15711EB8E5E7ABA83018999C2D65079FE38997641001C0A46EEB'
'iwlwifi-bz-b0-hr-b0-100.ucode + pnvm (Bz-hr, AX203)'

# MT7902 firmware: three files, not a clone of all of linux-firmware. From its
# GitLab copy: git.kernel.org answers scripts with a bot check page.
$lfw = 'a1ebb5ad7eae3caca60b8b3db5792cf43339ba58'
$mtFw = Join-Path $tp 'linux-firmware-mt7902'
New-Item -ItemType Directory -Force $mtFw | Out-Null
foreach ($f in @(@('mediatek/WIFI_RAM_CODE_MT7902_1.bin', 'B5958AC72C71FB8405E080F52D378D02B484812B9F1010C8843727056BBFC998'),
                 @('mediatek/WIFI_MT7902_patch_mcu_1_1_hdr.bin', 'D73BA9E982F781221A2B9F10C42031F20A9DCE046929FC0D55C791A417EFE30A'),
                 @('LICENSES/LICENCE.mediatek', 'A90D3F66704D85889945FEC5525EA77622549DA83ACED1AAC99828383F8F1805'))) {
  Get-File "https://gitlab.com/kernel-firmware/linux-firmware/-/raw/$lfw/$($f[0])" `
           (Join-Path $mtFw (Split-Path $f[0] -Leaf)) $f[1]
}
"linux-firmware (MT7902) @ $($lfw.Substring(0, 7))"

# MT7922/MT7921 firmware from the same pinned linux-firmware commit.
$mtkFw = Join-Path $tp 'linux-firmware-mt7922'
New-Item -ItemType Directory -Force $mtkFw | Out-Null
foreach ($f in @(@('mediatek/WIFI_MT7922_patch_mcu_1_1_hdr.bin', '5356256C3A361CE6A06165F67CB01B55AECAAC0E2F8D2D9CBDE6312C4CF1A91C'),
                 @('mediatek/WIFI_RAM_CODE_MT7922_1.bin', 'AE4DAB06CC1849682E254FE97183ED093C51382BACA9B3F8A00FD6809AB71884'),
                 @('mediatek/WIFI_MT7961_patch_mcu_1_2_hdr.bin', 'A276C06C2B772ADB50B86639D33C82824FF4C21D617FEB78CAEA74C040B873F6'),
                 @('mediatek/WIFI_RAM_CODE_MT7961_1.bin', 'B94217A951518A9C14095765F367BC5DD7698F2DC033941D6F18FC2EBD6A2AB9'),
                 @('LICENSES/LICENCE.mediatek', 'A90D3F66704D85889945FEC5525EA77622549DA83ACED1AAC99828383F8F1805'))) {
  Get-File "https://gitlab.com/kernel-firmware/linux-firmware/-/raw/$lfw/$($f[0])" `
           (Join-Path $mtkFw (Split-Path $f[0] -Leaf)) $f[1]
}
"linux-firmware (MT7922/MT7921) @ $($lfw.Substring(0, 7))"

# Experimental Intel BE200 (GL/FM) API 100 firmware (embeds PNVM in TLV 74).
$be200Fw = Join-Path $tp 'linux-firmware-iwlwifi-be200'
New-Item -ItemType Directory -Force $be200Fw | Out-Null
foreach ($f in @(@('intel/iwlwifi/iwlwifi-gl-c0-fm-c0-100.ucode', 'F8CC54927AE551025A7550CF2117BDDA30DC1F392EE925E5C60D80A9CDB0179B'),
                 @('LICENSES/LICENCE.iwlwifi_firmware', '16D5040C7CF851FC693B7542E20870935B99802533EA1BFD231C377A2305E5C5'))) {
  Get-File "https://gitlab.com/kernel-firmware/linux-firmware/-/raw/$lfw/$($f[0])" `
           (Join-Path $be200Fw (Split-Path $f[0] -Leaf)) $f[1]
}
"linux-firmware (Intel BE200) @ $($lfw.Substring(0, 7))"

# Wintun for awdld: winawdl's script checks its SHA-256 and WireGuard's signature.
if (-not (Test-Path "$tp\winawdl\deps\wintun\wintun.dll")) {
  & "$tp\winawdl\scripts\fetch-wintun.ps1"
}

$mingw = Join-Path $tp 'winlibs'
if (-not (Test-Path "$mingw\mingw64\bin\gcc.exe")) {
  New-Item -ItemType Directory -Force $mingw | Out-Null
  Get-File 'https://github.com/brechtsanders/winlibs_mingw/releases/download/16.2.0posix-23.1.3-14.0.0-ucrt-r3/winlibs-x86_64-posix-seh-gcc-16.2.0-mingw-w64ucrt-14.0.0-r3.zip' `
           "$mingw\mingw.zip" '3C69305081E4A647144339A936244FCF7D3D9E558319E1AD7BE2CD2886D4B6A1'
  Expand-Archive "$mingw\mingw.zip" $mingw -Force
}
'winlibs MinGW (GCC 16.2.0)'

$kit = "${env:ProgramFiles(x86)}\Windows Kits\10"
if (-not (Test-Path "$kit\Include\wdf\umdf")) {
  $wdk = Join-Path $tp 'wdk-nuget'
  if (-not (Test-Path "$wdk\wdk\c")) {
    New-Item -ItemType Directory -Force $wdk | Out-Null
    Get-File 'https://www.nuget.org/api/v2/package/Microsoft.Windows.WDK.x64/10.0.26100.6584' "$wdk\wdk.nupkg" $null
    Copy-Item "$wdk\wdk.nupkg" "$wdk\wdk.zip"
    Expand-Archive "$wdk\wdk.zip" "$wdk\wdk" -Force
    Remove-Item "$wdk\wdk.zip"
  }
  'WDK 10.0.26100.6584 (NuGet)'
} else { 'WDK: installed' }
