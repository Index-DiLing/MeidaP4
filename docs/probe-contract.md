# 探测阶段·冻结契约（I0-lite / I1-lite）

> 并行开发的前提。**本文件里的数字与函数签名在探测期内不得单方面修改**；要改先报到 Lead，Lead 改这里并通知所有写入者。
> 生产者：Lead。消费者：`usb_probe`(usb-core) / `mtp_probe`(mtp-core) / `web/probe.html`(web-host) / `tools/desc_lint.py`(verifier)。

---

## 1. 端点与接口分配（探测期冻结）

### 模式 A · 单 Vendor（`mode a`，MTP 关）

| 接口 | 号 | 功能 | IN EP | OUT EP |
|---|---|---|---|---|
| Vendor | 0 | WebUSB 回环 | **EP1** bulk 512 | **EP1** bulk 512 |
| **合计** | | | **1 IN** + EP0 = 2/8 | 1 OUT |

### 模式 B · MTP + Vendor（`mode b`，MTP 开）

| 接口 | 号 | 功能 | IN EP | OUT EP |
|---|---|---|---|---|
| MTP | 0 | 文件级共享（`storage` FATFS 卷） | **EP1** interrupt 64（事件）+ **EP2** bulk 512 | **EP2** bulk 512 |
| Vendor | 1 | WebUSB 回环 | **EP3** bulk 512 | **EP3** bulk 512 |
| **合计** | 2 | | **3 IN** + EP0 = 4/8 | 2 OUT |

### 推演：加 NCM 后（冒烟 C 只离线校验，不上电）
+ NCM = **EP4** interrupt IN 16 + **EP5** bulk IN 512 + **EP5** bulk OUT 512
→ **5 IN + EP0 = 6/8**，OUT 3 个。**余 2 个 IN 端点**（plan 表里留 1 个，实际更宽松）。

> EP 号**分方向独立编号**（USB 规矩），所以 IN/OUT 可以同号。EP 号从 1 开始，EP0 归控制端点。
>
> ⚠️ **bulk 的 wMaxPacketSize 分速度**：**FS = 64、HS = 512**。USB 2.0 全速 bulk 的合法上限就是 64；
> 表里写的 "512" 是 HS 值。FS 描述符里写 512 是违规的（DWC2 不会校验，但主机可能直接拒绝枚举）。
> 上表的 interrupt EP（MTP 事件）两种速度都是 64。

---

## 2. 描述符身份（探测期冻结）

| 项 | 值 | 依据 |
|---|---|---|
| `idVendor` | `0x303A`（Espressif） | 与 hello_p4/TinyUSB 默认一致，避免 Windows 驱动库误匹配 |
| `idProduct` | `0x4001`（模式 A）/ `0x4002`（模式 B） | 两个模式用不同 PID，便于主机侧区分 |
| `bcdUSB` | **`0x0210`** | 必须 ≥ 0x0210，否则 Windows 不会取 BOS/MS OS 描述符 |
| `bDeviceClass/SubClass/Protocol` | 模式 B：**`0xEF / 0x02 / 0x01`**（Misc / IAD）<br>模式 A：**`0xFF / 0x00 / 0x00`** | 模式 B 要让 Windows 挂 `usbccgp` 再按接口分发（wpdmtp / WinUSB）；模式 A 是**非复合**设备，意图是让 `winusb.sys` 直接绑**整个设备节点**（父节点自身即 WinUSB 设备，`{A5DCBF10-…}` 可直接打开）——见 §2.1 |
| `bcdDevice` | **每次改动描述符内容就递增**（当前实测 `0x0102`） | Windows 只在**安装时**应用 MS OS 2.0 注册表属性；不改硬件 ID/实例就拿不到新值 |
| 产品串 | `MeidaP4 Probe` | |
| 序列号串 | **每次改动描述符内容就换**（当前实测 `PROBE-D3`） | 同上：靠新序列号强制 Windows 造新实例。模式 A/B 的 PID 不同，本身已是不同实例；完整版改用 eFuse MAC |

