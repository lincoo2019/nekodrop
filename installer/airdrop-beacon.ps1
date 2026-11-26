# Sends the Bluetooth advertisement an Apple device sends before an AirDrop,
# so that devices nearby turn AWDL on and announce themselves. It carries no
# identity: only receivers visible to everyone react. Runs until it is killed;
# airdrop\sender.py starts and stops it. -DryRun builds it and sends nothing.
# Windows PowerShell 5.1 only: PowerShell 7 cannot load the Bluetooth API so.
#
# nekodrop's copy of winawdl's scripts\airdrop-beacon.ps1 (MIT), with two
# changes (#22): it leaves the folder it was started in (the install
# folder's airdrop\, which it otherwise held open, so an uninstall could not
# remove it), and it exits once the AirDrop engine that started it is gone,
# instead of living on as an orphan after every engine restart.
# 0.1.36 (#30, #32): when Windows aborts the advertisement, the reason
# (BluetoothError) is printed and the start is tried twice more.
$ErrorActionPreference = 'Stop'
[Environment]::CurrentDirectory = $env:SystemRoot
Set-Location $env:SystemRoot
$Parent = 0
try { $Parent = [int](Get-CimInstance Win32_Process -Filter "ProcessId=$PID" -ErrorAction Stop).ParentProcessId } catch { }
function Test-ParentGone {
  return $Parent -gt 0 -and -not (Get-Process -Id $Parent -ErrorAction SilentlyContinue)
}
$AppleCompanyId = 0x004C
$StartWaitSeconds = 5
$StartTries = 3

function Say($line) {
  [Console]::Out.WriteLine($line)
  [Console]::Out.Flush()
}

