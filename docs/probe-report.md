# 探测阶段实测报告（批 0）

> 上位：[usb-composite-plan.md](./usb-composite-plan.md) · 范围：[probe-plan.md](./probe-plan.md) · 契约：[probe-contract.md](./probe-contract.md)
> 本文只记录**实测**结果。每一条都带可复现的命令或原始输出出处。**离线 PASS ≠ 真机通过。**

---

## 0. 状态总览

| 冒烟项 | 结论 | 证据 |
|---|---|---|
| 工程地基（构建/烧写/环境） | ✅ 通过 | §1 |
| 模式 A / 模式 B 固件干净构建 | ✅ 通过 | §1 |
| **冒烟 A** 单 Vendor + WebUSB：Windows 自动绑 WinUSB | ✅ 通过 | §2.1 |
| **冒烟 A** 单 Vendor + WebUSB：Chrome `requestDevice` + **`claimInterface`** | ✅ **通过（2026-10-16 真机实测）** | §2.5 |
| **冒烟 A** bulk 回环数据正确性 | ✅ **通过**（主机侧 WinUSB 1 KB/64 KB/1 MB 失配全 0；Chrome 侧用户报告 6/6 通过） | §2.6 / §3.4 |
| **冒烟 B** MTP + Vendor 复合：MTP 被 Windows 识别并可浏览 | ✅ **通过**（Explorer 列表 + `PROBE.txt`） | §2.4 |
| **冒烟 B** 同一时刻 vendor 接口可被网页 claim | ✅ 通过（复合形态下 Chrome claim 成功） | §2.5 |
| **冒烟 C** 端点预算离线校验 | ✅ 通过 | §4 |
| PSRAM 32 MB / HS 口 | ✅ 通过 | §1 |

---

## 1. 环境、硬件与地基（全部实测）

| 项 | 实测值 / 结论 |
|---|---|
| ESP-IDF | v6.1，`C:\esp\v6.1\esp-idf`（git HEAD `fff9895c`，Junction → `H:\.Import_CDiskLinker\esp\v6.1\esp-idf`） |
| 工具链 | `C:\Espressif\tools`（cmake 4.0.3 / ninja 1.12.1 / riscv32-esp-elf esp-15.2.0 / python v6.1 venv） |
| ⚠️ 会话继承的失效环境变量 | `IDF_PATH=E:\esp32\.espressif\v6.0.2\esp-idf`（不存在）、`IDF_TOOLS_PATH=E:\esp32\.espressif`（不存在）。`tools/idf-env.ps1` 已显式修正；**不设 `ESP_IDF_VERSION` 会让 `idf.py` 直接 Python `TypeError`** |
| 芯片 | **ESP32-P4 revision v3.2**，Dual Core + LP Core 400 MHz，Crystal 40 MHz，MAC `e8:f6:0a:e8:8a:21` |
| Flash | **32 MB**（`esptool flash-id`：Manufacturer c8 / Device 4019） |
| PSRAM | **32 MB**（串口：`hex_psram density: 0x07 (256 Mbit)`；`probe: PSRAM total=33554432 B`） |
| 枚举速率 | **High Speed**（设备侧 `USBD init on controller 1, speed = High`；`USBD Bus Reset : High Speed`） |
| 连接形态 | **UART 桥独立于 OTG**：`USB-Enhanced-SERIAL CH343 (COM16)` = VID_1A86/PID_55D3（**纯 USB-UART 桥，不是 JTAG**）；P4 的 HS OTG 是另一个物理口 |
| 烧写 | `idf.py -p COM16 -b 921600 flash` → 266,688 B / 2.1 s，hash verified。`tools/flash.ps1` 默认速率已提到 921600 |
| 构建 | 模式 A `MeidaP4.bin` 0x33840 B；模式 B 0x33940 B（骨架期）。分区表实测：`nvs 0x9000/24K`、`phy_init 0xf000/4K`、`factory 0x10000/4M`、`storage 0x410000/4M` |
| 依赖解析 | `espressif/esp_tinyusb 2.3.0` + `espressif/tinyusb 0.21.0~2`，**从本地组件缓存离线解析成功** |
| 组件版本决策 | 探测期锁 **2.3.0**（用户确认）；2.4.0 只修 NCM/ECM 默认描述符缺 IAD，与探测无关 |