### BOS（自定义，模式 A/B 都有）
1. BOS 头：`bNumDeviceCaps = 2`，`wTotalLength` = 三个描述符实际长度和（实测 **57 B**）
2. **WebUSB 平台能力**：UUID `{3408b638-09a9-47a0-8bfd-a0768815b665}`，`bcdVersion=0x0100`，`bVendorCode=0x01`，**`iLandingPage=0`**
   → 没有它 Chrome **不会**把设备暴露给 `navigator.usb`。
   → `iLandingPage` **必须是 0**：我们不做 WebUSB 着陆页。声明非 0 等于诱导 Chrome 发 `GET_URL`，而我们对它 stall；真机日志已确认改成 0 后 Chrome **完全不发** `bRequest=0x01`。
3. **MS OS 2.0 平台能力**：UUID `{d8dd60df-4589-4cc7-9cd2-659d9e648a9f}`，`dwWindowsVersion=0x06030000`，`bVendorCode=0x02`，且 `wMSOSDescriptorSetTotalLength` **必须等于描述符集数组的真实长度**
   → 没有它 Windows **不会**自动给 vendor 接口挂 `winusb.sys`。

> ⚠️ 两个平台能力用**不同的 `bVendorCode`**（WebUSB=0x01 / MS OS=0x02）。
> **`bVendorCode` 就是对应请求的 `bRequest`**：WebUSB 的 `GET_URL` 用 `bRequest = 0x01`（WebUSB 的 bVendorCode），
> MS OS 的 `GET_MS_DESCRIPTOR` 用 `bRequest = 0x02`（MS OS 的 bVendorCode）。声明与分发必须严格对应。

### MS OS 2.0 描述符集（响应 `bmRequestType=0xC0, bRequest=0x02, wIndex=0 或 0x07`）

> ⚠️ **`wIndex = 0x0007` 是"设备级"请求**（真机实测 Windows 第一次就是 `C0 02 00 00 07 00 …`），
> 按 MS OS 2.0 规范它期望 `Set Header` → **`Configuration Subset Header (0x0001)`** → `Function Subset Header(s) (0x0002)` → 特性描述符。
> 真机上"缺子集头"的表现是：`CompatibleID` 仍可能被解析（驱动绑上了），但设备 `problem=31 (FAILED_LOAD)` 且设备侧 `mounted=0`（**`SET_CONFIGURATION` 没成功**）。

**必须包含 Registry Property** —— 这是让设备能被用户态/Chrome 打开的关键（见 §2.1 实测）：

| 模式 | 结构（**真机实测形态**） |
|---|---|
| **A（非复合，单接口）** | `Set Header (0x0000)` → `CompatibleID (0x0003)` → **`Registry Property (0x0004)`**（**不含** subset header，实测 158 B） |
| **B（复合）** | `Set Header (0x0000)` → `Function Subset Header (0x0002, bFirstInterfaceNumber=vendor itf)` → `CompatibleID (0x0003)` → **`Registry Property (0x0004)`**（实测 166 B） |

> ⚠️ **关于 subset header（含 Configuration Subset 0x0001）：真机证明它不是取得兼容 ID / 注册表属性的必要条件**。
> 模式 A 用"设备级 `Set Header → CompatibleID → RegProperty`"（完全没有 0x0001/0x0002）时，
> 主机侧 `CompatibleIds` 依然出现 `USB\MS_COMP_WINUSB`、`Service=WINUSB`、`DriverInfPath=winusb.inf`，
> Registry Property 也被正确写进 `Device Parameters`。
> 模式 A 早期那次 `problem=31` 的**真因是复数 Registry Property 值**（见下），不是缺 subset header。
> 所以 **subset header 由"硬性要求"降级为"规范推荐"**：完整版（MTP + Vendor + NCM 多功能）
> 建议补上 `Configuration Subset Header + Function Subset Header`，那也是 MS OS 2.0 的规范形态；
> 但探测期**未在"单数 RegProp + 带 subset"的组合下单独验证过**（见 `docs/probe-verify.md` §5 的 U8 —— 注意这是**验证报告**的 §5，不是本文件的 §5）。

