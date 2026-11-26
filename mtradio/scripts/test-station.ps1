#Requires -RunAsAdministrator
# Hardware test of the Wi-Fi station beside AWDL (Concurrent mode) while
# nekodrop is installed: for each network (saved in Windows, in range on
# 5 GHz), runs mtradio's nekodrop-radio-mt.exe as SYSTEM with AWDL on
# -AwdlChannel and a station config for that network, waits for the TAP
# adapter to get an address, pings the gateway and downloads through it, then
# gives the card back to MediaTek's driver and starts the service again.
# First it times channel moves (--test-switch). The Wi-Fi passwords are read
# from Windows' profiles into a file only SYSTEM and Administrators can read,
# which the radio deletes once read.
param([string[]]$Networks = @(), [int]$AwdlChannel = 44, [int]$Seconds = 75,
      [string]$DownloadUrl = 'https://speed.cloudflare.com/__down?bytes=25000000')
$ErrorActionPreference = 'Stop'
# netsh writes UTF-8 here, in the user's language: decode it as such, and
# match its English and Chinese labels (as \u escapes: this file stays ASCII).
[Console]::OutputEncoding = New-Object Text.UTF8Encoding($false)
$reSignal = '^\s+(Signal|\u4fe1\u53f7)\s*: (\d+)%'
$reChannel = '^\s+(Channel|\u9891\u9053)\s*: (\d+)'
$reProfile = '(All User Profile|\u6240\u6709\u7528\u6237\u914d\u7f6e\u6587\u4ef6)\s*:\s*(.+)$'
$reKey = '(Key Content|\u5173\u952e\u5185\u5bb9)\s*:\s*(.+)$'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$root = Join-Path $repo 'build\mtradio'
$devcon = Get-ChildItem (Join-Path $repo 'third_party\wdk-nuget\wdk\c\Tools') -Recurse -Filter devcon.exe |
          Where-Object { $_.FullName -match '\\x64\\' } | Select-Object -First 1 -ExpandProperty FullName
$exe = Join-Path $root 'nekodrop-radio-mt.exe'
$logs = Join-Path $root 'logs'
New-Item -ItemType Directory -Force $logs | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$log = Join-Path $logs "station-$stamp.log"
function Say([string]$s) { $s; Add-Content -Path $log -Value $s -Encoding utf8 }
trap { Say "error: $_ (line $($_.InvocationInfo.ScriptLineNumber))"; break }