---

## 2. 已真机验证通过的结论

### 2.1 R1 · Windows 自动绑 WinUSB —— ✅ 成立，**但只完成了一半**

| 设备节点 | 服务 | INF | setup class | CompatibleIds |
|---|---|---|---|---|
| `USB\VID_303A&PID_4002\PROBE-0001` | `usbccgp` | `usb.inf` | USB | `USB\COMPOSITE` … |
| `…&MI_00` | `WUDFWpdMtp` | `wpdmtp.inf` | WPD | `USB\Class_06&SubClass_01&Prot_01` |
| `…&MI_01` | **`WINUSB`** | **`winusb.inf`** | USBDevice | **`USB\MS_COMP_WINUSB`** |

- 友好名 `MeidaP4 WebUSB`；所有节点 `problem=0`；**全程未用 Zadig**。
- `USB\MS_COMP_WINUSB` 这条 compatible ID 是**手写 MS OS 2.0 描述符集被 Windows 解析并接受的直接证据**。

**设备侧同步证据**（`build-main-b/serial-b-onhost.log`）：`stats | mounted=1 out=0B/0pkt in=0B/0pkt ctrl=1 stall=0`。

### 2.2 描述符/BOS/端点（离线 + 真机双向一致）

> ⚠️ **2026-10 独立复核更正（verifier，task-4）**：本节引用的 `build-diag-b/serial-diag.log`
> 是一个**修复前的构建**。verifier 用 `tools/desc_lint.py` 逐槽位跑归档 ELF 实测：
>
> | 归档槽位 | `s_msos20_desc_set` 字节数 | 子描述符序列 | desc_lint |
> |---|---|---|---|
> | `diag-b` / `diagfix-b` / `main-b` | **38** | `0x0000→0x0002→0x0003`（**无 `0x0004` RegProp**） | 118/122（4 FAIL） |
> | `usbcore-b` / **`final-b`** / `verify-b` | **166** | `0x0000→0x0002→0x0003→0x0004` ✅ | **135/135 PASS** |
>
> 也就是说：本节日志里的 `MSOS20=38 B` / `wLength=0x26`(38) 出自**没有 Registry Property** 的 diag-b，
> 而 §2.5「RegProp 生效、Chrome claim 成功」出自 **final-b**。**两条结论来自两个不同的固件**。
> 描述符的最终形态请以 `final-b` 的 166 B 为准（ELF SHA256
> `89D0EA05372FB20F445CF2CE8B17D5CCCD9764F9337B71D1544B5FEE722D58A5`）。
> 本节的 38 B 数据仅作为"BOS/MS OS 取用发生在 `wIndex=0x0007`"这一条的**机制性证据**保留。

真机串口（`build-diag-b/serial-diag.log`，**pre-fix 构建，见上**）逐条对照：

```
USBD init on controller 1, speed = High
USBD Setup Received 80 06 00 01 00 00 40 00   GET device desc (64)
USBD Setup Received 80 06 00 01 00 00 12 00   18 bytes
USBD Setup Received 80 06 00 02 00 00 FF 00   config, 62 bytes
USBD Setup Received 80 06 00 0F 00 00 FF 00   GET BOS → 57 bytes      ✅ BOS 真的被取
USBD Setup Received C0 02 00 00 07 00 26 00   MS OS 2.0 → 38 bytes    ✅ wIndex=0x0007
USBD Setup Received 80 06 03 03 09 04 FF 00   string[3] lang 0x0409
  Open EP 81 size 64  / EP 02 512 / EP 82 512   MTP opened
  Open EP 03 512 / EP 83 512                    VENDOR opened
```

- `usb_probe: desc self-check OK | B (MTP + vendor) | IF=2 | FS cfg=62 B | HS cfg=62 B | BOS=57 B | MSOS20=38 B | vendor IF1 EP 0x03/0x83`
- 独立复算（verifier，自己的 ELF）：device 18 B、FS/HS 配置 32 B(a)/62 B(b)、BOS 57 B、MS OS 2.0 集 38 B、MTP 接口类 `0x06/0x01/0x01`、EP 分配与契约逐项一致。`tools/verify-offline.ps1` 模式 a 96/96、模式 b 108/108 PASS。

