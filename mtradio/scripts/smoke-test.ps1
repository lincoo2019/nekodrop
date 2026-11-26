#Requires -RunAsAdministrator
# One supervised MT7902 smoke test: binds the register proxy to the card, runs
# mtprobe as SYSTEM (AWE pages need SeLockMemoryPrivilege), then ALWAYS gives
# the card back to MediaTek's driver and removes the trust it added.
# Windows' Wi-Fi is gone while the test runs; the wired network stays up.
param([ValidateSet('id', 'dma', 'fw')] [string]$Step = 'fw',
      # Extra runs after the probe, each "exe-name|arguments", e.g. "nekodrop-radio-mt.exe|6 --test-rx 8".
      [string[]]$Runs = @())
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$root = Join-Path $repo 'build\mtradio'
$package = Join-Path $repo 'driver\regproxy\build\package'
$inf = Join-Path $package 'winawdl_regproxy.inf'
$cer = Join-Path $package 'winairdrop-signing.cer'
$certSubject = 'CN=winairdrop driver signing (local test)'
$probe = Join-Path $root 'mtprobe.exe'
$devcon = Get-ChildItem (Join-Path $repo 'third_party\wdk-nuget\wdk\c\Tools') -Recurse -Filter devcon.exe |
          Where-Object { $_.FullName -match '\\x64\\' } | Select-Object -First 1 -ExpandProperty FullName
$logs = Join-Path $root 'logs'
New-Item -ItemType Directory -Force $logs | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$probeLog = Join-Path $logs "probe-$stamp.log"
Start-Transcript (Join-Path $logs "smoke-$stamp.log") | Out-Null

function Get-Card { Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_14C3&DEV_7902*' } | Select-Object -First 1 }
function Show-Card($label) {
  $c = Get-Card
  $svc = (Get-PnpDeviceProperty -InstanceId $c.InstanceId -KeyName DEVPKEY_Device_Service).Data
  $drv = (Get-PnpDeviceProperty -InstanceId $c.InstanceId -KeyName DEVPKEY_Device_DriverInfPath).Data
  "${label}: $($c.FriendlyName) | status $($c.Status) | service $svc | $drv"
}

$card = Get-Card
if (-not $card) { throw 'No MT7902 (PCI\VEN_14C3&DEV_7902) present.' }
if (-not $devcon) { throw 'devcon.exe not found in the WDK package.' }
$vendorInf = (Get-PnpDeviceProperty -InstanceId $card.InstanceId -KeyName DEVPKEY_Device_DriverInfPath).Data
$hwid = ($card.InstanceId -split '\\')[1] -replace '&REV_.*', ''   # VEN_..&DEV_..&SUBSYS_..
$hwid = "PCI\$hwid"
$wired = Get-NetAdapter -Physical | Where-Object { $_.Status -eq 'Up' -and $_.PnPDeviceID -ne $card.InstanceId }
if (-not $wired) { throw 'No other network adapter is up; refusing to take the Wi-Fi card.' }
Show-Card 'before'
"vendor driver package: $vendorInf, hardware id $hwid"

try {
  'trust the local signing certificate (removed again at the end)'
  foreach ($store in 'Root', 'TrustedPublisher') { Import-Certificate -FilePath $cer -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null }
  pnputil /add-driver $inf
  if ($LASTEXITCODE -ne 0) { throw "pnputil add-driver failed ($LASTEXITCODE)" }
  & $devcon update $inf 'PCI\VEN_14C3&DEV_7902'
  "devcon exit $LASTEXITCODE"
  Start-Sleep -Seconds 3
  Show-Card 'bound'

  $steps = if ($Step -eq 'id') { @('id') } else { @('id', $Step) }
  foreach ($s in $steps) {
    "---- mtprobe $s (as SYSTEM)"
    $task = "mtprobe-$stamp-$s"
    $action = New-ScheduledTaskAction -Execute $probe -Argument "$s --log `"$probeLog`""
    $principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
    Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Force | Out-Null
    Start-ScheduledTask -TaskName $task
    $deadline = (Get-Date).AddSeconds(120)
    do { Start-Sleep -Milliseconds 500 } while ((Get-ScheduledTask -TaskName $task).State -eq 'Running' -and (Get-Date) -lt $deadline)
    $result = (Get-ScheduledTaskInfo -TaskName $task).LastTaskResult
    Unregister-ScheduledTask -TaskName $task -Confirm:$false
    "mtprobe $s exit code $result"
    if ($result -ne 0) { break }
  }
  $i = 0
  foreach ($run in $Runs) {
    if ($result -ne 0) { break }
    $exe, $runArgs = $run -split '\|', 2
    $i++
    "---- $exe $runArgs (as SYSTEM)"
    $task = "mtrun-$stamp-$i"
    $exePath = Join-Path $root "$exe"
    $action = New-ScheduledTaskAction -Execute 'cmd.exe' -Argument "/c `"`"$exePath`" $runArgs >> `"$probeLog`" 2>&1`""
    $principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
    Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Force | Out-Null
    Start-ScheduledTask -TaskName $task
    $deadline = (Get-Date).AddSeconds(180)
    do { Start-Sleep -Milliseconds 500 } while ((Get-ScheduledTask -TaskName $task).State -eq 'Running' -and (Get-Date) -lt $deadline)
    $runResult = (Get-ScheduledTaskInfo -TaskName $task).LastTaskResult
    Unregister-ScheduledTask -TaskName $task -Confirm:$false
    Get-Process ($exe -replace '\.exe$', '') -ErrorAction SilentlyContinue | Stop-Process -Force
    "$exe $runArgs exit code $runResult"
  }
  '---- probe log'
  if (Test-Path $probeLog) { Get-Content $probeLog }
} finally {
  '---- giving the card back to MediaTek'
  Get-Process mtprobe -ErrorAction SilentlyContinue | Stop-Process -Force
  $store = [xml](pnputil /enum-drivers /format xml | Out-String)
  $ours = $store.PnpUtil.Driver | Where-Object { $_.OriginalName -eq 'winawdl_regproxy.inf' -and $_.ProviderName -eq 'winairdrop project' }
  foreach ($p in $ours) { pnputil /delete-driver $p.DriverName /uninstall /force }
  $vendorPath = if ([IO.Path]::IsPathRooted($vendorInf)) { $vendorInf } else { Join-Path "$env:windir\INF" $vendorInf }
  & $devcon update $vendorPath $hwid
  if ($LASTEXITCODE -gt 1) { & $devcon update $vendorPath 'PCI\VEN_14C3&DEV_7902' }
  pnputil /scan-devices | Out-Null
  Start-Sleep -Seconds 3
  foreach ($s in 'Root', 'TrustedPublisher') {
    Get-ChildItem "Cert:\LocalMachine\$s" | Where-Object { $_.Subject -eq $certSubject } | Remove-Item
  }
  Show-Card 'after'
  Stop-Transcript | Out-Null
}
