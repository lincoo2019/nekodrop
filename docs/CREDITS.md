# Credits, contributors and license

English · [简体中文](CREDITS.zh-CN.md) · [Back to README](../README.md)

## Contributors

Thanks to everyone who contributed code, documentation, and detailed hardware test reports:

- [@BaiBaiAi](https://github.com/BaiBaiAi): Windows → iPhone sending without a client certificate ([#13](../../../pull/13)), pipelined AWDL transmit so large uploads finish ([#15](../../../pull/15)), and the first AX210 test report ([#14](../../../issues/14)).
- [@XXD051030](https://github.com/XXD051030): the balanced Bluetooth coexistence mode for Intel cards, so the PC finds iPhones and Macs in seconds instead of minutes, with a careful before/after study on an AX200 ([#26](../../../pull/26)).
- [@GGBond-cer](https://github.com/GGBond-cer): detailed AX200 test reports that pinned down A-MPDU and speed issues ([#4](../../../issues/4)), uninstaller leftover root cause ([#22](../../../issues/22)), the throughput testing guide ([#28](../../../pull/28)), uninstaller cleanup fix ([#38](../../../pull/38)), deleting the downloaded installer after an update ([#44](../../../pull/44)), keeping the panel and file dialogs on the cursor's screen on mixed-DPI monitors ([#49](../../../pull/49)), live download speed in Settings updates ([#64](../../../pull/64)), and choosing main panel or tray launch after an update ([#71](../../../pull/71)).
- [@DaiJiequn](https://github.com/DaiJiequn): MediaTek MT7902 support, a user-mode port of Linux mt76 that runs AirDrop both ways on demand ([#34](../../../pull/34)), and its Concurrent mode (Wi-Fi beside AirDrop with the router's A-MPDUs), whose shared station code MT7921/MT7922 now use too ([#47](../../../pull/47)).
- [@sh17632](https://github.com/sh17632): the AirDrop-style radar and Share sheet design that became the 2.0 interface ([#36](../../../issues/36)), AX211 test reports ([#35](../../../issues/35)), IPv6-only mDNS answers for macOS ([#55](../../../pull/55)), experimental standby BLE beacon (`NEKODROP_BLE_STANDBY=1`, [#69](../../../pull/69)), and deep macOS 15 / iOS 18 AWDL & BLE packet analysis ([#51](../../../issues/51), [#67](../../../issues/67)).
- [@YiHoooong](https://github.com/YiHoooong): MediaTek MT7922/MT7921 user-mode radio (`mtkradio`, [#40](../../../pull/40)), MT7922 channel-switch timeout / RX filter recovery and HE80 2SS LWTBL ([#65](../../../pull/65)), and sharing `owly` HE80 + MTU 1500 throughput findings.
- [@Yanlyn](https://github.com/Yanlyn): documented eduroam PEAP setup on Windows with AX201 ([#58](../../../pull/58)) and community MT7927 bring-up research ([#57](../../../issues/57)).
- Plus everyone who shared full diagnostic logs to help track down real-world bugs across AX211, AX411/AX1690i, BE200, RTL8852CE, MT7922, and Apple ID sign-in ([#30](../../../issues/30), [#37](../../../issues/37), [#42](../../../issues/42), [#53](../../../issues/53), [#54](../../../issues/54), [#61](../../../issues/61), [#62](../../../issues/62), [#66](../../../issues/66)).

## Where the code comes from

nekodrop is built on other open-source projects. This section says exactly what was **used as source code** (copied, ported or bundled) and what was only **studied as a reference**.

### Source code used

| Project | License | What nekodrop takes from it | Where it ends up |
|---|---|---|---|
| [retropineaple/ForbiddenItlwm](https://github.com/retropineaple/ForbiddenItlwm) 0.6.0 (itlwm fork, macOS) | GPL-2.0 | The Intel 22000-series driver `hal_iwx`: firmware loading, TLV parsing, context info, TX/RX rings, firmware commands, the register header `if_iwxreg.h`. Its AWDL strategies too: channel following (the 44/149 min rule, country-code learning), receive Block Ack, promiscuous off during BA, rate stepping, host-side unicast resend, beacon hints | `iwxradio/`, ported from kernel C to a user-mode C++ driver |
| ForbiddenItlwm's firmware folder | Intel redistribution license | iwlwifi firmware `iwlwifi-Qu*-hr-b0-68.ucode` | shipped in `radio\firmware\` |
| [raph559/winawdl](https://github.com/raph559/winawdl) | MIT / GPL-2.0 (`radio/`) | The way to take over a PCIe Wi-Fi card from user mode on Windows: the UMDF register-proxy driver, user-mode DMA, the radio pipe protocol, and the RTL8852CE radio daemon (`radio/`) | `driver/regproxy/` (Intel + RTL8852CE PCI IDs), `iwxradio/` (pipe and DMA), `radio\nekodrop-radio-rtl.exe` |
| [raph559/devourer](https://github.com/raph559/devourer) (`winawdl/pcie-windows`) | GPL-2.0 | Realtek RTL8852CE user-mode PCIe driver (`KESTREL_8852C`, embedded firmware and A-MPDU stack) | built into `radio\nekodrop-radio-rtl.exe` (`licenses\devourer-GPL-2.0.txt`) |
| winawdl `awdl/` (`awdld`) | GPL-3.0-or-later (builds on OWL) | The AWDL daemon: OWL's protocol core with a Windows I/O layer and Wintun | built from source, shipped as `awdl\awdld.exe` |
| winawdl `airdrop/` | MIT | The AirDrop engine in Python: mDNS, the HTTPS receiver, the sender, the BLE beacon | shipped as `airdrop\` with two small patches (`scripts/patch-engine.py`: stable instance name, spare ports) |
| [JungyoKim/WirDrop](https://github.com/JungyoKim/WirDrop) | MIT | Its Apple ID sign-in, `appleid` and `appleid-anisette`: Apple's sign-in, the AirDrop certificate from aidc.apple.com, the iCloud contacts. Its identity layout and the record / hash handling of owly-airdrop (MIT) guided `installer/nekodrop_identity.py` | `appleid\appleid.exe`, `appleid\appleid-anisette.exe`, built from source with one patch (`scripts/patch-appleid.py`: phone numbers default to +86) |
| [seemoo-lab/owl](https://github.com/seemoo-lab/owl) | GPL-3.0 | The AWDL implementation inside `awdld` | through winawdl |
| [Wintun](https://www.wintun.net/) | Wintun license | The `awdl0` virtual interface | `awdl\wintun.dll` (prebuilt) |
| [TAP-Windows6](https://github.com/OpenVPN/tap-windows6) | GPL-2.0 / OpenVPN | The "nekodrop" virtual Ethernet adapter for Windows' internet | installed by the setup (signed OpenVPN installer) |

### Studied as references (no code copied)

| Project | What it was used for |
|---|---|
| OpenBSD `iwx(4)`, Linux `iwlwifi` | How the firmware, UMAC scan, station mode and keys work |
| [seemoo-lab/opendrop](https://github.com/seemoo-lab/opendrop) | The AirDrop protocol (`/Discover`, `/Ask`, `/Upload`, mDNS records) |
| [microsoft/Windows-driver-samples](https://github.com/microsoft/Windows-driver-samples) | WiFiCx structure for the shelved kernel driver (`kdriver/`) |
| [t4t5/owly](https://github.com/t4t5/owly), [`YiHoooong/owly`](https://github.com/YiHoooong/owly/tree/feat/windows-mtkradio), [JungyoKim/WirDrop](https://github.com/JungyoKim/WirDrop) | Other AirDrop-on-other-platforms implementations, compared during design (including HE80 + MTU 1500 high-throughput reference) |
| [ndpyzwy-0w0/DropBridge](https://github.com/ndpyzwy-0w0/DropBridge) | UX design references: system toast hero preview + "Copy to clipboard" button (with Bitmap + FileDrop dual-format copy and received-folder boundary check) and automatic sibling JPG copy for HEIC / Live Photos |

### Written for nekodrop

- `iwxradio/`: the user-mode port itself, plus everything Windows-specific: running two MACs at once (AWDL listener plus Wi-Fi station on its own PHY), lock-free 4+4 MiB SHM data plane, 8 KiB RX DMA, station mode with WPA2 (4-way handshake, keys in firmware, TLC rate control), bridging to TAP, firmware UMAC scan, beacon parsing and security detection.
- `app/service/`: the Windows service: card ownership, radio and awdld supervision, the Wi-Fi manager (DPAPI-encrypted saved networks, auto-connect, hidden networks, captive-portal detection).
- `app/client/`: the tray app (WPF), notifications, live transfer speed display, the send window, the Wi-Fi window, Apple ID log export, Chinese/English UI.
- `app/shellext/`: the Windows 11 context-menu handler.
- `installer/`: the Inno Setup installer, the on-PC driver signing, TAP setup, diagnostic report (`report.ps1`), BLE beacon (`airdrop-beacon.ps1`), and uninstall.

## License

nekodrop as a whole is **GPL-2.0** (see [LICENSE](../LICENSE)), because `iwxradio/` derives from itlwm/ForbiddenItlwm.

- `if_iwxreg.h`: dual BSD/GPL-2.0.
- `driver/regproxy` and the AirDrop engine: keep winawdl's MIT license.
- `nekodrop-radio-rtl.exe` (`raph559/winawdl` `radio/` + `raph559/devourer`): GPL-2.0 (`licenses\devourer-GPL-2.0.txt`).
- `awdld`: GPL-3.0-or-later, from winawdl/OWL. It is a separate program talking over a pipe.
- iwlwifi firmware: Intel's redistribution license (`licenses\iwlwifi-firmware.txt`).
- Wintun and TAP-Windows: their own licenses.

The installer copies every license into `licenses\`.