### 2.3 模式 B 的 MTP 后端（片内 FATFS）——运行时全部正常

```
mtp_probe: mount 'storage' -> '/mtp' failed: ESP_FAIL
mtp_probe: no usable FATFS on 'storage' -> FORMATTING it (CONFIG_PROBE_MTP_FORMAT_IF_NEEDED=y)
mtp_probe: partition 'storage' was FORMATTED during this boot (no FATFS found before)
mtp_probe: mounted partition 'storage' after format at '/mtp' [rw + wear_levelling]
mtp_probe: volume '/mtp': total=4116480 B (4020 KiB), free=4116480 B
mtp_probe: marker written: /mtp/PROBE.txt (482 bytes)
tinyusb_mtp: MTP driver installed
tinyusb_mtp: MTP storage registered: id=0x00010001 path=/mtp name=MeidaP4 Storage
```

- 首次无 FATFS → 格式化分支走通（用 `esptool erase-region 0x410000 0x400000` 特意制造过）；**第二次上电不再格式化**（`free` 从 4112384 → 保持，文件仍在）。
- 卷可用 **4,116,480 B = 4020 KiB**（mtp-core 原预测 4,145,152 B 是 WL 裸容量；`esp_vfs_fat_info` 报的是 FAT 可用容量，差 28,672 B = 7 扇区 = FAT 元数据，属正常）。
- 全程**未碰 SD 卡**（探测期有意简化，避开 LDO4 供电/引脚风险）。

### 2.4 冒烟 B 的 MTP 半边 —— ✅ 成立（Lead 独立复核，非引用他人汇报）

| 检查 | 结果 |
|---|---|
| `Get-PnpDevice -Class WPD` | `Status=OK  svc=WUDFWpdMtp  FriendlyName=MeidaP4 Storage`（修前是"MTP USB 设备"→ 说明 Windows 已读到我们 MTP `DeviceInfo` 里的友好名） |
| `HKLM\SOFTWARE\Microsoft\Windows Portable Devices\Devices\USB#VID_303A&PID_4002&MI_00#8&2B768EE9&0&0000` | **已存在**，`FriendlyName=MeidaP4 Storage`（修前不存在） |
| **资源管理器 "This PC"** | **列出便携设备 `MeidaP4 Storage`** |
| MTP 目录树递归 | `MeidaP4 Storage` → **`PROBE.txt`**（存储路径 `…\{10001,/mtp,4116480}`） |

即 plan 批 1 冒烟 B 的"资源管理器出现 MTP 设备 + 能浏览 storage 卷文件"两条**都已达成**。

### 2.5 冒烟 A / B 的网页侧 —— ✅ `requestDevice` + **`claimInterface` 成功**（Chrome 155，`file://`）

用户实测（`file:///E:/ESP_PROJECT/MeidaP4/web/probe.html`，Chrome 155）：
- `requestDevice` 能看到 `MeidaP4 WebUSB`（VID 0x303A / PID 0x4002）
- **`claimInterface` 成功**（模式 B 复合形态下 claim 的是 vendor 接口 **IF1**，MTP 的 IF0 未动）
- 同一时刻资源管理器里 MTP 设备仍在 → **"受保护类按接口判定"在实现层成立**（plan §0/批 1 冒烟 B 的核心假设 ✅）

成立的两个前提（缺一不可）：
1. MS OS 2.0 描述符集里发 **单数 `DeviceInterfaceGUID` + `REG_SZ`** 的 Registry Property，值为 `{A5DCBF10-6530-11D2-901F-00C04FB951ED}`；
2. 让 Windows 造**全新实例**（递增 `bcdDevice` / 换序列号），否则属性不会被重新应用。

独立复核（Lead 直接查注册表）：当前实例 `USB\VID_303A&PID_4002&MI_01\8&39523820&0&0001`
→ `problem=0`，`DeviceClasses` 下**注册出** `{a5dcbf10-6530-11d2-901f-00c04fb951ed}` 与 `{dee824ef-…}`。

---

## 3. 探测阶段发现的 4 个 plan 级修正（最重要产出）

### 3.1 ⛔ 「WCID 自动绑上 winusb.sys」**不足以**让 Chrome 看到设备

