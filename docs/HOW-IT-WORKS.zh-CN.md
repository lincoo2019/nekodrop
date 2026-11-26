# 工作原理、进度与构建

[English](HOW-IT-WORKS.md) · 简体中文 · [返回 README](../README.zh-CN.md)

## 工作原理

```
 nekodrop.exe（托盘程序，用户会话）──JSON 管道──► NekoDropService（SYSTEM）
   │  AirDrop 引擎（Python）                        │ 接管 / 归还网卡
   │  通知 · 右键菜单 · 发送窗口                       │ Wi-Fi 管理、看门狗
   │                                                ▼
   └── awdl0（Wintun 8 MiB，IPv6）◄── awdld（AWDL，OWL）◄─SHM/管道─ nekodrop-radio（用户态驱动）
                                                         ├ MAC 0：AWDL，信道 6/44/149
     “nekodrop” TAP 网卡 ◄── 以太网桥接 ◄─────────────────┤ MAC 1：站点模式，连你的路由器
     （Windows 上网用）                                   └ UMDF 寄存器代理 + 用户态 DMA → 无线网卡
```

1. **寄存器代理**：一个很小的 UMDF 驱动（`driver/regproxy`）代替厂商驱动绑定网卡。它只负责把网卡寄存器和中断映射给一个用户态进程，我们自己的代码不在内核里运行。
2. **用户态驱动**：Intel 网卡用的 `nekodrop-radio`（`iwxradio/`）是把 OpenBSD/itlwm 的 `iwx` 驱动移植到用户态。它通过用户态 DMA（8 KiB 接收缓冲 + 与 `awdld` 的 4+4 MiB 无锁共享内存环）加载 Intel 固件，然后同时跑两个 MAC：一个监听和注入 AWDL 帧，另一个是普通的 Wi-Fi 站点，支持 WPA2（四次握手、密钥交给固件、速率控制）。Realtek 和 MediaTek 的 radio 为各自的芯片做同样的事，Wi-Fi 站点的代码是共用的。
3. **AWDL**：`awdld`（OWL，Open Wireless Link）通过共享内存 / radio 管道实现苹果的 AWDL 协议，把链路做成 IPv6 网卡 `awdl0`（8 MiB Wintun 环、`ev_win.c` 公平轮询、TCP GRO，可选 HE80 + MTU 1500）。
4. **AirDrop**：Python 引擎在 `awdl0` 上实现 AirDrop：mDNS 发现（含 macOS IPv6 应答），`8771` 端口上的 HTTPS（备用段 `8772-8779`、`18771-18779`、`28771-28779`），流式 DVZip→CPIO 解包与 4 MiB 异步落盘，`/Discover`、`/Ask`、`/Upload` 流程，以及带传输后自动重脉冲唤醒的蓝牙信标。
5. **上网**：站点 MAC 收发的帧桥接到“nekodrop” TAP 网卡，Windows 把它当成以太网，用 DHCP 拿地址。

设计细节：[docs/DESIGN.md](DESIGN.md)（驱动、DMA、风险）· [docs/CLIENT.md](CLIENT.md)（服务、客户端、联网模式）· [docs/TESTING.md](TESTING.md)（传输速度测试方法，英文）。

## 进度

