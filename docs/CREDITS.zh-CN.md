# 代码来源、贡献者与许可

[English](CREDITS.md) · 简体中文 · [返回 README](../README.zh-CN.md)

## 贡献者

感谢提交代码、文档和细致测试报告的各位：

- [@BaiBaiAi](https://github.com/BaiBaiAi)：Windows → iPhone 发送不再出示客户端证书（[#13](../../../pull/13)）、AWDL 发送流水线化让大文件能传完（[#15](../../../pull/15)），以及第一份 AX210 实测报告（[#14](../../../issues/14)）。
- [@XXD051030](https://github.com/XXD051030)：Intel 网卡的蓝牙均衡共存模式，让电脑几秒内就能找到 iPhone 和 Mac，并附了一份 AX200 上的详细前后对照（[#26](../../../pull/26)）。
- [@GGBond-cer](https://github.com/GGBond-cer)：大量详细的 AX200 实测报告，定位 A-MPDU 和速度问题（[#4](../../../issues/4)）、卸载残留根因（[#22](../../../issues/22)），撰写传输速度测试指南（[#28](../../../pull/28)），修复卸载清理竞态（[#38](../../../pull/38)），实现升级后自动删除安装包（[#44](../../../pull/44)）、多屏不同缩放下窗口不跨屏（[#49](../../../pull/49)）、更新下载实时速率显示（[#64](../../../pull/64)），以及更新完成后可选打开主面板或托盘启动（[#71](../../../pull/71)）。
- [@DaiJiequn](https://github.com/DaiJiequn)：MediaTek MT7902 支持，从 Linux mt76 移植的用户态驱动，按需模式下双向 AirDrop 可用（[#34](../../../pull/34)）；以及它的「同时连 Wi-Fi」模式，这部分共用的联网代码现在 MT7921/MT7922 也在用（[#47](../../../pull/47)）。
- [@sh17632](https://github.com/sh17632)：提出 AirDrop 风格的雷达界面和「共享」菜单方案，成为 2.0 新界面的基础（[#36](../../../issues/36)）；贡献 AX211 实测反馈（[#35](../../../issues/35)）、macOS IPv6-only mDNS 地址查询应答（[#55](../../../pull/55)）、实验性待机 BLE 广播（`NEKODROP_BLE_STANDBY=1`，[#69](../../../pull/69)），以及深入的 macOS 15 / iOS 18 AWDL 与 BLE 抓包分析（[#51](../../../issues/51)、[#67](../../../issues/67)）。
- [@YiHoooong](https://github.com/YiHoooong)：MediaTek MT7922/MT7921 用户态无线电驱动（`mtkradio`，[#40](../../../pull/40)），修复 MT7922 切道超时与 RX 过滤恢复并加入 HE80 2SS LWTBL（[#65](../../../pull/65)），并分享 `owly` 的 HE80 + MTU 1500 高吞吐实测经验。
- [@Yanlyn](https://github.com/Yanlyn)：补充 Windows + AX201 连接 eduroam PEAP 校园网文档（[#58](../../../pull/58)），以及社区 MT7927 驱动适配研究（[#57](../../../issues/57)）。
- 同时感谢所有提交完整诊断日志、帮助定位 AX211、AX411/AX1690i、BE200、RTL8852CE、MT7922 及 Apple ID 登录问题的热心测试者（[#30](../../../issues/30)、[#37](../../../issues/37)、[#42](../../../issues/42)、[#53](../../../issues/53)、[#54](../../../issues/54)、[#61](../../../issues/61)、[#62](../../../issues/62)、[#66](../../../issues/66) 等）。

## 代码来源

nekodrop 是在其他开源项目的基础上做的。这里写清楚哪些是**直接用了源码**（复制、移植或打包），哪些只是**参考**。

### 用到的源码

| 项目 | 许可 | 用了什么 | 在 nekodrop 里的位置 |
|---|---|---|---|
| [retropineaple/ForbiddenItlwm](https://github.com/retropineaple/ForbiddenItlwm) 0.6.0（itlwm 分支，macOS） | GPL-2.0 | Intel 22000 系列驱动 `hal_iwx`：固件加载、TLV 解析、context info、收发环、固件命令、寄存器头文件 `if_iwxreg.h`。还有它的 AWDL 策略：信道跟随（44/149 的 min 规则、国家码学习）、接收 Block Ack、BA 期间关混杂、速率阶梯、主机侧单播重发、信标提示 | `iwxradio/`，从内核 C 移植成用户态 C++ 驱动 |
| ForbiddenItlwm 的固件目录 | Intel 再分发许可 | iwlwifi 固件 `iwlwifi-Qu*-hr-b0-68.ucode` | 随安装包放在 `radio\firmware\` |
| [raph559/winawdl](https://github.com/raph559/winawdl) | MIT / GPL-2.0（`radio/`） | 在 Windows 上从用户态接管 PCIe 网卡的方法：UMDF 寄存器代理驱动、用户态 DMA、radio 管道协议，以及 RTL8852CE radio 守护进程（`radio/`） | `driver/regproxy/`（支持 Intel 与 RTL8852CE 的 PCI ID）、`iwxradio/`（管道和 DMA）、`radio\nekodrop-radio-rtl.exe` |
| [raph559/devourer](https://github.com/raph559/devourer)（`winawdl/pcie-windows`） | GPL-2.0 | Realtek RTL8852CE 用户态 PCIe 驱动（`KESTREL_8852C`，内嵌固件与双向 A-MPDU 栈） | 编译打包为 `radio\nekodrop-radio-rtl.exe`（`licenses\devourer-GPL-2.0.txt`） |
| winawdl 的 `awdl/`（`awdld`） | GPL-3.0 或更新（基于 OWL） | AWDL 守护进程：OWL 的协议核心加上 Windows I/O 层和 Wintun | 从源码编译，随安装包放在 `awdl\awdld.exe` |
| winawdl 的 `airdrop/` | MIT | Python 写的 AirDrop 引擎：mDNS、HTTPS 接收端、发送端、蓝牙信标 | 放在 `airdrop\`，打了两个小补丁（`scripts/patch-engine.py`：固定实例名、备用端口） |
| [JungyoKim/WirDrop](https://github.com/JungyoKim/WirDrop) | MIT | Apple ID 登录（`appleid`、`appleid-anisette`）：Apple 登录、向 aidc.apple.com 申请 AirDrop 证书、读取 iCloud 通讯录。`installer/nekodrop_identity.py` 参照了它的身份目录格式和 owly-airdrop（MIT）的确认记录 / 哈希处理 | `appleid\appleid.exe`、`appleid\appleid-anisette.exe`，从源码编译，打了一个补丁（`scripts/patch-appleid.py`：手机号默认 +86） |
| [seemoo-lab/owl](https://github.com/seemoo-lab/owl) | GPL-3.0 | `awdld` 里的 AWDL 实现 | 经由 winawdl |
| [Wintun](https://www.wintun.net/) | Wintun 许可 | 虚拟网卡 `awdl0` | `awdl\wintun.dll`（预编译） |
| [TAP-Windows6](https://github.com/OpenVPN/tap-windows6) | GPL-2.0 / OpenVPN | Windows 上网用的“nekodrop”虚拟以太网卡 | 安装时装上（OpenVPN 签名的安装程序） |

### 参考过的项目（没有复制代码）

| 项目 | 参考了什么 |
|---|---|
| OpenBSD `iwx(4)`、Linux `iwlwifi` | 固件、UMAC 扫描、站点模式、密钥的工作方式 |
| [seemoo-lab/opendrop](https://github.com/seemoo-lab/opendrop) | AirDrop 协议（`/Discover`、`/Ask`、`/Upload`、mDNS 记录） |
| [microsoft/Windows-driver-samples](https://github.com/microsoft/Windows-driver-samples) | 已搁置的内核驱动（`kdriver/`）的 WiFiCx 结构 |
| [t4t5/owly](https://github.com/t4t5/owly)、[`YiHoooong/owly`](https://github.com/YiHoooong/owly/tree/feat/windows-mtkradio)、[JungyoKim/WirDrop](https://github.com/JungyoKim/WirDrop) | 其他平台上实现 AirDrop 的方案，设计时对比参考（含 HE80 + MTU 1500 高吞吐思路） |
| [ndpyzwy-0w0/DropBridge](https://github.com/ndpyzwy-0w0/DropBridge) | 交互体验参考：系统通知大图预览 + 一键复制剪贴板（Bitmap + FileDrop 双格式复制与目录边界校验）以及 HEIC / Live Photo 自动伴生 JPG 副本 |

### nekodrop 自己写的部分

- `iwxradio/`：用户态移植本身，以及所有 Windows 特有的部分：两个 MAC 同时运行（AWDL 监听加上独立 PHY 上的 Wi-Fi 站点）、4+4 MiB 无锁共享内存数据面、8 KiB 接收 DMA、WPA2 站点模式（四次握手、密钥交给固件、TLC 速率控制）、TAP 桥接、固件 UMAC 扫描、信标解析和加密方式识别。
- `app/service/`：Windows 服务：网卡接管、radio 和 awdld 监管、Wi-Fi 管理（DPAPI 加密保存的网络、自动连接、隐藏网络、网页登录识别）。
- `app/client/`：托盘程序（WPF）、通知、实时传输速率显示、发送窗口、Wi-Fi 窗口、Apple ID 日志脱敏导出、中英文界面。
- `app/shellext/`：Windows 11 右键菜单。
- `installer/`：Inno Setup 安装包、本机驱动签名、TAP 配置、诊断报告（`report.ps1`）、BLE 信标（`airdrop-beacon.ps1`）和卸载。

## 许可

nekodrop 整体以 **GPL-2.0** 发布（见 [LICENSE](../LICENSE)），因为 `iwxradio/` 派生自 itlwm/ForbiddenItlwm。

- `if_iwxreg.h`：BSD/GPL-2.0 双许可。
- `driver/regproxy` 和 AirDrop 引擎：沿用 winawdl 的 MIT 许可。
- `nekodrop-radio-rtl.exe`（`raph559/winawdl` `radio/` + `raph559/devourer`）：GPL-2.0（`licenses\devourer-GPL-2.0.txt`）。
- `awdld`：GPL-3.0 或更新，来自 winawdl/OWL，是通过管道通信的独立程序。
- iwlwifi 固件：Intel 再分发许可（`licenses\iwlwifi-firmware.txt`）。
- Wintun 和 TAP-Windows：各按其许可。

安装包会把所有许可证复制到 `licenses\`。
