# 常见问题、一键回退与诊断报告

[English](SUPPORT.md) · 简体中文 · [返回 README](../README.zh-CN.md)

## 常见问题

| 现象 | 试试 |
|---|---|
| iPhone 搜不到电脑 | 把 iPhone 的 AirDrop 设为“所有人（10 分钟）”（这台电脑有 Apple ID 身份时也可以用“仅限联系人”），关掉再打开（清掉缓存的旧记录），手机解锁并放在附近 |
| 电脑找 iPhone 很慢 | 确认蓝牙是开着的：电脑靠蓝牙信标叫醒 iPhone。发不出信标时，诊断报告的引擎日志里会有 `beacon: false` 和原因 |
| 发给 iPhone 很慢 | nekodrop 连的 5 GHz Wi-Fi 和 AirDrop（44 或 149）不在同一信道时会变慢；在 nekodrop 里断开 Wi-Fi，或者把路由器改到那个信道会好很多 |
| nekodrop 连上 Wi-Fi 后 AirDrop 就不行了 | 路由器的 5 GHz 信道和 AirDrop（44 或 149）不一样：打开 *设置 → 实验性功能 →「Wi-Fi 和 AirDrop 不在同一信道时按苹果的方式分时」*；电脑插着网线的话，也可以在 nekodrop 里断开 Wi-Fi（[#35](../../../issues/35)） |
| iPhone 上出现两个电脑 | 之前运行留下的缓存记录，在 iPhone 上把 AirDrop 关掉再打开 |
| 没网 | 托盘 → **Wi-Fi…**，确认已连接。网络可能需要网页登录，或者是纯 WPA3 / 企业网（见上表） |
| Windows 的 Wi-Fi 图标没了 | 同时模式下是正常的：网卡归 nekodrop 管。用 **Wi-Fi…**，或者切到“按需” |
| 同时模式下某些软件或游戏登录不了、不显示二维码 | 有些软件要求系统里有 Windows 的无线网卡，而同时模式下没有（nekodrop 的网卡是有线类型）。用这类软件时请切到“按需”（[#33](../../../issues/33)） |
| 其他问题 | 提 [Issue](../../../issues)，并附上[诊断报告](#反馈问题诊断报告) |

## 出问题了？一键回退

开始菜单 → **nekodrop - restore Windows Wi-Fi**（或双击 `C:\Program Files\nekodrop` 里的 `restore.cmd`）。它是独立脚本（即使安装目录损坏也能运行），会停掉 nekodrop，解除 Intel 硬编码、按网卡厂商归还原厂驱动（Intel 网卡行为保持不变），删除 nekodrop 的驱动包并恢复 Windows 自带的 Wi-Fi，不管 nekodrop 把网卡留在什么状态。nekodrop 本身不会被卸载，想再用时重新运行安装程序即可。日志：`C:\ProgramData\nekodrop\restore.log`。

## 反馈问题：诊断报告

日志是我们修问题的依据，尤其是我们手上没有的网卡。提 [Issue](../../../issues) 时请务必附上。

1. 开始菜单 → **nekodrop - diagnostic report**（或托盘图标 → **生成诊断报告…**，或主窗口底部的 **诊断报告**）。会弹出管理员权限提示，请允许，因为日志和驱动状态只有管理员能读。
2. 桌面上会生成两个文件，报告会用记事本打开：
   - `nekodrop-report-<时间>.md`：摘要、最近一次 radio 启动的逐步检查（驱动绑定 → 固件 → PNVM → 初始化 → 就绪）、系统、网卡、驱动、网络和蓝牙信息、发现设备的时间线（含发现过程每一步用了多少秒），以及关键日志行。它也已经复制到剪贴板，**直接粘贴到 issue 里**即可。
   - `nekodrop-logs-<时间>.txt`：完整日志，**拖进 issue 作为附件**。
3. SSID、MAC 地址、内网 IP、用户名和电脑名都会替换成 `<SSID-1>`、`<MAC-2>` 这样的占位符，不会上传任何东西。发出前请先看一眼报告内容。

**旧版本没有这个入口时**，可以下载 [`installer/report.ps1`](../installer/report.ps1)，在命令提示符里运行：`powershell -ExecutionPolicy Bypass -File report.ps1`。也可以手动收集日志：

| 文件 | 内容 |
|---|---|
| `C:\ProgramData\nekodrop\service.log`（以及 `service.log.1`） | 服务、radio（固件、AWDL、Wi-Fi）和 awdld 的日志，需要管理员权限才能读 |
| `%LocalAppData%\nekodrop\engine.log`（以及 `engine.log.old`） | AirDrop 引擎：发现设备、蓝牙信标、收发文件 |
| `%LocalAppData%\nekodrop\appleid\appleid.log` | Apple ID 登录与 Anisette 辅助日志（`report.ps1` 自动脱敏收录，亦可在 设置 → Apple ID 一键导出） |
| `C:\ProgramData\nekodrop\install.log` | 安装过程：驱动、TAP 网卡、防火墙、服务 |
| `C:\ProgramData\nekodrop\restore.log` | 一键回退 |