| 部分 | 状态 |
|---|---|
| 用户态 Intel 驱动：固件启动、监听、注入、扫描 | ✅ AX201、AX200、AX210、AX211、AX411/AX1690i、9560 上可用（后五者为用户反馈） |
| iPhone → Windows | ✅ 可用；AX201 实测净收最高 **50.81 MiB/s（53.28 MB/s）**、端到端 **47.36 MiB/s**（`v2.2.9+`，0 丢包） |
| Windows → iPhone | ✅ 可用；AX201 实测最高 **50.55 MiB/s（53.00 MB/s）**（`v2.2.9+`，BA 确认率 99.85%，0 丢包） |
| 联网同时 AirDrop | ✅ 可用（WPA2、DHCP、DNS 都走“nekodrop”网卡） |
| Wi-Fi 管理、已保存网络、网页登录识别 | ✅ |
| 服务、托盘程序、通知、右键菜单、安装包 | ✅ |
| 托盘面板、雷达、「共享」菜单、自定义快捷键、实时传输速率 | ✅ `v2.2.10` |
| 仅限联系人（Apple ID 登录、日志导出或 Mac 证书） | ✅ 实验性（`v2.1.0+`，`v2.2.9` 补齐 `-27952`/2FA 重试与日志脱敏导出） |
| 纯 WPA3、OWE | 🧪 0.1.12，未实机测试 |
| 802.1X（企业版） | ⏳ 计划中（eduroam PEAP 方案见 [#58](../../../pull/58)） |
| AX200、AX210、AX211、AX411 / Killer AX1690i | ✅ 用户反馈双向 AirDrop 可用（[#1](../../../issues/1)、[#14](../../../issues/14)、[#24](../../../issues/24)、[#37](../../../issues/37)） |
| Qu 上的 9560 | ✅ 用户实测可用（[#45](../../../issues/45)） |
| So 上的 9560、AX203 | 🧪 已照上游移植，还没有用户反馈 |
| Realtek RTL8852CE | ✅ 用户实测双向可用，峰值约 **24.5 MB/s**（[#61](../../../issues/61)） |
| MediaTek MT7902 | ✅ 双向 AirDrop 和同时连 Wi-Fi，作者实测（[#34](../../../pull/34)、[#47](../../../pull/47)） |
| MediaTek MT7922 / MT7921 | 🧪 MT7922 按需模式已实测（[#40](../../../pull/40)）；`v2.2.8`–`v2.2.9` 已补齐 HE80 2SS、扫描 `FIF_BIT_CLR`、固件 RA 与 RX A-MSDU 修复（[#65](../../../pull/65)、[#66](../../../issues/66)） |
| Intel BE200（Wi-Fi 7） | 🧪 实验性：`v2.2.9` 加固 Bz/MLD 4096 字节页对齐与双 LMAC 错误转储（[#53](../../../issues/53)、[docs/BE200.md](BE200.md)） |
| Windows 10 | ✅ 用户反馈 0.1.16 及以上双向 AirDrop 可用（[#18](../../../issues/18)） |
| 内核 WiFiCx 驱动（`kdriver/`） | 🧊 已搁置：实验性，安装包**不会安装**它，见 [docs/KERNEL.md](KERNEL.md) |

## 从源码构建

需要 Visual Studio 2022 Build Tools（C++、Windows SDK、CMake、vcpkg）、.NET 8 SDK 和 Git。Inno Setup 6 可选，没装就输出 zip。不用装 WDK，脚本会用它的 NuGet 包。

```powershell
git clone https://github.com/zheznanohana/nekodrop.git
cd nekodrop
.\scripts\fetch-deps.ps1        # 下载 winawdl、devourer、ForbiddenItlwm（固件）、MinGW、WDK NuGet 到 third_party\
.\scripts\build-installer.ps1   # 全部构建 → build\nekodrop-<version>-setup.exe
.\scripts\build-radio.ps1       # 只构建 Intel 用户态驱动并跑离线自检
.\scripts\build-radio-rtl.ps1   # 只构建 Realtek RTL8852CE radio（devourer）并跑离线单测
```

| 目录 | 内容 |
|---|---|
| `iwxradio/` | 用户态 Intel 驱动（C++）：固件、AWDL 监听/注入、站点模式、WPA2、UMAC 扫描 |
| `driver/regproxy/` | UMDF 寄存器代理驱动 |
| `app/service/` | NekoDropService（C#）：网卡接管、radio 监管、Wi-Fi 管理 |
| `app/client/` | 托盘程序（C# WPF）：界面、通知、运行 AirDrop 引擎 |
| `app/shellext/` | Windows 11 右键菜单 |
| `installer/` | Inno Setup 脚本、安装/卸载脚本、TAP 配置 |
| `kdriver/` | 已搁置的内核 WiFiCx 驱动（实验性，不随安装包发布） |