**现象**：Chrome 155 中 `web/probe.html` 点"请求设备" → **chooser 空列表（"找不到兼容设备"）**；`file:///…/probe.html` 与 `http://127.0.0.1:8765/probe.html` **两个地址都一样**；此时设备确实在主机上（`MI_01` 服务 = WINUSB，problem=0）。

**根因**（host-usb 成员实测，Pin 级证据）：
1. `MI_01` **没有注册任何 device interface**：全机 `DeviceClasses` 里搜实例哈希 `2b768ee9` → **0 命中**（父节点有，子节点无）。
2. `MI_01\Device Parameters` 下**没有 `DeviceInterfaceGUIDs`**（只有 VendorRevision / RevisionId / ExtPropDescSemaphore）。
3. `winusb.inf` 的 `[WINUSB.NT.HW]` 是**空占位段**（原文注释 "Keep empty section placeholder"），全文件唯一带 `DeviceInterfaceGUIDs` 的是 ADB 专用段 → **走 WCID 自动绑定的设备不会自动注册 device interface**。
4. 父节点（usbccgp FDO）那条 `{A5DCBF10-6530-11D2-901F-00C04FB951ED}` path 用 `WinUsb_Initialize` 打不开：`err=6 ERROR_INVALID_HANDLE`。

**对 plan 的影响**：plan §5.1 只写了"发 MS OS 2.0 描述符（WCID）→ Windows 自动挂 winusb.sys"。**实测证明这半句成立、但不充分**：还要在 MS OS 2.0 描述符集里补 **Registry Property feature**（`wDescriptorType=0x04`，`wPropertyDataType=0x07` = `REG_MULTI_SZ`，属性名 `DeviceInterfaceGUIDs`），或者干脆让该功能是**非复合**设备（父节点自身为 WinUSB 设备）。→ 已开 task-8 做 A/B 两个实验验证。
**连带影响**：plan §6 的降级预案"Windows WCID 自动绑定不生效 → Zadig 手动装 WinUSB（开发期可接受）"**不解决这个问题**——驱动已经绑上了，缺的是 device interface 注册。

**具体该写哪个 GUID（host-usb 字节级查证，决定性）**：

| 候选 | 含义 | 浏览器/系统侧实测 |
|---|---|---|
| **`{A5DCBF10-6530-11D2-901F-00C04FB951ED}`** | `GUID_DEVINTERFACE_USB_DEVICE`，由 **hub 驱动**硬编码（`USBHUB3.SYS:0x62EE0` / `usbhub.sys:0x67D50`） | **Chrome 155 `chrome.dll` 命中 2 次、Edge 155 `msedge.dll` 命中 2 次** → **必须写这个** |
| `{DEE824EF-729B-4A0E-9C14-B7117D33A817}` | `GUID_DEVINTERFACE_WINUSB`，由 **winusb.sys** 硬编码（`winusb.sys:0x146F0`） | 两个浏览器里 **0 命中**；libusb/sigrok 类应用用，可选 |
| `{F72FE0D4-CBCB-407D-8814-9ED673D0DD6B}` | winusb.inf 里 **ADB 专用段**的值 | 0 命中，**不要用** |

关键机制：该 GUID 只发给**设备节点**，**从不发给 usbccgp 子接口**（全机该 GUID 的 38 条 DeviceClasses 记录里 `&MI_` 命中 = 0）。**两变量对照实验**（本机 present 且 `service==WINUSB` 的设备只有 2 个）证明注册被 `DeviceInterfaceGUIDs` 的**存在性**门控：

| devnode | `Device Parameters\DeviceInterfaceGUID(s)` | 实际注册出的接口 |
|---|---|---|
| 我们 `VID_303A&PID_4002&MI_01` | **不存在** | **0 个** |
| 华硕 `VID_0B05&PID_19AF&MI_00` | 有 `{6987B675-…}` | `{6987B675-…}` + `{DEE824EF-…}` |

→ 所以修复值（`REG_MULTI_SZ`，UTF-16LE，每串 NUL 结尾、再补一个 NUL）：
`{A5DCBF10-6530-11D2-901F-00C04FB951ED}\0{DEE824EF-729B-4A0E-9C14-B7117D33A817}\0\0`