function Get-Card { Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_14C3&DEV_7902*' } | Select-Object -First 1 }
function Card-Inf { (Get-PnpDeviceProperty -InstanceId (Get-Card).InstanceId -KeyName DEVPKEY_Device_DriverInfPath).Data }

$store = [xml](pnputil /enum-drivers /format xml | Out-String)
$proxyInf = $store.PnpUtil.Driver | Where-Object { $_.OriginalName -eq 'winawdl_regproxy.inf' -and $_.ProviderName -eq 'winairdrop project' } |
            Select-Object -Last 1 -ExpandProperty DriverName
if (-not $proxyInf) { throw 'nekodrop register proxy not in the driver store' }
$hwid = 'PCI\' + ((((Get-Card).InstanceId -split '\\')[1]) -replace '&REV_.*', '')

# nekodrop may hold the card (AirDrop on demand): stop it and give the card
# back to MediaTek's driver, so Windows can scan.
Stop-Service NekoDropService -Force -ErrorAction SilentlyContinue
Start-Sleep 3
Get-Process nekodrop-radio-mt, awdld -ErrorAction SilentlyContinue | Stop-Process -Force
$vendorInf = Card-Inf
if ($vendorInf -eq $proxyInf) {
  $vendorInf = $store.PnpUtil.Driver | Where-Object { $_.ProviderName -like 'MediaTek*' -and $_.ClassName -eq 'Net' } |
               Select-Object -Last 1 -ExpandProperty DriverName
  if (-not $vendorInf) { throw 'MediaTek''s driver is not in the driver store' }
  & $devcon update "$env:windir\INF\$vendorInf" $hwid | Out-Null
  Say "card given back to $vendorInf before the test"
}
# MediaTek's driver takes about a minute to come back after a swap (the
# service's own give-back on stop is one): wait for a network list that is
# still there ten seconds later.
function Visible { @(netsh wlan show networks mode=bssid | Select-String '^SSID \d+ :').Count }
Start-Sleep 5
$deadline = (Get-Date).AddSeconds(180)
do {
  Start-Sleep 3
  $stable = (Visible) -ge 3
  if ($stable) { Start-Sleep 10; $stable = (Visible) -ge 3 }
} while (-not $stable -and (Get-Date) -lt $deadline)
Say "Windows sees $(Visible) networks"

# The networks to try, from Windows' scan (taken while Windows still has the card).
$scan = @{}
$cur = $null; $bssid = $null
foreach ($l in (netsh wlan show networks mode=bssid)) {
  if ($l -match '^SSID \d+ : (.*)$') { $cur = $Matches[1].Trim(); continue }
  if ($l -match 'BSSID \d+\s+: ([0-9a-f:]{17})') { $bssid = $Matches[1]; continue }
  if ($l -match $reSignal) { $signal = [int]$Matches[2]; continue }
  if ($l -match $reChannel) {
    $ch = [int]$Matches[2]
    if ($ch -gt 14 -and (-not $scan[$cur] -or $scan[$cur].Signal -lt $signal)) {
      $scan[$cur] = [pscustomobject]@{ Ssid = $cur; Bssid = $bssid; Channel = $ch; Signal = $signal }
    }
  }
}
$saved = netsh wlan show profiles | Select-String $reProfile | ForEach-Object { $_.Matches[0].Groups[2].Value.Trim() }
if (-not $Networks) { $Networks = $scan.Keys | Where-Object { $saved -contains $_ } }
$targets = foreach ($n in $Networks) {
  if (-not $scan[$n]) { Say "skipped `"$n`": not heard on 5 GHz"; continue }
  $key = netsh wlan show profile name="$n" key=clear | Select-String $reKey |
         ForEach-Object { $_.Matches[0].Groups[2].Value.TrimEnd() }
  if (-not $key) { Say "skipped `"$n`": no saved password"; continue }
  [pscustomobject]@{ Ssid = $n; Bssid = $scan[$n].Bssid; Channel = $scan[$n].Channel; Signal = $scan[$n].Signal; Key = $key }
}
if (-not $targets) {
  Say "no saved 5 GHz network in range: $($scan.Count) heard on 5 GHz ($($scan.Keys -join ' | ')), $(@($saved).Count) saved"
  netsh wlan show networks mode=bssid | Select-Object -First 24 | ForEach-Object { Say "netsh: $_" }
  Start-Service NekoDropService
  exit 1
}
foreach ($t in $targets) { Say "network `"$($t.Ssid)`" $($t.Bssid) channel $($t.Channel), signal $($t.Signal)%" }

Say "proxy $proxyInf, vendor $vendorInf, $hwid"

$runDir = 'C:\ProgramData\nekodrop\run'
New-Item -ItemType Directory -Force $runDir | Out-Null
$conf = Join-Path $runDir 'mt-test-sta.conf'

function Start-Radio([string]$arguments, [string]$out) {
  $task = "mtsta-$(Get-Random)"
  $action = New-ScheduledTaskAction -Execute 'cmd.exe' -Argument "/c `"`"$exe`" $arguments > `"$out`" 2>&1`""
  $principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
  Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Force | Out-Null
  Start-ScheduledTask -TaskName $task
  return $task
}
function Wait-Radio([string]$task, [int]$limit) {
  $deadline = (Get-Date).AddSeconds($limit)
  do { Start-Sleep -Milliseconds 500 } while ((Get-ScheduledTask -TaskName $task).State -eq 'Running' -and (Get-Date) -lt $deadline)
  Unregister-ScheduledTask -TaskName $task -Confirm:$false
  Get-Process nekodrop-radio-mt -ErrorAction SilentlyContinue | Stop-Process -Force
}

try {
  & $devcon update "$env:windir\INF\$proxyInf" 'PCI\VEN_14C3&DEV_7902' | Out-Null
  Start-Sleep 3
  Say "bound to $(Card-Inf)"

  $out = Join-Path $logs "station-$stamp-switch.log"
  Wait-Radio (Start-Radio "$AwdlChannel --test-switch 40" $out) 60
  Get-Content $out | ForEach-Object { Say $_ }

  $n = 0
  foreach ($t in $targets) {
    $n++
    Say "==== `"$($t.Ssid)`" on channel $($t.Channel), AWDL on $AwdlChannel"
    $lines = @("ssid=$($t.Ssid)", "bssid=$($t.Bssid)", "channel=$($t.Channel)", 'tap=nekodrop', "passphrase=$($t.Key)")
    [IO.File]::WriteAllLines($conf, $lines, (New-Object Text.UTF8Encoding($false)))
    $acl = New-Object Security.AccessControl.FileSecurity
    $acl.SetAccessRuleProtection($true, $false)
    foreach ($sid in 'S-1-5-18', 'S-1-5-32-544') {
      $acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule(
        (New-Object Security.Principal.SecurityIdentifier($sid)), 'FullControl', 'Allow')))
    }
    Set-Acl $conf $acl
    $out = Join-Path $logs "station-$stamp-$n.log"
    $task = Start-Radio "$AwdlChannel --sta-config `"$conf`" --run-for $Seconds" $out
    $start = Get-Date
    $connected = $false
    while (((Get-Date) - $start).TotalSeconds -lt 30) {
      Start-Sleep 1
      if ((Test-Path $out) -and (Select-String -Path $out -Pattern 'station: connected to' -Quiet)) { $connected = $true; break }
    }
    Say "connected: $connected after $([int]((Get-Date) - $start).TotalSeconds) s"
    $ip = $null; $gw = $null
    if ($connected) {
      while (((Get-Date) - $start).TotalSeconds -lt 50) {
        Start-Sleep 1
        $cfg = Get-NetIPConfiguration -InterfaceAlias nekodrop -ErrorAction SilentlyContinue
        $ip = $cfg.IPv4Address | Where-Object { $_.IPAddress -notlike '169.254.*' } | Select-Object -First 1 -ExpandProperty IPAddress
        $gw = $cfg.IPv4DefaultGateway | Select-Object -First 1 -ExpandProperty NextHop
        if ($ip -and $gw) { break }
      }
      Say "TAP address: $ip, gateway $gw, after $([int]((Get-Date) - $start).TotalSeconds) s"
    }
    if ($ip -and $gw) {
      ping -S $ip $gw -n 20 -w 1500 | ForEach-Object { Say "ping: $_" }
      $curl = & curl.exe --interface $ip -s -o NUL -m 25 -w 'download: %{size_download} bytes in %{time_total} s, %{speed_download} bytes/s, HTTP %{http_code}' $DownloadUrl
      Say $curl
    }
    Wait-Radio $task ($Seconds + 30)
    Remove-Item $conf -ErrorAction SilentlyContinue
    Get-Content $out | Where-Object { $_ -notmatch 'fw-event' } | ForEach-Object { Say $_ }
  }
} finally {
  Remove-Item $conf -ErrorAction SilentlyContinue
  & $devcon update "$env:windir\INF\$vendorInf" $hwid | Out-Null
  Start-Sleep 3
  Say "given back to $(Card-Inf)"
  Start-Service NekoDropService
}
