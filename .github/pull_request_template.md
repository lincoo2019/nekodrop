## What and why / 改了什么、为什么

<!-- The problem (issue link if any), the root cause, the change. / 问题（有 issue 请附链接）、根因、改动。 -->

## Tests / 测试

<!-- What you ran and on which hardware. Say what you could not verify. / 跑了什么、在什么硬件上；没能验证的部分请写明。 -->

---

### Adding or changing a Wi-Fi card? / 适配或修改网卡？

Follow [`.claude/skills/add-wifi-card/SKILL.md`](https://github.com/zheznanohana/nekodrop/blob/main/.claude/skills/add-wifi-card/SKILL.md) (also a guide for people) and [docs/ADDING-A-CARD.md](https://github.com/zheznanohana/nekodrop/blob/main/docs/ADDING-A-CARD.md). Requests for a card without code are not accepted; a card PR needs:

按 [`.claude/skills/add-wifi-card/SKILL.md`](https://github.com/zheznanohana/nekodrop/blob/main/.claude/skills/add-wifi-card/SKILL.md)（人也能直接读）和 [docs/ADDING-A-CARD.md](https://github.com/zheznanohana/nekodrop/blob/main/docs/ADDING-A-CARD.md) 来做。不接受单纯的网卡支持请求；网卡 PR 需要：

- [ ] The card: PCI ID (`VEN_xxxx&DEV_yyyy`, subsystem), family, firmware file and where it comes from (pinned commit + SHA-256) / 网卡 PCI ID、家族、固件文件及来源（锁定提交 + SHA-256）
- [ ] A diagnostic report **and** the full log from a real run (Start menu → *nekodrop - diagnostic report*): bring-up all `ok`, AWDL frames heard / 真机运行的诊断报告**和**完整日志：启动各步 `ok`、收到 AWDL 帧
- [ ] AirDrop both ways (PC → iPhone/Mac and back), with speeds; Concurrent mode tested if the PR claims it / 双向 AirDrop 并写明速度；声称支持「同时连 Wi-Fi」的需实测
- [ ] Cards that already work behave exactly as before: new paths are gated on the new family / 已支持的网卡行为不变：新代码只在新网卡上生效
- [ ] Shared iwxradio code is C++17 and warning-free under MSVC `/W4 /WX` (the kernel compile in CI) / 共用 iwxradio 代码符合 C++17，MSVC `/W4 /WX` 下无警告
- [ ] `installer/install.ps1` and `installer/report.ps1` tables byte-identical and ASCII-only; self-tests updated / 两个脚本的表逐字节一致、纯 ASCII，自检已更新
- [ ] README card table, `docs/COMPATIBILITY*.md`, CHANGELOG / README 网卡表、兼容性文档、CHANGELOG
- [ ] CI green: `build`, `report-test`, `engine-test` / CI 全绿