- `CompatibleID = "WINUSB\0\0"`（8 B），`SubCompatibleID = 8×0x00`。
- **`Registry Property`**（⚠️ 必须用 **单数 `DeviceInterfaceGUID` + `REG_SZ`** 形式；真机实测复数 `DeviceInterfaceGUIDs` + `REG_MULTI_SZ` 会让 `winusb.sys` 起不来，见下方）：
  ```
  wLength(2)            = 10 + wPropertyNameLength + wPropertyDataLength
                          （= wLength(2)+wDescriptorType(2)+wPropertyDataType(2)+wPropertyNameLength(2)
                             +wPropertyDataLength(2) + name + data；⚠️ 早期契约写成 "4+name+data+2"，少算 4 字节）
  wDescriptorType(2)    = 0x0004   (MS_OS_20_FEATURE_REG_PROPERTY)
  wPropertyDataType(2)  = 0x0001   (REG_SZ)
  wPropertyNameLength(2)= (len("DeviceInterfaceGUID")+1)*2 = 40
  PropertyName          = "DeviceInterfaceGUID" UTF-16LE + NUL   (40 B)
  wPropertyDataLength(2)= 78
  PropertyData          = "{A5DCBF10-6530-11D2-901F-00C04FB951ED}" UTF-16LE + NUL  (78 B)
  ```
  自洽核算：`wLength = 10 + 40 + 78 = 128`（实测实现正是 128）。
- `{A5DCBF10-6530-11D2-901F-00C04FB951ED}` 是**必须**的值：Chrome/Edge **唯一**认它
  （host-usb 对 `chrome.dll`/`msedge.dll` 做 16 字节 GUID 内存布局字节扫描：该 GUID 各命中 2 次，
  `{DEE824EF-…}` 与 `{F72FE0D4-…}` **零命中**）。真机验证：写入该值后 `MI_01` 从 `problem=31` 变为
  `problem=0`，且 Chrome 155 里 `requestDevice` 能看到设备、**`claimInterface` 成功**。
- ⚠️ **不要用复数形式**：`DeviceInterfaceGUIDs` + `REG_MULTI_SZ (0x0007)` 会把值正确写进
  `Device Parameters`，但 `winusb.sys` 随后启动失败（Device Manager **Code 31** /
  `DEVPKEY_Device_ProblemStatus = 0xC0000182`），且设备再也拿不到 `SET_CONFIGURATION`
  （非复合时表现为 `mounted=0`）。真机在**全新实例**上复现过（含/不含 `A5DCBF10` 都复现），
  并已排除 `REV_0100` 硬件 ID 撞车这个混淆变量。切换到单数形式后立即 `problem=0`。
- 只给 **vendor** 功能发 CompatibleID + Registry Property；MTP 那一路由 Windows 自带的 `wpdmtp.sys` 按接口类绑定，**不要**给它发。
- ⚠️ **Windows 只在"安装时"应用 MS OS 2.0 注册表属性**：同一实例/同一硬件 ID 上改描述符**不会**刷新，
  会得到"描述符对了但主机状态是旧的"的**假阴性**。因此**每次改动描述符内容都必须换序列号或递增 `bcdDevice`** 以强制新实例（我们没有管理员权限做 `pnputil /remove-device`）。探测期实测踩过两次。

> ⚠️ **`wIndex` 必须同时接受 `0` 和 `0x07`**：Microsoft 文档、Zephyr `msos_desc.h`
> （`MS_OS_20_DESCRIPTOR_INDEX = 0x07`）、TinyUSB 官方 `webusb_serial`/`dfu` 示例、Chrome 的
> build-for-webusb 文档，用的都是 **`wIndex = 7`**；`wIndex` 只用于过滤，不影响返回内容。
> 只认 0 会在真机上被 Windows 直接 stall 掉 —— 那是 R1 的关键路径。
>
> ⚠️ `tinyusb_mtp_install_driver()` **不注册任何 USB 描述符**（只建 mutex/表/字符串，
> 见 `managed_components/espressif__esp_tinyusb/mtp/tinyusb_mtp.c`）。模式 B 的 MTP 接口段
> **必须**由 `usb_probe` 自带配置描述符提供，否则装了 MTP 类也枚举不出来。

