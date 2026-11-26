# nekodrop：Windows 客户端与服务设计

目标：像 Windows 自带功能一样用 AirDrop。装一次，之后不再弹 UAC；平时 Wi-Fi 照常上网；收文件走系统通知，发文件走资源管理器右键菜单。

## 1. 进程结构

```
┌───────────── 用户会话 ─────────────┐        ┌──────────── 会话 0（SYSTEM）────────────┐
│ nekodrop.exe（托盘客户端，WPF）     │ 管道   │ NekoDropService（Windows 服务）          │
│ · 托盘图标、发送窗口、设置          │◄──────►│ · 网卡归属：Intel 驱动 ⇄ 代理驱动         │
│ · 系统通知（接受/拒绝）             │\\.\pipe│ · 启停 nekodrop-radio / awdld / 引擎     │
│ · 右键“用 nekodrop 发送”入口        │\nekodrop│ · 信道/国家码策略（6 → 44/149）          │
│ · 蓝牙广播监听（按需唤醒）          │        │ · 收发任务、进度、已接收文件              │
└────────────────────────────────────┘        └─────────────────────────────────────────┘
```

- 服务以 LocalSystem 运行：锁页内存（DMA）、切换驱动、创建 Wintun/TAP 适配器、读取 Wi-Fi 配置文件，都不再需要 UAC。
- 客户端不需要管理员权限，开机随用户登录启动（`HKCU\...\Run`），只能连本机管道，管道 ACL 只允许交互用户和 SYSTEM。

## 2. 联网与 AirDrop 的关系（三种模式，设置里可选）

| 模式 | 行为 | 代价 |
|---|---|---|
| **同时**（默认，支持时） | 用户态驱动同时开两个 MAC 上下文：一个 BSS_STA 连你的路由器，一个 LISTENER 做 AWDL，固件分时切换信道（与 ForbiddenItlwm 的 coex 模式相同）。Windows 看到的网络是一个 TAP 以太网适配器，DHCP/ARP 由系统自己完成 | 只支持 WPA2-PSK / 开放网络；WPA3、企业网退回“按需” |
| 按需 | 平时网卡在 Intel 驱动下正常上网；手机打开 AirDrop 时（蓝牙广播）或你点“发送”时，服务接管网卡，传完自动还回 | 传输期间断网几十秒 |
| 关闭 | 网卡始终归 Intel 驱动 | — |

“同时”模式的连接参数（SSID、BSSID、信道、密码）在接管网卡前，由服务通过 WLAN API 从 Windows 当前连接和配置文件里读出，用户不需要重新输入。

## 3. 界面（遵循 Windows 11 设计规范）

- **托盘图标**：状态用图标变化表示（空闲 / 可被发现 / 传输中）。左键打开发送窗口，右键菜单：
  - 打开 nekodrop
  - 接收：关闭 / 仅限联系人（2.1.0 起，需在 设置 → Apple ID 打开实验开关并有 Apple ID 身份）/ 所有人 10 分钟
  - 设置… / 退出
- **发送窗口**：Mica 背景、系统强调色、跟随浅色/深色主题、圆角。上半部分是附近设备，下半部分是待发送文件（可拖入）。点设备开始发送，进度显示在设备卡片上。
- **接收**：系统通知（Toast），标题“iPhone 想发送 2 张照片”，按钮“接受 / 拒绝”。点按钮用 `nekodrop:` 协议激活客户端（未打包应用的标准做法，不需要 COM 激活器）。完成后再发一条“已保存到 下载\nekodrop”，点击打开文件夹。
- **资源管理器**：
  - 右键菜单“用 nekodrop 发送”（`HKCU\Software\Classes\*\shell\nekodrop`，支持多选）。Windows 11 中位于“显示更多选项”里；顶层菜单需要打包应用 + `IExplorerCommand`，列为后续工作。
  - “发送到 → nekodrop”快捷方式，两个入口都打开发送窗口并带上文件。
- **设置**：设备名称、保存位置、联网模式、开机启动、总是接受的设备列表。

## 4. 打包

- `nekodrop-setup.exe`（Inno Setup，按机器安装）：复制文件、签名并安装代理驱动包（不自动接管网卡）、安装并启动服务、添加防火墙规则（仅 `fe80::/64`）、开始菜单快捷方式（带 AUMID，系统通知需要）、注册右键菜单和 `nekodrop:` 协议。
- 卸载反向执行：还原网卡驱动、删除服务、防火墙规则和证书信任。
- AirDrop 协议引擎沿用 winawdl 的 Python 实现，随安装包附带 Python 嵌入式发行版，用户不需要装 Python。

## 5. 管道协议（JSON 行）

客户端 → 服务：`status`、`set_mode`、`discoverable`、`send {files, device}`、`answer {id, accept}`、`wake`。
服务 → 客户端：`state`、`devices`、`request {id, sender, files}`、`progress {id, done, total}`、`finished {id, ok, path}`。