### 3.2 ⛔ `esp_tinyusb 2.3.0` 的 MTP **默认无法工作**（链接期缺陷）

**现象**：`wpdmtp.sys` 已绑定、`problem=0`，但 Windows 事件日志
`Microsoft-Windows-WPDClassInstaller 25089: 安装驱动程序后无法访问设备()。错误代码 0x80070651`，
且 `HKLM\SOFTWARE\Microsoft\Windows Portable Devices\Devices` 下**没有**本设备条目 → Explorer 里不出现便携设备。

**设备侧根因链**（`build-diag-b/serial-diag.log` + `nm`）：

```
USBD Xfer Complete on EP 02 with 16 bytes      ← Windows 发来合法 OpenSession(16 B)
  MTP OpenSession: Command phase
    Stall EP 02                                 ← mtp_device.c:514  phase==MTP_PHASE_ERROR
    Stall EP 82                                 ← mtp_device.c:515
  MTP request: Get Device Status
  Stall EP0                                     ← mtp_device.c:367  TU_VERIFY(len > 0)
```
`nm build-diag-b/MeidaP4.elf`：
```
tud_mtp_command_received_cb          W   ← 弱符号赢了
tud_mtp_request_get_device_status_cb W
tud_mtp_request_device_reset_cb      W
tud_mtp_data_xfer_cb                 W
tinyusb_mtp_install_driver           T
```
机制：上游 `tinyusb/src/class/mtp/mtp_device.c:62/70` 用 `TU_ATTR_WEAK` 提供回调默认实现；esp_tinyusb 的**强实现**在 `mtp/tinyusb_mtp_transport.c`，但**没有任何东西引用该目标文件** → 静态库成员未被拉入 → 弱默认胜出。弱默认 `tud_mtp_command_received_cb()` 返回负值 → `mtp_device.c:425` 置 `MTP_PHASE_ERROR` → 两条 bulk 端点被 stall。

**旁证**：esp_tinyusb 自己的 MTP 测试 app 用了 `WHOLE_ARCHIVE`（`test_apps/mtp_storage/main/CMakeLists.txt:5`），而 2.3.0 的 `CMakeLists.txt:100-109` 只是把 mtp 源码加进库、**没有** whole-archive 或强制引用机制。2.4.0 的 CHANGELOG 也没提 → **大概率同样存在**。

**修复**：`components/usb_probe/CMakeLists.txt` 里 `-Wl,--undefined=mtp_dispatch`（用 `CONFIG_TINYUSB_MTP_ENABLED` 守卫）。修复后 nm 实测四个符号全部变 `T`。

### 3.3 契约细节修正（已回填 probe-contract.md）

| 原契约 | 实测/源码结论 |
|---|---|
| MS OS 2.0 请求 `wIndex=0` | Windows 实测发 **`wIndex=0x0007`**（`C0 02 00 00 07 00 26 00`）→ 必须同时接受 0 和 7 |
| 表里 bulk 一律 "512" | **FS bulk = 64、HS bulk = 512**（USB 2.0 FS bulk 上限 64）；interrupt EP 两种速度都 64 |
| `tud_mount_cb/tud_umount_cb` | esp_tinyusb 2.3.0 已提供**强符号**，重定义会链接失败 → 改用 `tinyusb_config_t.event_cb` |
| 大块回环 | 设备侧缓冲约 12 KB，**主机必须并发发起 OUT/IN 或按 ≤4 KB 分块**，否则死锁；心跳与回环共用同一 IN 流，必须先剥离 `HB <n>` 行 |
| 冒烟 A 第⑤条"页面能读出 BOS/WebUSB 平台能力" | **纯 WebUSB JS 做不到**（不暴露 BOS）。改为"设备出现在 requestDevice 列表 + `chrome://usb-internals` 交叉验证"，BOS 由设备侧串口日志证明 |

