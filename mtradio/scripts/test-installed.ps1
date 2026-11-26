#Requires -RunAsAdministrator
# Hardware test runs while nekodrop is installed: stops NekoDropService, binds
# the card to the register proxy nekodrop already put in the driver store, runs
# mtradio's build of nekodrop-radio-mt.exe as SYSTEM, then gives the card back
# to MediaTek's driver and starts the service again. Deletes nothing.
param([string[]]$Runs = @('6 --test-kinds 20 --only 4', '6 --test-kinds 20 --only 3', '6 --test-kinds 20 --only 2', '6 --test-kinds 20 --only 1'))
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$root = Join-Path $repo 'build\mtradio'
$devcon = Get-ChildItem (Join-Path $repo 'third_party\wdk-nuget\wdk\c\Tools') -Recurse -Filter devcon.exe |
          Where-Object { $_.FullName -match '\\x64\\' } | Select-Object -First 1 -ExpandProperty FullName
$exe = Join-Path $root 'nekodrop-radio-mt.exe'
$logs = Join-Path $root 'logs'
New-Item -ItemType Directory -Force $logs | Out-Null
$log = Join-Path $logs ("installed-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.log')

function Get-Card { Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_14C3&DEV_7902*' } | Select-Object -First 1 }
function Card-Inf { (Get-PnpDeviceProperty -InstanceId (Get-Card).InstanceId -KeyName DEVPKEY_Device_DriverInfPath).Data }

$store = [xml](pnputil /enum-drivers /format xml | Out-String)
$proxyInf = $store.PnpUtil.Driver | Where-Object { $_.OriginalName -eq 'winawdl_regproxy.inf' -and $_.ProviderName -eq 'winairdrop project' } |
            Select-Object -Last 1 -ExpandProperty DriverName
# The driver the card has now (MediaTek's), to give it back to exactly that one.
$vendorInf = Card-Inf
if ($vendorInf -eq $proxyInf) { throw 'the card is on the register proxy: give it back to MediaTek''s driver first' }
if (-not $proxyInf) { throw 'nekodrop register proxy not in the driver store' }
$hwid = 'PCI\' + ((((Get-Card).InstanceId -split '\\')[1]) -replace '&REV_.*', '')
"proxy $proxyInf, vendor $vendorInf, $hwid" | ForEach-Object { $_; Add-Content -Path $log -Value $_ -Encoding ascii }

Stop-Service NekoDropService -Force -ErrorAction SilentlyContinue
Start-Sleep 3
try {
  & $devcon update "$env:windir\INF\$proxyInf" 'PCI\VEN_14C3&DEV_7902' | Out-Null
  Start-Sleep 3
  "bound to $(Card-Inf)" | ForEach-Object { $_; Add-Content -Path $log -Value $_ -Encoding ascii }
  $i = 0
  foreach ($run in $Runs) {
    $i++
    "---- nekodrop-radio-mt.exe $run" | ForEach-Object { $_; Add-Content -Path $log -Value $_ -Encoding ascii }
    $task = "mtinst-$i-$(Get-Random)"
    $action = New-ScheduledTaskAction -Execute 'cmd.exe' -Argument "/c `"`"$exe`" $run >> `"$log`" 2>&1`""
    $principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
    Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Force | Out-Null
    Start-ScheduledTask -TaskName $task
    $deadline = (Get-Date).AddSeconds(240)
    do { Start-Sleep -Milliseconds 500 } while ((Get-ScheduledTask -TaskName $task).State -eq 'Running' -and (Get-Date) -lt $deadline)
    Unregister-ScheduledTask -TaskName $task -Confirm:$false
    Get-Process nekodrop-radio-mt -ErrorAction SilentlyContinue | Stop-Process -Force
  }
} finally {
  & $devcon update "$env:windir\INF\$vendorInf" $hwid | Out-Null
  Start-Sleep 3
  "given back to $(Card-Inf)" | ForEach-Object { $_; Add-Content -Path $log -Value $_ -Encoding ascii }
  Start-Service NekoDropService
}