### 字符串描述符索引（冻结）
| idx | 内容 |
|---|---|
| 0 | LANGID `0x0409` |
| 1 | Manufacturer `MeidaP4` |
| 2 | Product `MeidaP4 Probe` |
| 3 | Serial（**每次改动描述符内容就换**，当前实测 `PROBE-D3`；见 §2 身份表） |
| 4 | Vendor 接口 (`MeidaP4 WebUSB`) —— 仅当它 ≤ 7 时可用 |
| 5 | MTP 接口 (`MeidaP4 Storage`) —— 模式 B |

> `esp_tinyusb` 上限 8 条（`USB_STRING_DESCRIPTOR_ARRAY_SIZE`），**不要**试图用 0xEE。

---

## 3. 组件接口（探测期冻结）

### 3.1 `components/usb_probe` — 生产者 `usb-core`

```c
/* include/usb_probe.h */
typedef struct {
    uint32_t out_bytes;    /* 主机→设备 累计字节 */
    uint32_t in_bytes;     /* 设备→主机 累计字节 */
    uint32_t out_packets;
    uint32_t in_packets;
    uint32_t echo_errors;  /* 回环不一致次数 */
    uint32_t ctrl_requests;/* 收到的供应商控制请求数 */
    uint32_t ctrl_stalls;  /* 未实现而 stall 的供应商请求数（真机调试用） */
    bool     mounted;      /* 主机是否已枚举完成（来自 tinyusb_config_t.event_cb） */
} usb_probe_stats_t;

esp_err_t usb_probe_start(void);                 /* 组装描述符 + 安装 tinyusb + 起回环任务 */
void      usb_probe_get_stats(usb_probe_stats_t *out);
uint32_t  usb_probe_config_desc_len(bool high_speed);  /* 供离线校验/自检使用 */
```

**要求**
- 自带 FS **和** HS 配置描述符（模式 A 必须；模式 B 也自带，不用 `esp_tinyusb` 的默认数组，便于冻结端点表）。
- 自带 device 描述符 + 字符串数组 + `tud_descriptor_bos_cb()` + `tud_vendor_control_xfer_cb()`。
- 回环：`tud_vendor_rx_cb()` 收到什么就 `tud_vendor_write()` 回去；同时累加统计。
- 必需打挂载/卸载日志便于主机侧确认枚举：用 `tinyusb_config_t.event_cb` 处理
  `TINYUSB_EVENT_ATTACHED` / `TINYUSB_EVENT_DETACHED`。
  ⚠️ **不要**自己定义 `tud_mount_cb()` / `tud_umount_cb()`：esp_tinyusb 2.3.0
  （`tinyusb.c:62/86`）已经提供**强符号**，重定义会 multiple definition 链接失败。
- 安装用 `TINYUSB_PORT_HIGH_SPEED_0`（P4 是双口芯片，不设会走 FS 口）。
- 组件 `Kconfig` 里提供 `CONFIG_USB_PROBE_MTP_ITF_ENABLE` 之类开关，但**接口号/EP 号必须与本文件一致**。

### 3.2 `components/mtp_probe` — 生产者 `mtp-core`

```c
/* include/mtp_probe.h */
esp_err_t mtp_probe_start(void);   /* 挂载 storage 分区(FATFS) + tinyusb_mtp_install_driver + register_storage */
bool      mtp_probe_is_mounted(void);
const char *mtp_probe_mount_point(void);  /* 返回 CONFIG_PROBE_MTP_MOUNT_POINT */
```

