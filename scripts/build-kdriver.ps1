# Builds kdriver\ into build\kdriver\nekodrop-wlan.sys (KMDF 1.33, NetAdapterCx 2.4,
# WiFiCx 1.2) with MSVC and the WDK NuGet package. Not signed here.
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\toolchain.ps1"
$repo = Split-Path $PSScriptRoot -Parent
$tools = Initialize-Toolchain
$kit = 'C:\Program Files (x86)\Windows Kits\10'
$wdk = Join-Path $repo 'third_party\wdk-nuget\wdk\c'
$ver = Get-ChildItem "$wdk\Include" -Directory | Where-Object { Test-Path "$($_.FullName)\km\wificx" } |
       Sort-Object Name | Select-Object -Last 1 -ExpandProperty Name
$msvc = Split-Path (Split-Path (Split-Path (Split-Path $tools.Cl)))
$inc = @(
  (Join-Path $wdk "Include\$ver\km"),
  (Join-Path $wdk "Include\$ver\km\crt"),
  (Join-Path $wdk "Include\$ver\shared"),
  (Join-Path $kit "Include\$ver\shared"),
  (Join-Path $kit "Include\$ver\shared\netcx\shared\1.0"),
  (Join-Path $wdk 'Include\wdf\kmdf\1.33'),
  (Join-Path $wdk "Include\$ver\km\netcx\kmdf\adapter\2.4"),
  (Join-Path $wdk "Include\$ver\km\wificx\1.2"),
  (Join-Path $wdk "Include\$ver\km\wlan\2.0"),
  (Join-Path $msvc 'include')
)
$lib = @(
  (Join-Path $wdk "Lib\$ver\km\x64"),
  (Join-Path $wdk 'Lib\wdf\kmdf\x64\1.33'),
  (Join-Path $wdk "Lib\$ver\km\x64\netcx\kmdf\adapter\2.4"),
  (Join-Path $wdk "Lib\$ver\km\x64\wificx\1.2"),
  (Join-Path $wdk "Lib\$ver\km\x64\wlan\2.0"),
  (Join-Path $msvc 'lib\x64')
)
$env:INCLUDE = $inc -join ';'
$env:LIB = $lib -join ';'
$out = Join-Path $repo 'build\kdriver'
New-Item -ItemType Directory -Force $out | Out-Null
$defs = '/D_AMD64_', '/D_WIN64', '/DAMD64', '/DNTDDI_VERSION=0x0A000010', '/D_WIN32_WINNT=0x0A00',
        '/DKMDF_VERSION_MAJOR=1', '/DKMDF_VERSION_MINOR=33',
        '/DNET_ADAPTER_CX_2_4', '/DNETADAPTER_VERSION_MAJOR=2', '/DNETADAPTER_VERSION_MINOR=4', '/DNET_VERSION_MAJOR=2', '/DNET_VERSION_MINOR=4',
        '/DWIFI_VERSION_MAJOR=1', '/DWIFI_VERSION_MINOR=2', '/DPOOL_NX_OPTIN=1', '/DPOOL_ZERO_DOWN_LEVEL_SUPPORT'
Get-ChildItem $out -Filter *.obj | Remove-Item
$kdir = Join-Path $repo 'kdriver'
$shared = Join-Path $repo 'iwxradio\src'
$common = '/nologo', '/c', '/kernel', '/GS-', '/Gy', '/Zp8', '/W4', '/WX', '/wd4324', '/O2', '/std:c++17', '/EHs-c-',
          '/GR-', '/utf-8', '/Zc:threadSafeInit-'
# The WDF/WiFiCx glue: kernel headers only.
$src = Get-ChildItem $kdir -Filter *.cpp | Where-Object Name -notmatch '^k(plat|proxy|dma|hal)\.cpp$' | ForEach-Object FullName
& $tools.Cl @common /Zc:wchar_t- @defs /FIprecomp.h "/I$kdir" "/Fo$out\" @src
if ($LASTEXITCODE -ne 0) { throw "compile failed ($LASTEXITCODE)" }
# The hardware layer shared with the user-mode radio, with the C++ library
# in kernel mode (no exceptions, no RTTI, non-paged allocation in kplat.cpp).
$ucrt = Join-Path $kit "Include\$ver\ucrt"
$env:INCLUDE = (@((Join-Path $msvc 'include'), $ucrt) + $inc) -join ';'
$stl = '/DNEKO_KERNEL', '/D_HAS_EXCEPTIONS=0', '/D_HAS_STATIC_RTTI=0', '/D_ITERATOR_DEBUG_LEVEL=0',
       '/D_USE_STD_VECTOR_ALGORITHMS=0', '/D_CRT_SECURE_NO_WARNINGS', '/D_NO_CRT_STDIO_INLINE', '/DNOMINMAX', '/wd4996', '/wd4244', '/wd4815', '/wd4267'
$hw = @('iwx.cpp', 'iwx_sta.cpp', 'iwx_scan.cpp', 'fw.cpp', 'rx_reorder.cpp' | ForEach-Object { Join-Path $shared $_ }) +
      @('kplat.cpp', 'kproxy.cpp', 'kdma.cpp', 'khal.cpp' | ForEach-Object { Join-Path $kdir $_ })
& $tools.Cl @common @defs @stl "/I$kdir" "/I$shared" "/Fo$out\" @hw
if ($LASTEXITCODE -ne 0) { throw "compile failed ($LASTEXITCODE)" }
$objs = Get-ChildItem $out -Filter *.obj | ForEach-Object FullName
$link = Join-Path (Split-Path $tools.Cl) 'link.exe'
& $link /nologo /DRIVER /SUBSYSTEM:NATIVE /ENTRY:FxDriverEntry /NODEFAULTLIB "/OUT:$out\nekodrop-wlan.sys" @objs `
  ntoskrnl.lib hal.lib wmilib.lib BufferOverflowFastFailK.lib wdfldr.lib wdfdriverentry.lib `
  netadaptercxstub.lib wificxstub.lib WificxTLVGenParse.lib ndis.lib wdmsec.lib
if ($LASTEXITCODE -ne 0) { throw "link failed ($LASTEXITCODE)" }
Get-Item (Join-Path $out 'nekodrop-wlan.sys') | Select-Object Name, Length

# Driver package: .sys + INF + firmware, catalog (signed on the target PC).
$pkg = Join-Path $out 'package'
if (Test-Path $pkg) { Remove-Item $pkg -Recurse -Force }
New-Item -ItemType Directory -Force $pkg | Out-Null
Copy-Item (Join-Path $out 'nekodrop-wlan.sys'), (Join-Path $kdir 'nekodrop-wlan.inf') $pkg
$fw = Join-Path $repo 'third_party\ForbiddenItlwm\itlwm\firmware'
Get-ChildItem $fw -Filter 'iwlwifi-Qu*-hr-b0-68.ucode' | Copy-Item -Destination $pkg
Copy-Item (Join-Path $repo 'third_party\ForbiddenItlwm\iwlwifi-firmware-license') (Join-Path $pkg 'LICENSE.iwlwifi-firmware.txt')
if (Test-Path $tools.InfVerif) {
  & $tools.InfVerif /w (Join-Path $pkg 'nekodrop-wlan.inf') | Out-Host
  if ($LASTEXITCODE -ne 0) { throw "InfVerif failed ($LASTEXITCODE)" }
}
& $tools.Inf2Cat "/driver:$pkg" /os:10_X64 /uselocaltime | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Inf2Cat failed ($LASTEXITCODE)" }
Get-ChildItem $pkg | Select-Object Name, Length
