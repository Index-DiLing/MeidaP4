# 探测阶段（批 0）归档清单

> 本目录是 ESP32-P4 WebUSB 复合设备方案**可行性探测阶段**的全部一次性产物。
> 按用户要求："把这次测试的文件全部归档，只留有用的经验和有效代码。"
>
> **保留在仓库里的是**：可编译的代码（`components/`、`main/`、`web/`）、可复用工具（`tools/`）、
> 以及经验文档（`docs/`）。**归档到这里的是**：构建产物、每个写入者槽位的生成配置、以及离线参考源码。
> 归档内容**可以安全删除**（用下面的命令可完整重建）。

---

## 1. 目录内容

| 子目录 | 内容 | 重建方式 |
|---|---|---|
| `builds/` | 探测期 14 个 `build-<slot>-<mode>` 目录（含**冻结版** `build-final-a/b`）+ 探测前遗留的 `build.generic-template.bak` | `pwsh tools/build.ps1 -Mode <a\|b> -Slot <slot>` |
| `sdkconfigs/` | 各槽位生成的 `sdkconfig.<slot>-<mode>`（14 个）+ 探测前的 `sdkconfig.template-backup` | 同上（由 `sdkconfig.defaults` + `sdkconfig.<a\|b>.defaults` 生成） |
| `reference-components/` | 从本机组件缓存解压的 `esp_tinyusb 2.3.0` 与 `tinyusb 0.21.0~2` 源码树，供**离线**查 API/宏 | 从 `C:\Espressif\tools\components\espressif\{esp_tinyusb\2.3.0,tinyusb\0.21.0~2}\*.zip` 解压 |

> ⚠️ 本目录里的 ELF（如 `builds/build-final-*/MeidaP4.elf`）是**冻结版**的构建产物，
> 可用 `pwsh -File tools/verify-offline.ps1` 直接做描述符静态校验；若已删除，先按上表重建。

---

## 2. 有用的经验在哪

**不在本目录**，而在仓库的 `docs/`（这些是这次探测真正的产出）：

| 文件 | 作用 |
|---|---|
| [probe-report.md](../../docs/probe-report.md) | **实测总报告**：4 条 plan 级修正 + 全部真机数字 + 未验证清单 |
| [probe-contract.md](../../docs/probe-contract.md) | 冻结契约（描述符身份、端点表、组件接口、线协议 v0、构建命令） |
| [probe-plan.md](../../docs/probe-plan.md) | 探测阶段范围与验收标准 + 真机追加结论 |
| [probe-verify.md](../../docs/probe-verify.md) | 独立复核报告（ELF 反解、对抗性核对、存疑清单） |
| [host-verify.md](../../docs/host-verify.md) | Win11 + Chrome/Edge 分步验收清单与失败判读 |
| [host-usb-test.md](../../docs/host-usb-test.md) | 主机侧 WinUSB 工具用法 + GUID 证据表 + 踩坑记录 |

---

## 3. 归档时的关键实测数字（便于以后对照，不必翻日志）

| 项 | 值 |
|---|---|
| 芯片 / 速率 | ESP32-P4 rev v3.2，双核+LP 400 MHz，**HS（High Speed）** 口 |
| Flash / PSRAM | 32 MB / **32 MB @250 MHz**（`probe: PSRAM total=33554432 B`） |
| 烧写 | `COM16`（CH343 UART 桥，**不是 JTAG**）@ **921600**，266 KB / 2.1 s |
| 分区表 | `nvs 0x9000/24K`、`phy_init 0xf000/4K`、`factory 0x10000/4M`、`storage 0x410000/4M` |
| 模式 A 描述符 | FS/HS cfg **32 B**，BOS **57 B**，MS OS 集 **158 B**，Vendor IF0 EP `0x01/0x81` |
| 模式 B 描述符 | FS/HS cfg **62 B**，BOS **57 B**，MS OS 集 **166 B**，MTP IF0(EP1 int + EP2 bulk) + Vendor IF1(EP3) |
| 端点预算 | 模式 A = 1 IN(+EP0=2/8)；模式 B = 3 IN(+EP0=4/8)；**模式 B + NCM = 5 IN(+EP0=6/8，余 2)**；仅 HS 口可行（FS 口 IN 上限 5） |
| MTP 卷 | 片内 `storage` FATFS，可用 **4,116,480 B (4020 KiB)** |
| 回环（WinUSB 主机侧） | 1 KB/64 KB/1 MB **失配全 0**；1 MB ≈ **51.5 Mbit/s**；设备侧 `out == in == 1,115,136 B / 2178 pkt`（逐包精确） |
| Chrome | `file://` 下 `requestDevice` + **`claimInterface` 成功**；用户报告 1 KB/64 KB/1 MB 各两次通过 |
| 心跳积压 | 无人读 IN 时每秒 1 行；35.8 s 空闲积压 **332 B**（修复前曾达 374 行 ≈ 4 KB） |

---

## 4. 归档范围之外

- `E:\ESP_PROJECT\dsh-acl-recovery\`（**未归档、未删除**）：本会话开始时的工作区文件权限修复记录（含可回滚脚本），与本次探测无关，保留以便追溯。
- `managed_components/`、`dependencies.lock`：**留在仓库**。前者是构建输入（由组件管理器生成，可重新解析），后者是版本锁，应当提交。
