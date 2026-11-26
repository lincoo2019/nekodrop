# Compiles winawdl_regproxy.dll and validates its INF.
# Signing and installation are separate, deliberate steps.
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\..\..\scripts\toolchain.ps1"

# 2.31: the newest UMDF Windows 10 2004-22H2 has (2.33 is Windows 11 only, issue #3).
$umdfVersion = '2.31'
$tools = Initialize-Toolchain -UmdfVersion $umdfVersion
$umdfMajor, $umdfMinor = $umdfVersion.Split('.')
$out = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Force $out | Out-Null

Push-Location $out
try {
  # C4324 fires inside the kit's own wdfrequest.h.
  & $tools.Cl /nologo /W4 /WX /wd4324 /O2 /GS /guard:cf /Zi /MT `
      /DUNICODE /D_UNICODE /D_AMD64_ /D_WIN64 `
      "/DUMDF_VERSION_MAJOR=$umdfMajor" "/DUMDF_VERSION_MINOR=$umdfMinor" `
      "/Fd$out\winawdl_regproxy.pdb" "$PSScriptRoot\regproxy.c" `
      /link /DLL "/OUT:$out\winawdl_regproxy.dll" /SUBSYSTEM:WINDOWS /GUARD:CF /DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF `
      '/PDBALTPATH:%_PDB%' `
      WdfDriverStubUm.lib ntdll.lib mincore.lib
  if ($LASTEXITCODE -ne 0) { throw "regproxy.c failed to compile ($LASTEXITCODE)" }
} finally { Pop-Location }

Copy-Item "$PSScriptRoot\winawdl_regproxy.inf" $out -Force
# Basic validation (/v), not the Windows Driver rules (/w): those require the
# reflector through Include/Needs WUDFRD.inf, which leaves the device without a
# function driver on Windows 10 (#18). The package is signed on the user's PC,
# never submitted for WHQL.
& $tools.InfVerif /v "$out\winawdl_regproxy.inf"
if ($LASTEXITCODE -ne 0) { throw "INF validation failed ($LASTEXITCODE)" }

Get-ChildItem $out -Include *.dll, *.inf -Recurse | Select-Object Name, Length