try {
  $namespace = 'Windows.Devices.Bluetooth.Advertisement'
  [void][Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementPublisher, Windows.Devices.Bluetooth.Advertisement, ContentType = WindowsRuntime]
  [void][Windows.Devices.Bluetooth.Advertisement.BluetoothLEManufacturerData, Windows.Devices.Bluetooth.Advertisement, ContentType = WindowsRuntime]
  Add-Type -AssemblyName System.Runtime.WindowsRuntime

  # PowerShell cannot call into the list an advertisement keeps its sections
  # in, so that one call is compiled.
  $metadata = "$env:windir\System32\WinMetadata"
  $compiler = New-Object System.CodeDom.Compiler.CompilerParameters
  $compiler.GenerateInMemory = $true
  $compiler.ReferencedAssemblies.AddRange([string[]]@(
    [Reflection.Assembly]::Load('System.Runtime, Version=4.0.0.0, Culture=neutral, PublicKeyToken=b03f5f7f11d50a3a').Location,
    "$metadata\Windows.Devices.winmd", "$metadata\Windows.Foundation.winmd"))
  Add-Type -CompilerParameters $compiler -TypeDefinition @'
using Windows.Devices.Bluetooth.Advertisement;
public static class AirDropBeacon {
  public static void Add(BluetoothLEAdvertisementPublisher publisher, BluetoothLEManufacturerData data) {
    publisher.Advertisement.ManufacturerData.Add(data);
  }
}
'@

  # Why Windows aborted the advertisement. Compiled on its own: if this cannot
  # be built, only the reason is lost, the beacon still runs.
  $watch = $false
  try {
    $compiler2 = New-Object System.CodeDom.Compiler.CompilerParameters
    $compiler2.GenerateInMemory = $true
    $compiler2.ReferencedAssemblies.AddRange([string[]]@(
      [Reflection.Assembly]::Load('System.Runtime, Version=4.0.0.0, Culture=neutral, PublicKeyToken=b03f5f7f11d50a3a').Location,
      [Reflection.Assembly]::Load('System.Runtime.InteropServices.WindowsRuntime, Version=4.0.0.0, Culture=neutral, PublicKeyToken=b03f5f7f11d50a3a').Location,
      "$metadata\Windows.Devices.winmd", "$metadata\Windows.Foundation.winmd"))
    Add-Type -CompilerParameters $compiler2 -TypeDefinition @'
using Windows.Devices.Bluetooth.Advertisement;
public static class AirDropBeaconWatch {
  public static string Error = "";
  public static void Watch(BluetoothLEAdvertisementPublisher publisher) {
    publisher.StatusChanged += OnStatus;
  }
  static void OnStatus(BluetoothLEAdvertisementPublisher sender, BluetoothLEAdvertisementPublisherStatusChangedEventArgs args) {
    if (args.Status == BluetoothLEAdvertisementPublisherStatus.Aborted) Error = args.Error.ToString();
  }
}
'@
    $watch = $true
  } catch { }

  # Type 5 (AirDrop), 18 bytes: eight zeros, version 1, four two-byte hashes
  # of the sender's contact details, a zero. With an Apple ID identity the
  # engine passes the owner's prefixes (Apple ID, phone, email, second email)
  # in NEKODROP_BLE_HASHES, so iPhones set to Contacts Only wake for a known
  # sender; otherwise fresh random bytes are generated on each start/repulse.
  function New-AirDropPayload {
    $hashes = New-Object byte[] 8
    $known = $env:NEKODROP_BLE_HASHES
    if ($known -match '^[0-9a-fA-F]{16}$') {
      for ($i = 0; $i -lt 8; $i++) { $hashes[$i] = [Convert]::ToByte($known.Substring(2 * $i, 2), 16) }
    } else {
      (New-Object Random).NextBytes($hashes)
    }
    return [byte[]](@(0x05, 0x12) + (,0 * 8) + @(0x01) + $hashes + @(0x00))
  }

  function Start-AirDropPublisher {
    $payload = New-AirDropPayload
    $data = New-Object "$namespace.BluetoothLEManufacturerData"
    $data.CompanyId = $AppleCompanyId
    $data.Data = [Runtime.InteropServices.WindowsRuntime.WindowsRuntimeBufferExtensions]::AsBuffer($payload)
    $pub = $null
    for ($try = 1; $try -le $StartTries; $try++) {
      $pub = New-Object "$namespace.BluetoothLEAdvertisementPublisher"
      [AirDropBeacon]::Add($pub, $data)
      if ($watch) { [AirDropBeaconWatch]::Error = ''; [AirDropBeaconWatch]::Watch($pub) }
      $pub.Start()
      $deadline = (Get-Date).AddSeconds($StartWaitSeconds)
      while ("$($pub.Status)" -notin 'Started', 'Aborted' -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 100
      }
      if ("$($pub.Status)" -eq 'Started' -or $try -eq $StartTries -or (Test-ParentGone)) { break }
      try { $pub.Stop() } catch { }
      Start-Sleep -Seconds 1
    }
    return $pub
  }

  if ($args -contains '-DryRun') {
    $payload = New-AirDropPayload
    $data = New-Object "$namespace.BluetoothLEManufacturerData"
    $data.CompanyId = $AppleCompanyId
    $data.Data = [Runtime.InteropServices.WindowsRuntime.WindowsRuntimeBufferExtensions]::AsBuffer($payload)
    $publisher = New-Object "$namespace.BluetoothLEAdvertisementPublisher"
    [AirDropBeacon]::Add($publisher, $data)
    Say "would advertise $($payload.Length) bytes$(if ($watch) { '' } else { ' (no abort reason)' })"
    return
  }

  $publisher = Start-AirDropPublisher
  if (-not $publisher -or "$($publisher.Status)" -ne 'Started') {
    $why = if ($watch -and [AirDropBeaconWatch]::Error) { " (error $([AirDropBeaconWatch]::Error))" } else { '' }
    $st = if ($publisher) { "$($publisher.Status)" } else { 'Failed' }
    Say "Bluetooth did not start advertising, status $st$why after $StartTries tries"
    return
  }
  Say 'advertising'

  # Optional re-pulse trigger file from sender.Beacon.repulse(): briefly stop
  # and restart advertising (2.5 s gap) so iOS sharingd sees a fresh BLE wake
  # edge after a transfer ends instead of deduplicating the old continuous ad.
  $pulseFile = $env:NEKODROP_BEACON_PULSE
  $lastPulse = [DateTime]::MinValue
  if ($pulseFile -and (Test-Path -LiteralPath $pulseFile)) {
    try { $lastPulse = (Get-Item -LiteralPath $pulseFile -ErrorAction Stop).LastWriteTimeUtc } catch { }
  }
  $ticks = 0
  while ($true) {
    Start-Sleep -Milliseconds 500
    $ticks++
    if (($ticks % 4) -eq 0 -and (Test-ParentGone)) { break }
    if ($pulseFile -and (Test-Path -LiteralPath $pulseFile)) {
      $mt = [DateTime]::MinValue
      try { $mt = (Get-Item -LiteralPath $pulseFile -ErrorAction Stop).LastWriteTimeUtc } catch { }
      if ($mt -gt $lastPulse) {
        $lastPulse = $mt
        try { if ($publisher) { $publisher.Stop() } } catch { }
        for ($w = 0; $w -lt 5; $w++) {
          if (Test-ParentGone) { break }
          Start-Sleep -Milliseconds 500
        }
        if (Test-ParentGone) { break }
        $publisher = Start-AirDropPublisher
      }
    }
  }
  try { if ($publisher) { $publisher.Stop() } } catch { }
} catch {
  Say "Bluetooth advertising failed: $($_.Exception.Message)"
}