**关于 verifier 的 S1（MS OS 2.0 缺 Configuration Subset Header）**：真机证据显示它**不阻断 CompatibleID 的应用**——mode A 用"设备级 Set Header → CompatibleID → RegProperty"（完全没有 0x0001/0x0002）时，`CompatibleIds` 里依然出现 `USB\MS_COMP_WINUSB`、`Service=WINUSB`、`DriverInfPath=winusb.inf`，Registry Property 也被写进 `Device Parameters`。**但**mode A 的 `problem=31` 最终查明是**复数 Registry Property 值**导致的（见 3.1），不是 subset header 的问题。不过契约仍要求 mode A 带上 `Configuration Subset Header + Function Subset Header`——因为那是 MS OS 2.0 的规范形态，而 mode A 从未在"单数值 + 带子集"的组合下单独验证过（见 §5 U8）。

### 3.4 ⛔ bulk 回环：**claim 成功但数据整体位移**（定位中，非浏览器/权限问题）

用户 1 MB 回环结果：期望 1 MiB，**一致仅 4098 B**，失配 1,044,478 B，**首处失配在全局偏移 0**（期望 `0x70` 实际 `0x2C`），过滤心跳 41 行，吞吐 7.962 MB/s，RTT 平均 0.497 ms。

**判读（usb-core 的算术，Lead 采信）**：载荷是 xorshift PRNG（seed `0xC0FFEE`），
`1048576 − 1044478 = 4098`，而 `4098 / 1048576 = 0.39% ≈ 1/256` —— 这正是**"IN 流相对期望整体位移 k 字节"**的指纹（PRNG 下每字节仅 ~1/256 概率碰巧相等）。
加上 7.96 MB/s + RTT 0.497 ms×256 块 ≈ 127 ms，说明**用例是健康跑完的**（没有死锁、没有卡死、期间无复位）。
→ **结论：设备把 ~1 MiB 都回出来了，但 IN 流最前面多了 k 个字节**，导致从偏移 0 起整体错位。
用户看到的"实收 4098 B"是页面的**一致字节数**，不是 IN 原始总字节；`k = rawInBytes − size`。

**根因已定位（主机侧受控对账 + 页面复现）——固件无罪，问题在页面的流对齐**：

用主机侧 WinUSB 工具（`tools/winusb_loopback.py`，绕过浏览器）对账：
```
1 KB : mismatch=0  rawInBytes=1033    k=+9  hbBytes=9   k-hbBytes=0
64 KB: mismatch=0  rawInBytes=65536   k=0   hbBytes=0   k-hbBytes=0
1 MB : mismatch=0  rawInBytes=1048576 k=0   hbBytes=0   k-hbBytes=0
设备侧 stats 增量: out=+1115136B/2178pkt  in=+1115136B/2178pkt（=1024+65536+1048576；2178×512 逐包精确）
接口 IF1 class=0xFF，PipeId 0x03 OUT / 0x83 IN，512 B，协商 HighSpeed；无 panic、无异常 rst
```
→ **固件回环逐字节正确、链路双向逐包精确。**

⚠️ **判据修正**：`k = rawInBytes − size` 包含**合法剥离的心跳字节**，`k>0` 本身不证明错位；必须用 **`k − hbBytes == 0`**（上表三例都满足）。

页面侧的两个缺陷（host-usb 在自己的工具里复现并 hexdump 抓到现场）：
1. **HB 过滤器跨用例状态泄漏** —— 上一轮 reader 在**一行中间**被中断，残留 `_in_line=true` + `_pending="H"`；下一个用例的第一个 `0x0A` 去闭合陈旧残片 → `\nH` 被当数据 → **从全局偏移 0 整体错位**。
   ```
   RAW IN    : 0A 48 42 20 34 36 33 38 33 37 0A EF F6 4E ...
   剥离后实收 : 0A 48 0A 48 42 20 34 36 33 38 33 37 0A EF F6 4E ...
   ```
2. **测量前不 drain/对齐** —— 设备空闲时每秒无条件发 `\nHB <ms>\n` 且**无人读就积在设备 FIFO**（实测一次积压 **374 行 ≈ 4 KB**）。用户的 `k ≈ 4098` 与之数量级完全吻合。→ 这同时是**协议脆弱性**：固件侧已修（空闲→数据转换时清 TX FIFO，让任何主机实现都不必先猜行边界）。

