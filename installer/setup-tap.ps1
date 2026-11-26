#Requires -RunAsAdministrator
# nekodrop's own TAP-Windows adapter, named "nekodrop", on DHCP. In concurrent
# mode the radio bridges the Wi-Fi station to it, so Windows gets its address
# from the router through it. Never another program's TAP adapter: those may
# carry static addresses (a VPN's), which is what broke the first test.
param([string]$TapSetup = (Join-Path $PSScriptRoot 'tap-windows.exe'))
$ErrorActionPreference = 'Stop'
$name = 'nekodrop'
$tapinstall = Join-Path $env:ProgramFiles 'TAP-Windows\bin\tapinstall.exe'
$tapinf = Join-Path $env:ProgramFiles 'TAP-Windows\driver\OemVista.inf'

function TapAdapters { @(Get-NetAdapter -IncludeHidden -ErrorAction SilentlyContinue |
                         Where-Object { $_.InterfaceDescription -like 'TAP-Windows Adapter V9*' -and $_.Status -ne 'Not Present' }) }

# A previous install or Windows device removal can leave a phantom 'nekodrop'
# adapter with status "Not Present": -IncludeHidden matched it, skipped creating
# a live adapter, and timed out waiting for IPv4 (issue #20). Remove any phantom
# copy first so a fresh adapter can take the name 'nekodrop'.
foreach ($ghost in @(Get-NetAdapter -Name $name -IncludeHidden -ErrorAction SilentlyContinue | Where-Object Status -eq 'Not Present')) {
  "removing stale '$name' adapter ($($ghost.PnPDeviceID))"
  if ($ghost.PnPDeviceID) {
    try { & pnputil.exe /remove-device $ghost.PnPDeviceID | Out-Host } catch { }
  }
}

$existing = Get-NetAdapter -Name $name -IncludeHidden -ErrorAction SilentlyContinue |
            Where-Object Status -ne 'Not Present' | Select-Object -First 1
if (-not $existing) {
  $before = TapAdapters | ForEach-Object InterfaceGuid
  if (-not ((Test-Path $tapinstall) -and (Test-Path $tapinf))) {
    if (-not (Test-Path $TapSetup)) { throw "TAP-Windows installer not found: $TapSetup" }
    # Installs the driver and tapinstall; usually adds one adapter itself.
    $p = Start-Process $TapSetup -ArgumentList '/S', '/SELECT_UTILITIES=1' -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "TAP-Windows setup failed ($($p.ExitCode))" }
  }
  Start-Sleep 2
  $new = TapAdapters | Where-Object { $before -notcontains $_.InterfaceGuid } | Select-Object -First 1
  if (-not $new) {
    if (-not (Test-Path $tapinstall)) { throw 'tapinstall.exe is missing after TAP-Windows setup' }
    & $tapinstall install $tapinf tap0901 | Out-Host
    Start-Sleep 3
    $new = TapAdapters | Where-Object { $before -notcontains $_.InterfaceGuid } | Select-Object -First 1
  }
  if (-not $new) { throw 'no new TAP adapter appeared' }
  if ($new.Status -eq 'Disabled') { Enable-NetAdapter -Name $new.Name -Confirm:$false -ErrorAction SilentlyContinue }
  Rename-NetAdapter -Name $new.Name -NewName $name
  "created TAP adapter '$name' ($($new.InterfaceDescription))"
} elseif ($existing.Status -eq 'Disabled') {
  Enable-NetAdapter -Name $name -Confirm:$false
  "enabled TAP adapter '$name'"
}
# A new adapter's IP interface turns up a few seconds after it does: setting
# it at once failed with "no MSFT_NetIPInterface object" (issue #12).
$deadline = (Get-Date).AddSeconds(30)
while (-not (Get-NetIPInterface -InterfaceAlias $name -AddressFamily IPv4 -ErrorAction SilentlyContinue)) {
  if ((Get-Date) -gt $deadline) {
    $a = Get-NetAdapter -Name $name -IncludeHidden -ErrorAction SilentlyContinue
    throw "the '$name' adapter has no IPv4 interface after 30 s (status: $(if ($a) { $a.Status } else { 'missing' }))"
  }
  Start-Sleep -Milliseconds 500
}
# Addresses from the router, whatever was set before.
Set-NetIPInterface -InterfaceAlias $name -AddressFamily IPv4 -Dhcp Enabled
Get-NetIPAddress -InterfaceAlias $name -AddressFamily IPv4 -PrefixOrigin Manual -ErrorAction SilentlyContinue |
  Remove-NetIPAddress -Confirm:$false -ErrorAction SilentlyContinue
Get-NetRoute -InterfaceAlias $name -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue |
  Where-Object Protocol -eq 'NetMgmt' | Remove-NetRoute -Confirm:$false -ErrorAction SilentlyContinue
Set-DnsClientServerAddress -InterfaceAlias $name -ResetServerAddresses
# After any wired adapter: Windows often gives Ethernet a default route metric
# of 256 + 25 = 281, which lost to 75 + 75 = 150 (issues #1, #11).
Set-NetIPInterface -InterfaceAlias $name -InterfaceMetric 500 -ErrorAction SilentlyContinue
Get-NetIPInterface -InterfaceAlias $name -AddressFamily IPv4 | Select-Object InterfaceAlias, Dhcp, ConnectionState
