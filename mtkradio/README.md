# mtkradio — MediaTek MT7922 / MT7921 user-mode radio for nekodrop

Experimental user-mode PCIe driver for **MediaTek MT7922 (`14C3:0616` / `14C3:7922`)** and **MT7921 (`14C3:7961` / `14C3:0608`)** on Windows, speaking the exact same `\\.\pipe\winawdl-radio` protocol (`radio_link.h` v3) as `iwxradio`.

## What's included

- [`../driver/regproxy/winawdl_regproxy.inf`](../driver/regproxy/winawdl_regproxy.inf): updated with MediaTek MT7922/MT7921 PCI IDs (`PCI\VEN_14C3&DEV_0616` etc.).
- [`src/mtk_regs.h`](src/mtk_regs.h): MT7921/MT7922 fixed register map, L1 remap, WFDMA0 ring/config bits, Connac2 MCU & TXD/RXD/TXP structs (ported from Linux `mt76`).
- [`src/mtk_dma.h`](src/mtk_dma.h) / [`src/mtk_dma.cpp`](src/mtk_dma.cpp): 32-bit physical-address AWE DMA pool (`pa < 4 GiB`, matching `mt7921/pci.c` `DMA_BIT_MASK(32)`).
- [`src/mtk_dev.h`](src/mtk_dev.h) / [`src/mtk_dev.cpp`](src/mtk_dev.cpp): interrupt-free polling WFDMA engine, driver-own handshake, ROM patch + RAM firmware loader, channel switch, raw 802.11 TX/RX, second own MAC (BSS 1), AP STA record / RX BA, and TX-free completion handling.
- [`src/mtk_station.h`](src/mtk_station.h) / [`src/mtk_station.cpp`](src/mtk_station.cpp): `StaDevice` (`iwxradio/src/sta_device.h`) implementation driving `iwxradio`'s `WifiStation` with host-side CCMP (`../mtradio/src/ccmp.*`), AP STA record, and per-TID RX BA reorder buffer.
- [`src/mtk_main.cpp`](src/mtk_main.cpp): `--probe`, `--boot-test`, `--sta-config <file>` (experimental 5 GHz Concurrent mode with single-channel time-sharing and Wi-Fi scans), `--country <CC>`, and full `\\.\pipe\winawdl-radio` daemon mode.
- `firmware/`: `WIFI_MT7922_patch_mcu_1_1_hdr.bin` and `WIFI_RAM_CODE_MT7922_1.bin`.

## Testing on Windows (Administrator PowerShell)

1. **Build `winawdl_regproxy.dll` + `nekodrop-radio-mtk.exe`**:
   ```powershell
   .\scripts\fetch-deps.ps1
   .\driver\regproxy\build.ps1
   .\scripts\build-radio-mtk.ps1
   ```
2. **Bind `winawdl_regproxy` to the MT7922 card** (in Device Manager or via `pnputil` + `take-card.ps1`, after signing the driver package as nekodrop's installer does).
3. **Step 1 — Safe register probe**:
   ```powershell
   .\build\mtkradio\nekodrop-radio-mtk.exe --probe
   ```
   Expected output: `MTK chip_id=0x00007922 hw_rev=...` and `PROBE OK!`.
4. **Step 2 — Firmware boot & 10-second AWDL sniff on ch 149** (open iPhone share sheet on AirDrop Everyone):
   ```powershell
   .\build\mtkradio\nekodrop-radio-mtk.exe --boot-test 149 --verbose
   ```
   Expected output: `MT7922 firmware booted and ready!` followed by non-zero `total_80211` and `awdl_frames`.
5. **Step 3 — Run with `awdld` / `nekodrop`**:
   ```powershell
   .\build\mtkradio\nekodrop-radio-mtk.exe 149 --width 80 --verbose
   ```
   Then start `awdld` / `nekodrop` against `\\.\pipe\winawdl-radio`.
