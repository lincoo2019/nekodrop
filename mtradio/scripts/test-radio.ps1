#Requires -RunAsAdministrator
# Hardware rounds for the radio daemon. -Quick: one short listen on channel 6.
param([switch]$Quick, [switch]$Tx, [switch]$Kinds)
$runs = if ($Kinds) { @('nekodrop-radio-mt.exe|6 --test-kinds 20') } elseif ($Tx) { @('nekodrop-radio-mt.exe|44 --test-tx 4') } elseif ($Quick) { @('nekodrop-radio-mt.exe|6 --test-rx 6') } else { @(
  'nekodrop-radio-mt.exe|6 --test-rx 8',
  'nekodrop-radio-mt.exe|149 --test-rx 12',
  'nekodrop-radio-mt.exe|44 --test-rx 12',
  'nekodrop-radio-mt.exe|6 --test-tx 5') }
# An installed nekodrop owns the card: stop its service for the test, start it again after.
$svc = Get-Service NekoDropService -ErrorAction SilentlyContinue
if ($svc -and $svc.Status -eq 'Running') { Stop-Service NekoDropService -Force; Start-Sleep 3 }
try { & (Join-Path $PSScriptRoot 'smoke-test.ps1') -Step id -Runs $runs }
finally { if ($svc) { Start-Service NekoDropService } }
