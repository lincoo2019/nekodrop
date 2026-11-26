# How it works, status and building

English · [简体中文](HOW-IT-WORKS.zh-CN.md) · [Back to README](../README.md)

## How it works

```
 nekodrop.exe (tray app, your session) ──JSON pipe──► NekoDropService (SYSTEM)
   │  AirDrop engine (Python)                         │ takes/gives back the card
   │  notifications · context menu · send window      │ Wi-Fi manager, watchdogs
   │                                                  ▼
   └── awdl0 (Wintun 8 MiB, IPv6) ◄── awdld (AWDL, OWL) ◄─SHM/pipe─ nekodrop-radio (user-mode driver)
                                                          ├ MAC 0: AWDL on channel 6/44/149
     "nekodrop" TAP adapter ◄── Ethernet bridge ◄─────────┤ MAC 1: station, joined to your router
     (Windows' internet)                                  └ UMDF register proxy + user-mode DMA → Wi-Fi card
```

1. **Register proxy.** A tiny UMDF driver (`driver/regproxy`) binds to the card in place of its vendor's driver. It does nothing but map the card's registers and interrupts into one user-mode process. No kernel code of ours runs.
2. **User-mode driver.** For Intel cards, `nekodrop-radio` (`iwxradio/`) is a port of the OpenBSD/itlwm `iwx` driver to user mode. It loads Intel's firmware through user-mode DMA (with 8 KiB RX buffers and lock-free 4+4 MiB SHM ring to `awdld`), then runs two MACs at once: one listens and injects AWDL frames, the other is a normal Wi-Fi station with WPA2 (4-way handshake, keys in firmware, rate control). The Realtek and MediaTek radios do the same job for their chips; the Wi-Fi station code is shared.
3. **AWDL.** `awdld` (OWL, the Open Wireless Link) speaks Apple's AWDL protocol over the shared-memory / radio pipe and exposes the link as the IPv6 interface `awdl0` (8 MiB Wintun ring, fair-polled `ev_win.c`, TCP GRO, optional HE80 + MTU 1500).
4. **AirDrop.** A Python engine speaks AirDrop over `awdl0`: mDNS discovery (with IPv6-only macOS replies), HTTPS on port `8771` (spare blocks `8772-8779`, `18771-18779`, `28771-28779`), streaming DVZip→CPIO extraction with 4 MiB async disk buffering, the `/Discover`, `/Ask` and `/Upload` flow, and the BLE beacon (with automatic post-transfer re-pulse).
5. **Internet.** Frames from the station MAC are bridged to the TAP adapter "nekodrop". Windows treats it as Ethernet and gets an address by DHCP.

Design notes: [docs/DESIGN.md](DESIGN.md) (driver, DMA, risks) · [docs/CLIENT.md](CLIENT.md) (service, client, modes) · [docs/TESTING.md](TESTING.md) (throughput test method).

## Status

| Part | State |
|---|---|
| User-mode Intel driver: firmware boot, monitor, inject, scan | ✅ works on AX201, AX200, AX210, AX211, AX411/AX1690i, 9560 (user reports for the last five) |
| iPhone → Windows | ✅ works; up to **50.81 MiB/s (53.28 MB/s)** net / **47.36 MiB/s** end-to-end on AX201 (`v2.2.9+`, 0 drops) |
| Windows → iPhone | ✅ works; up to **50.55 MiB/s (53.00 MB/s)** on AX201 (`v2.2.9+`, 99.85% BA rate, 0 drops) |
| Wi-Fi and AirDrop at the same time | ✅ works (WPA2, DHCP, DNS through the "nekodrop" adapter) |
| Wi-Fi manager, saved networks, captive portal | ✅ |
| Service, tray app, notifications, context menu, installer | ✅ |
| Tray panel, radar, Share sheet, custom shortcut, live transfer speed | ✅ `v2.2.10` |
| Contacts Only (Apple ID sign-in, log export, or Mac certificate) | ✅ experimental (`v2.1.0+`, `-27952` / 2FA retry & log export in `v2.2.9`) |
| WPA3-only, OWE | 🧪 0.1.12, untested on hardware |
| 802.1X (Enterprise) | ⏳ planned (eduroam PEAP setup documented in [#58](../../../pull/58)) |
| AX200, AX210, AX211, AX411 / Killer AX1690i | ✅ two-way AirDrop in user reports ([#1](../../../issues/1), [#14](../../../issues/14), [#24](../../../issues/24), [#37](../../../issues/37)) |
| 9560 on Qu | ✅ works in a user report ([#45](../../../issues/45)) |
| 9560 on So, AX203 | 🧪 ported from upstream, no user report yet |
| Realtek RTL8852CE | ✅ two-way AirDrop confirmed up to **~24.5 MB/s** ([#61](../../../issues/61)) |
| MediaTek MT7902 | ✅ AirDrop both ways and Concurrent, tested by its author ([#34](../../../pull/34), [#47](../../../pull/47)) |
| MediaTek MT7922 / MT7921 | 🧪 MT7922 tested on demand ([#40](../../../pull/40)); HE80 2SS, scan `FIF_BIT_CLR`, firmware RA & RX A-MSDU fixes in `v2.2.8`–`v2.2.9` ([#65](../../../pull/65), [#66](../../../issues/66)) |
| Intel BE200 (Wi-Fi 7) | 🧪 experimental: Bz/MLD path with 4096 B ring alignment & dual-LMAC diagnostics in `v2.2.9` ([#53](../../../issues/53), [docs/BE200.md](BE200.md)) |
| Windows 10 | ✅ two-way AirDrop confirmed in user reports on 0.1.16+ ([#18](../../../issues/18)) |
| Kernel WiFiCx driver (`kdriver/`) | 🧊 shelved: experimental and **not installed** by the setup. See [docs/KERNEL.md](KERNEL.md) |

## Building from source

Needs Visual Studio 2022 Build Tools (C++, Windows SDK, CMake, vcpkg), the .NET 8 SDK and Git. Inno Setup 6 is optional (without it you get a zip). No WDK install is needed: the NuGet package is used.

```powershell
git clone https://github.com/zheznanohana/nekodrop.git
cd nekodrop
.\scripts\fetch-deps.ps1        # winawdl, devourer, ForbiddenItlwm (firmware), MinGW, WDK NuGet → third_party\
.\scripts\build-installer.ps1   # everything → build\nekodrop-<version>-setup.exe
.\scripts\build-radio.ps1       # just the Intel user-mode driver, plus its offline self-tests
.\scripts\build-radio-rtl.ps1   # just the Realtek RTL8852CE radio (devourer), plus its offline unit tests
```

| Folder | What |
|---|---|
| `iwxradio/` | User-mode Intel driver (C++): firmware, AWDL monitor/inject, station, WPA2, UMAC scan |
| `driver/regproxy/` | UMDF register-proxy driver |
| `app/service/` | NekoDropService (C#): card ownership, radio supervision, Wi-Fi manager |
| `app/client/` | Tray app (C# WPF): UI, notifications, AirDrop engine host |
| `app/shellext/` | Windows 11 context-menu handler |
| `installer/` | Inno Setup script, install/uninstall scripts, TAP setup |
| `kdriver/` | Shelved kernel WiFiCx driver (experimental, not shipped) |