**要求**
- 用 `esp_vfs_fat_spiflash_mount_rw_wl()`（或 IDF 6.x 等价 API）挂载 `storage` 分区到 `CONFIG_PROBE_MTP_MOUNT_POINT`。
- 首次无文件系统时按 `CONFIG_PROBE_MTP_FORMAT_IF_NEEDED` 格式化。
- 调 `tinyusb_mtp_install_driver(&cfg)` + `tinyusb_mtp_register_storage(&s_cfg, &hdl)`。
- **必须在 `usb_probe_start()` 之前调用**（MTP 驱动要先 install，tinyusb 后装）。`main.cpp` 已按此顺序接线。
- 探测期只用**片内 `storage` 分区**，不碰 SD 卡（SD 的 LDO4/引脚风险留到完整版）。
- 挂载成功后往卷里写一个 `PROBE.txt`（内容含时间戳与固件版本），让 MTP 有文件可看。

### 3.3 `main/main.cpp` — 生产者 Lead（别人不要改）

顺序：`mtp_probe_start()` → `usb_probe_start()` → 每 5 s 打印 `usb_probe_get_stats()`。

---

## 4. 线协议 v0（探测期，故意极简）

真实 I0（magic/通道/长度/序号/流控/重传）留到完整版。探测期只要证明"bulk 双向通路成立"：

- OUT（主机→设备）：任意字节流，设备**原样回环**到 IN。
- IN（设备→主机）：回环数据 + **心跳行**：设备在空闲 1 s 后主动发 `\nHB <uptime_ms>\n`。
- 主机测试页发 N 字节、收 N 字节并逐字节比对；心跳行单独过滤显示。
- ⚠️ **大块回环必须并发发起 OUT 与 IN（或按 ≤4 KB 分块）**：设备侧缓冲有限
  （vendor RX/TX FIFO 各 4 KB + 8 KB stream buffer ≈ 12 KB）。若页面先
  `await transferOut(N)` 再 `transferIn`，N ≳ 12 KB 时设备 FIFO 满 → NAK →
  `transferOut` 永不 resolve → **死锁**。这是 USB 流控的必然结果，不是固件缺陷。
- ⚠️ 心跳与回环数据**共用同一个 IN 流**，页面必须先剥离 `\nHB <n>\n` 行再逐字节比对。
- 无帧头、无 ACK、无重传。**不要**在这里设计正式协议。

---

## 5. 构建命令（每个写入者用自己的 build/sdkconfig 槽位，避免互相踩）

```powershell
# 通用形式
pwsh -File tools/build.ps1 -Mode <a|b> -Slot <你的名字>
# 等价于（Slot=usbcore 时）：
idf.py -B build-usbcore-a -D SDKCONFIG=sdkconfig.usbcore-a `
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.a.defaults" build
```

| 写入者 | Slot | 模式 |
|---|---|---|
| usb-core | `usbcore` | a 和 b **都要** |
| mtp-core | `mtp` | b |
| Lead | `main` | a 和 b |

**并发纪律**
- 只用上面的 `-B` / `-D SDKCONFIG`，**绝不要**用裸 `idf.py build`（会写共享的 `sdkconfig` 和 `build/`）。
- `managed_components/` 与 `dependencies.lock` 是共享的：Lead 会先跑一次完成一次安装。若 configure 报组件管理器锁错误，**等 20 s 重试一次**即可。
- 不要 `idf.py fullclean`；要清就删自己的 `build-<slot>-<mode>` 和 `sdkconfig.<slot>-<mode>`。

---

## 6. 主机侧验收（web-host + 用户）

`web/probe.html` 必须是一个**单文件、无构建、无外部依赖**的页面，功能：
1. 检查 `navigator.usb` 是否存在，并显示 `isSecureContext`（用于验证 `file://` 是安全上下文）。
2. `requestDevice`（按 VID `0x303A` 过滤）→ 显示设备/配置/接口/端点清单。
3. `claimInterface` → 报告成功/失败原因（把 `DOMException.name` 打出来）。
4. 回环测试：1 KB / 64 KB / 1 MB（可调）逐字节校验 + 吞吐量与 RTT 统计。
5. `transferIn` 读心跳，显示设备运行时间。
6. MTP 模式（mode b）下：明确提示"资源管理器应同时出现 MTP 设备"，并**不**尝试 claim MTP 接口。

`web/probe.html` 只依赖标准 WebUSB API（`navigator.usb`），不得依赖 `navigator.usb.requestDevice` 之外的任何实验特性。