3. **ZLP（零长度包）被当成致命错误** —— 用户第二次真机复测（页面已修前两项）暴露的新缺陷：
   ```
   测量前排空：读 1 次 / 9 B（完整心跳行 1 行），对齐=是
   第 1 块失败 @offset 4096: ProbeEmptyReadError: transferIn 返回 0 字节
   结束：中止；一致 65536/65536 B；失配 0 B；rawInBytes=4318；hbBytes=222；k=-61218
   ```
   用户观察：**"1 KB 可以，超过 4 KB 就不行"**。判读：第 0 块**正好 4096 B**（512 的整数倍）
   → TinyUSB 的 vendor IN 在"传输长度恰为 max-packet 整数倍且流已空"时会**补一个 ZLP**，
   该 ZLP 在 WebUSB 侧作为一次**独立的 0 字节 `transferIn`** 返回，页面把它当致命错误中止了。
   1 KB 之所以"可以"：请求读长 4096 > 实际可用 1024 → 收到**短包**即完成，不需要 ZLP。
   **设备侧无罪**：host-usb 的 WinUSB 工具在同一块板/同一版固件上 1 KB/64 KB/1 MB 全 `mismatch=0`、约 51 Mbit/s。
   → 两条结论：(a) 页面必须把 0 字节读当合法事件（计入 `zeroLenReads` 并继续读，靠用例总超时兜底）；
   (b) **正式 I0 线协议必须把 ZLP 语义定死**，否则每个主机实现都会踩同一个坑。

**⚠️ 一个测量陷阱（已修工具）**：`tools/serial_capture.py` 打开串口时的 DTR/RTS 抬动**必定复位芯片**，
所以"先抓基线 → 跑测试 → 再抓一次"的**增量法在本机不成立**（两次读各自都把计数器清零）。
Lead 已修该工具：`--no-reset` 更可靠地保持 DTR/RTS 低，且**检测到启动横幅就打印 WARNING** 说明计数器已被复位。
正确做法是**让一次抓取把测试夹在中间**（后台抓 45 s → 等 8 s 打出基线 stats → 在抓取期间跑回环 → 逐条做差）。

---

## 4. 冒烟 C · 端点预算（离线，独立复算）

- P4 端点上限（`tinyusb/src/portable/synopsys/dwc2/dwc2_esp32.h:80`）：`ep_count=16, ep_in_count=8`（IN 含 EP0）→ **用户可用 IN ≤ 7**。
- 模式 A：1 IN + EP0 = **2/8**；模式 B：3 IN + EP0 = **4/8**。
- 模式 B **再加 NCM**（EP4 int IN + EP5 bulk IN/OUT）：3 + 2 = 5 IN → **+EP0 = 6/8，余 2**；总端点 9/16。**plan 的算术成立，且比 plan 表的"6/8"更宽松。**
- ⚠️ **plan 表漏写的限定**：`dwc2_esp32.h:79-80` 显示 **FS 口只有 `ep_in_count=5`（用户 IN ≤ 4）** → 「模式 B + NCM」在 FS 口**放不下**，只在 HS 口成立。实测固件确实用 HS 口（`USBD init ... speed = High`）。

---

### 2.6 回环数据通路 —— ✅ 通过（两条独立证据）

**主机侧（WinUSB 直连，绕过浏览器）**：`tools/winusb_loopback.py` 1 KB / 64 KB / 1 MB **失配全 0**，
`k − hbBytes == 0`；1 MB 约 **51.5 Mbit/s**（RTT 平均 0.627 ms）。
设备侧串口对账：`out == in == 1,115,136 B / 2178 pkt`（= `1024+65536+1048576`，`2178×512` **逐包精确**），
全场无 `Guru Meditation` / `Backtrace` / 异常复位。

**浏览器侧（Chrome 155，`file://`）**：用户实测报告 **1 KB / 64 KB / 1 MB 各两次全部通过**
（`失配 0`、`k − hbBytes 0`、`无法解释 0`）。
> ⚠️ **证据强度声明**：这一条是**用户口头确认**，原始"导出报告"未留档（板子已拔、无法再跑）。
> 主机的机械证据（上一条）是完整留档的；浏览器侧的原始文本没有归档。

