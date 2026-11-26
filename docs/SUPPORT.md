# Troubleshooting, restore and diagnostic report

English · [简体中文](SUPPORT.zh-CN.md) · [Back to README](../README.md)

## Troubleshooting

| Symptom | Try |
|---|---|
| iPhone doesn't see the PC | Set the iPhone's AirDrop to *Everyone for 10 Minutes* (or *Contacts Only*, if this PC has an Apple ID identity), turn it off and on (this clears a cached record), and keep the phone unlocked nearby |
| The PC takes long to find the iPhone | Check that Bluetooth is on: the PC wakes iPhones with a Bluetooth beacon. The diagnostic report's engine lines say `beacon: false` and why when it cannot send one |
| Sending to the iPhone is slow | If nekodrop's Wi-Fi is on a different 5 GHz channel than AirDrop (44 or 149), speed drops; disconnecting Wi-Fi in nekodrop or moving the router to that channel helps |
| AirDrop stops working once nekodrop joins Wi-Fi | The router's 5 GHz channel differs from AirDrop's (44 or 149): turn on *Settings → Experimental → Time sharing when Wi-Fi and AirDrop use different channels*, or disconnect Wi-Fi in nekodrop if the PC is wired ([#35](../../../issues/35)) |
| The PC shows up twice on the iPhone | A cached record from an earlier run. Toggle AirDrop on the iPhone |
| No internet | Tray → **Wi-Fi…**: check you're connected. The network may need a sign-in page or be WPA3-only/802.1X (see above) |
| The Windows Wi-Fi icon disappeared | Expected in *together* mode: nekodrop owns the card. Use **Wi-Fi…**, or switch to *on demand* |
| An app or game refuses to log in / shows no QR code in *together* mode | Some apps want to see a Windows Wi-Fi adapter, and in *together* mode there is none (nekodrop's adapter is a wired-type one). Switch to *on demand* for those apps ([#33](../../../issues/33)) |
| Something else | Open an [issue](../../../issues) with the [diagnostic report](#reporting-a-problem-the-diagnostic-report) |

## Something went wrong? One-click restore

Start menu → **nekodrop - restore Windows Wi-Fi** (or double-click `restore.cmd` in `C:\Program Files\nekodrop`). It is standalone (works even if nekodrop's install folder is damaged), stops nekodrop, gives the card back to its vendor driver without hardcoding Intel (keeping Intel rollback behaviour unchanged), removes nekodrop's driver packages and brings Windows Wi-Fi back, whatever state nekodrop left the card in. nekodrop stays installed; run the installer again to use it. Log: `C:\ProgramData\nekodrop\restore.log`.

## Reporting a problem: the diagnostic report

Logs are what let us fix things, especially on cards we don't have. Please attach them to every [issue](../../../issues).

1. Start menu → **nekodrop - diagnostic report** (or tray icon → **Diagnostic report…**, or **Diagnostic report** at the bottom of the main window). Allow the administrator prompt; the logs and driver state are readable by administrators only.
2. Two files appear on your Desktop and the report opens in Notepad:
   - `nekodrop-report-<time>.md`: a summary, a step-by-step check of the last radio start (driver bind → firmware → PNVM → init → ready), system, card, driver, network and Bluetooth details, a discovery timeline (with how many seconds each step of discovery took) and the key log lines. It is also copied to the clipboard: **paste it into the issue**.
   - `nekodrop-logs-<time>.txt`: the full logs. **Drag it into the issue** as an attachment.
3. SSIDs, MAC and private IP addresses, your user name and the computer name are replaced by placeholders such as `<SSID-1>` and `<MAC-2>`. Nothing is uploaded. Please look the report over before posting.

**On an older nekodrop without this entry**, download [`installer/report.ps1`](../installer/report.ps1) and run it from a command prompt: `powershell -ExecutionPolicy Bypass -File report.ps1`. Or collect the logs by hand:

| File | What's in it |
|---|---|
| `C:\ProgramData\nekodrop\service.log` (and `service.log.1`) | the service, the radio (firmware, AWDL, Wi-Fi) and awdld; readable by administrators |
| `%LocalAppData%\nekodrop\engine.log` (and `engine.log.old`) | the AirDrop engine: discovery, Bluetooth beacon, transfers |
| `%LocalAppData%\nekodrop\appleid\appleid.log` | Apple ID sign-in & Anisette helper log (auto-redacted in `report.ps1` and exportable from Settings → Apple ID) |
| `C:\ProgramData\nekodrop\install.log` | the installer: driver, TAP adapter, firewall, service |
| `C:\ProgramData\nekodrop\restore.log` | the one-click restore |