过程中在浏览器链路里修掉的 **3 个主机侧缺陷**（固件侧从头到尾是逐字节正确的）：
| # | 缺陷 | 机制 |
|---|---|---|
| 1 | HB 过滤器**跨用例状态泄漏** | 上一轮 reader 在心跳行中间被中断，残留"行内"状态 + 半截字节；下一用例的第一个 `0x0A` 去闭合陈旧残片 → `\nH` 被当数据 → **偏移 0 整体错位** |
| 2 | 测量前**未 drain/对齐** | 设备空闲时每秒无条件发心跳且无人读就积在 FIFO（实测一次积压 **374 行 ≈ 4 KB**）→ 用户那次 `k ≈ 4098` 与之吻合 |
| 3 | **ZLP 被当成致命错误** | 块长恰为 512 整数倍时 TinyUSB 补 ZLP，WebUSB 表现为独立的 **0 字节 `transferIn`** → 页面中止（用户观察："1 KB 可以，超过 4 KB 就不行"） |

---

## 5. 未验证项（**本阶段真机已不可用 → 永久未在此阶段验证**，不是"待验证"）

| # | 项 | 说明 / 建议 |
|---|---|---|
| U6 | 供电与热稳定（总线 500 mA 是否够 Wi-Fi 峰值） | 探测期未涉及；完整版**必测**（plan §6 风险 4） |
| U7 | NCM / NAPT 真机 | 完整版阶段；离线端点预算已算出「模式 B + NCM = 5 IN +EP0 = 6/8，仅 HS 口可行」 |
| **U8** | **"单数 Registry Property + 带 subset header"** 的组合 | ⚠️ **这是模式 B 的冻结形态**（`SET → FUNC_SUBSET(IF1) → COMPAT → REGPROP` + 单数 REG_SZ）；真机验证过的却是「单数 + **无** subset」（模式 A）。**板子已拔，无法关闭。建议完整版上电后列为第一优先复测** |
| U9 | 模式 A 是否也能被 Chrome 看到（非复合形态的对照实验） | 未做；模式 A 冻结为 `bDeviceClass=0xFF/00/00` + 设备级 MS OS 集 |
| U10 | `zeroLenReads` 的真机频率是否符合 ZLP 推论（64 KB ≈ 16、1 MB ≈ 256） | 只是 mock 推论，用户报告未附该字段数值 |
| U11 | `echo_errors` 真机读数 | 固件 `stats` 行未含该字段，仅溢出时打警告；完整版建议加进 stats |

---

## 6. 结论与交付

**探测阶段达到目的**：plan 的 3 个高风险假设全部被真机判定，并额外发现 4 处 plan 级缺陷 ——
其中 2 处（**WCID 不足以让 Chrome 看到设备**、**esp_tinyusb MTP 强符号未链接**）如果留到写业务代码之后才暴露，
返工面会很大。

**保留下来的东西**（按用户要求：只留有用的经验和有效代码）：
- **有效代码**：`components/usb_probe/`、`components/mtp_probe/`、`main/`、`web/probe.html`；
  两个模式 `-Slot final -Fresh` 干净构建 **exit 0**。
- **可复用工具**：`tools/{idf-env,build,flash}.ps1`、`tools/serial_capture.py`、`tools/winusb_loopback.py`、`tools/desc_lint.py`、`tools/verify-offline.ps1`。
- **经验**：`docs/probe-report.md`（本文）、`docs/probe-contract.md`、`docs/probe-plan.md`、`docs/probe-verify.md`、`docs/host-verify.md`、`docs/host-usb-test.md`。
- **归档**：全部构建产物与槽位配置 → `archive/probe-batch0/`（3.5 GB，可安全删除，见该目录 README）。
  参考用组件源码树（esp_tinyusb 2.3.0 / tinyusb 0.21.0~2）也在那里，供**离线**查 API。

**终局独立判定**（verifier，task-11，冻结版源码 SHA256 已固化）：
```
tools/verify-offline.ps1
Mode Total Passed Failed Warn Exit Result
a      120    120      0    1    0 PASS
b      137    137      0    1    0 PASS
总判定: PASS   exit 0
```
> 边界：这**只**说明"冻结代码写出的 Registry Property 形状与真机验证过能成功的那个形状逐字节相同"，
> **不能**推出端到端功能一定成立。**离线 PASS ≠ 真机通过。**

**开工完整版时的第一件事**：先关掉 §5 的 **U8**（模式 B 冻结形态的真机复测），再按 plan 批 1 往下走。
