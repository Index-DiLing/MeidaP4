# 完整版 · 冻结的接口 / 端点 / 身份分配（I1 数据基础）

> 上位：[usb-composite-plan.md](../usb-composite-plan.md) §1/§2 · 前序：[probe-contract.md](../probe-contract.md)
> 生产者：Lead。**要改先报 Lead**；Lead 改本文件并同步所有写入者。
> 实现载体：`components/usb_composite/`（I1 装配器）。**任何工作流都不得直接构造或修改配置描述符**，
> 只通过 `usb_composite_register()` 注册自己的片段与回调。

---

## 0. 与前序契约的关系

探测期（批 0）冻结的 `docs/probe-contract.md` 覆盖**模式 A / 模式 B 两个探测形态**，且**已经真机验证通过**。
完整版是**第三个形态**（4 功能复合），因此：

- 探测期的模式 A/B 固件与槽位**继续保留**（`components/usb_probe/`、`components/mtp_probe/`），可随时作为对照组烧写。
- 完整版用**新的 PID（0x4103，见 §3）**，让 Windows 建全新设备实例 —— 这是让 MS OS 2.0 注册表属性被重新应用的**必要条件**
  （probe-contract §2 已实测：同一硬件 ID 上改描述符不会刷新）。
- 完整版**不复用** `usb_probe` 的描述符代码；`usb_composite` 是独立实现（探测期代码保持冻结，不再演进）。

---

## 1. 接口分配（完整版冻结）

| IF | 功能 | 类/子类/协议 | 归属工作流 | 说明 |
|---|---|---|---|---|
| **0** | MTP | `06/01/01` (Still Imaging / PTP) | WS-storage | Windows 用自带 `wpdmtp.sys`，**不发** WCID |
| **1** | Vendor · 视频 | `FF/00/00` | WS-webusb + WS-video | **只有 IN 端点**（视频独占，避免队头阻塞） |
| **2** | Vendor · 控制 | `FF/00/00` | WS-webusb | CTRL / FILE / LOG / TELEM 复用这一对端点 |
| **3** | NCM 控制 | `02/0D/00` (CDC / NCM) | WS-net | 与 IF4 组成 IAD |
| **4** | NCM 数据 | **`0A/00/01`** (CDC-Data / **protocol = NTB**) | WS-net | ⚠️ 见下方更正 |
| **合计** | **5 个接口** | | | |

> ⚠️ **相对 plan §0 表格的两处细化**（不改结论，只改接口粒度）：
> 1. plan 把 Vendor 写成"1 个接口"；实际拆成 **IF1(视频) + IF2(控制)**。原因是 `esp_tinyusb`/upstream TinyUSB 的
>    vendor 驱动（`vendor_device.c:284 vendord_open()`）**每个 vendor 接口只认 1 个 IN + 1 个 OUT**，
>    同一接口上放两个 IN 只会保留最后一个。所以"视频独占一个 IN"必须靠**两个 vendor 接口**实现
>    （`CONFIG_TINYUSB_VENDOR_COUNT=2`，Kconfig 上限恰为 2，实测确认）。
> 2. NCM 按规范必须 `IAD + 控制接口 + 数据接口` 两个接口，plan 的"3 类接口"是功能维度，不是接口号维度。

---

### 1.1 ⚠️ 备用设置（Alternate Setting）—— NCM 数据接口**必须**有两套

CDC-NCM 规范要求数据接口提供两个 alternate setting，**Windows 收件箱 `UsbNcm.sys` 就是这么要求的**
（WS-net 2026-10 提出，Lead 已修装配器与校验器）：

| 接口 | `bAlternateSetting` | `bNumEndpoints` | 端点 |
|---|---|---|---|
| IF4（NCM 数据） | **0** | **0** | 无（默认设置不占带宽） |
| IF4（NCM 数据） | **1** | **2** | EP6 bulk IN + EP6 bulk OUT |

**对装配器与校验器的影响**（已实现，见 `usb_composite_desc.c`）：
- `Interface Descriptor` 只有在 **`bAlternateSetting == 0`** 时才算"一个新接口"，才递增接口号计数；
- `bAlternateSetting != 0` 必须**复用前一个接口号**，且**不计入** `bNumInterfaces`；
- 冻结的 `bNumInterfaces = 5`（§1 表）**不变** —— alt=1 不是第 6 个接口。

⚠️ 修之前的行为是"每个 Interface Descriptor 都当成新接口"，后果是 **NCM 的片段会在
`usb_composite_prepare()` 运行期被拒 → 整个复合设备（连 MTP / vendor 一起）起不来**。
设备未连接时这种序列化错误完全看不出来，是必踩的坑。

---

## 2. 端点分配（完整版冻结）

| EP | 方向 | 类型 | wMaxPacketSize (HS / FS) | 服务 | 轮询 |
|---|---|---|---|---|---|
| **EP1** | IN | interrupt | 64 / 64 | MTP 事件 | 1 ms |
| **EP2** | IN | bulk | 512 / 64 | MTP 数据 | — |
| **EP2** | OUT | bulk | 512 / 64 | MTP 数据 | — |
| **EP3** | IN | bulk | 512 / 64 | **Vendor 视频**（独占） | — |
| **EP4** | IN | bulk | 512 / 64 | Vendor 控制/文件/日志/遥测 | — |
| **EP4** | OUT | bulk | 512 / 64 | Vendor 控制/文件 | — |
| **EP5** | IN | interrupt | 16 / 16 | NCM 通知 | 8 ms |
| **EP6** | IN | bulk | 512 / 64 | NCM 数据 | — |
| **EP6** | OUT | bulk | 512 / 64 | NCM 数据 | — |

> ⚠️ **2026-10 更正（verifier 独立复算提出，实现是对的、本表是错的）**：
> §2.2 原先写 **852 word / 余 172**，**漏掉了 Buffer DMA 的 EPInfo 开销**。
> 正确的复算链（`dcd_dwc2.c` 源码级）：
> ```
> dcd_dwc2.c:291-294  is_dma ⇒ dfifo_top = 1024 − 2*ep_count = 1024 − 32 = 992
>                     ★ 这 32 word = EPInfo / DMA 元数据（Buffer DMA 每端点方向 1 word × 2 方向 × 16 端点）
> dcd_dwc2.c:232      GRXFSIZ = 13+1+2*(512/4+1)+2*16 = 304        （这一行原本是对的）
> dcd_dwc2.c:244      TX FIFO = ceil(mps/4)：EP0 16 + EP1 16 + EP2/3/4/6 各 128 + EP5 4 = 548
> 合计 = 32 + 304 + 548 = **884** ≤ 1024；余 = **140**
> ```
> 结论方向不变（884 可行；开 double buffering 后 884+4×128 = 1396 **仍然不可开**），
> 但余量必须按 **140** 算，否则将来加端点会拿错余量。
> ⚠️ **重要局限（verifier 指出）**：`uc_selfcheck()` 用的是同一个 884，
> **所以自检抓不到"冻结表数字本身错"这类问题 —— 只有独立复算能。** 这是本验证链的已知盲区。

- **IN 端点用量：EP1~EP6 = 6 个**，+ EP0 = **7 / 8 上限** → 余 1（与 plan §1 一致）。
- **OUT 端点：3 个**（EP2 / EP4 / EP6），各自独立编号空间。
- **全程只用 bulk / interrupt，不用等时** —— 绕开 8.192 MB/s 等时天花板（plan §1）。
- ⚠️ **本表只在 HS 口成立**。FS 口 `ep_in_count=5`（用户 IN ≤ 4），放不下（probe-report §4 实测）。
  固件必须设 `tinyusb_config_t.port = TINYUSB_PORT_HIGH_SPEED_0`。

### 2.1 端点号分配规则（I1 装配器实现）

- IN 与 OUT **各自从 EP1 起递增分配**，顺序 = 注册顺序。
- OUT 是否复用某个 IN 的号码，由功能在注册时**显式**给出（`ep_out_same_num_as_in[j] = i`）；
  `= USB_EP_NO_PAIR (0xFF)` 表示从 OUT 方向独立分配一个新号。
  **没有隐式规则**——因为 MTP 与 NCM 都是「2 IN 但只有 1 OUT，且那个 OUT 必须配到第 2 个 IN」，
  任何"第 i 个配第 i 个"的规则都会把它们配错（见 `usb_composite.h` 里该字段的注释）。
- 分配结果写入 `usb_fn_alloc_t` 后交给功能回调；功能**不允许**自己改号。

按注册顺序 `MTP → video → ctrl → NCM` 实算，正好得到 §2 的表：

| 功能 | in_count | out_count | 配对表 | IN 分配 | OUT 分配 |
|---|---|---|---|---|---|
| MTP | 2（int64, bulk512） | 1（bulk512） | `[1]` | EP1, EP2 | 复用 IN[1] → **EP2** |
| video | 1（bulk512） | 0 | — | EP3 | — |
| ctrl | 1（bulk512） | 1（bulk512） | `[0]` | EP4 | 复用 IN[0] → **EP4** |
| NCM | 2（int16, bulk512） | 1（bulk512） | `[1]` | EP5, EP6 | 复用 IN[1] → **EP6** |

### 2.2 ⚠️ DWC2 FIFO 预算（本方案真正的硬约束，plan 未算）

HS 口只有 `otg_dfifo_depth = 1024` 个 32-bit word = **4 KB**，由 `dcd_dwc2.c:dfifo_alloc()` **自动**按
`ceil(wMaxPacketSize/4)` 分配，**没有用户可调旋钮**。按本表实算（`bm_double_buffered = 0`，已确认是默认值）：

```
EPInfo (DMA 元数据)      →  32 word   (2 word/方向 × 16 端点；dcd_dwc2.c:291-294 从 1024 里先扣掉)
GRXFSIZ(共享 OUT)        → 304 word   (13+1+2*(512/4+1)+2*16)   dcd_dwc2.c:232
EP0 IN            64 B  →  16 word
EP1 IN  int  64 B  →  16 word
EP2 IN  bulk 512 B → 128 word
EP3 IN  bulk 512 B → 128 word
EP4 IN  bulk 512 B → 128 word
EP5 IN  int  16 B  →   4 word
EP6 IN  bulk 512 B → 128 word
------------------------------------------------
合计               → 884 word  ≤ 1024   余 140 word (560 B)
```

> ⚠️ **2026-10 更正**：本代码块原先写 **852 / 余 172**（**漏了 EPInfo 的 32 word**，
> 只有独立复算能发现 —— `uc_selfcheck()` 用的是同一个数，抓不到）。
> 正确值以本块为准：**884 / 余 140**。复算链见 §2 的更正注记。

⚠️ **一旦启用 double buffering（`_tud_cfg.bm_double_buffered`），每个 bulk IN 翻倍** → 合计 1364 word
**超过 1024** → `dfifo_alloc()` 的 `TU_ASSERT` 失败 → 末尾端点打不开 → **整个复合设备枚举失败**。
`bm_double_buffered` 默认 0，**不要打开**；若将来要开，只能给 EP3（视频）一个端点开。
`components/usb_composite` 的离线自检里必须复算这份预算并在超限时**编译期/启动期报错**。

---

## 3. 身份（完整版冻结）

| 项 | 值 | 理由 |
|---|---|---|
| `idVendor` | `0x303A` | 同探测期 |
| `idProduct` | **`0x4103`** | ⚠️ **刻意避开 Espressif 的自动 PID 空间**：`usb_descriptors.c:18-20` 的自动 PID = `0x4000 \| (CDC<<0\|MSC<<1\|HID<<2\|MIDI<<3\|AUDIO<<4\|VENDOR<<5\|MTP<<6)`，即 **0x4000~0x407F** 全被 esp_tinyusb 默认设备占满。探测期用的 0x4001 正是这个空间里的值，**真机上撞到了一台遗留的 `USB\VID_303A&PID_4001\123456` 设备节点**，Windows 直接套用了缓存的驱动状态（probe-report §1 / usb_probe.c:131-141 有完整记录）。0x4103 在该空间之外，不可能撞。 |
| `bcdUSB` | `0x0210` | 必须 ≥0x0210，否则不取 BOS |
| `bDeviceClass/SubClass/Protocol` | **`0xEF / 0x02 / 0x01`** | 复合设备 IAD 形态；`usbccgp` 按接口分发 |
| `bcdDevice` | **每次改描述符内容就 +1**（起始 `0x0100`） | 硬件 ID 含 REV → 强制新 devnode → MS OS 属性重新应用 |
| 产品串 | `MeidaP4 Composite` | |
| 序列号 | 由 **eFuse MAC** 生成（稳定） | 实例身份靠 PID + bcdDevice 保证，序列号给用户看 |

---

## 4. 字符串描述符（`USB_STRING_DESCRIPTOR_ARRAY_SIZE = 8`，**刚好用满**）

**分配规则（确定性，装配器实现）**：
1. 索引 0/1/2/3 固定：LANGID / Manufacturer / Product / Serial；
2. 然后**按功能注册顺序**，为每个声明了 `itf_string` 的功能分配索引；
   **内容相同（`strcmp` 相等）的字符串共享同一个索引**（这就是 IF1/IF2 共用一条的原因）；
3. 接口字符串全部分完之后，再**按注册顺序**为声明了 `extra_string` 的功能分配索引（同样按内容去重）。

按生产注册顺序 `MTP → video → ctrl → NCM` 实算：

| idx | 内容 | 使用者 |
|---|---|---|
| 0 | LANGID `0x0409` | — |
| 1 | `MeidaP4` | device（identity.manufacturer） |
| 2 | `MeidaP4 Composite` | device（identity.product） |
| 3 | 序列号（eFuse MAC） | device（identity.serial） |
| **4** | `MeidaP4 SD Card` | IF0 (MTP) |
| **5** | `MeidaP4 WebUSB` | IF1 + IF2（**内容相同 → 共享**） |
| **6** | `MeidaP4 Network` | IF3 (NCM) |
| **7** | NCM MAC 串（12 个大写十六进制字符） | IF3 (NCM 的 `iMACAddress`) |

> ⚠️ **2026-10 更正（verifier 独立复核提出）**：本表原先写的是 `4=WebUSB / 5=MTP`，
> **与上面那条分配规则不相容** —— 规则是"按注册顺序"，而注册顺序是 `MTP → video → ctrl → NCM`，
> 所以 4 必然落在 MTP 上。**实现是对的，原表是我写错了。**
>
> 为什么不反过来改实现去迁就原表：那样需要一条"vendor 串优先、其余按顺序"的**特例规则**，
> 而它没有任何技术依据 —— 序号的具体数值**对 USB 与 Windows 都无意义**，
> 唯一有意义的是"每个接口指向正确的内容"（`IF0→MeidaP4 SD Card`、`IF1/IF2→MeidaP4 WebUSB`、`IF3→MeidaP4 Network`）。
> 批 0 真机验证过的是 **WCID/WinUSB 绑定 + Chrome claim + MTP 枚举**，
> **字符串序号数值在其中没有任何因果作用**（probe ELF 里 `4=Vendor/5=MTP` 只是那次固件自己的注册顺序的产物，
> 不构成"必须如此"的证据）。因此按与 `resync_bytes` 相同的原则处理：**改文档去迁就正确且确定的实现**。
>
> **自检断言应盯"内容↔接口"映射，而不是盯序号数值。**

> ⚠️ 上限 8 条（`descriptors_control.h:15`），**已用满**：任何新功能都不能再要字符串索引。
> 装配器在超预算时必须让 `uc_prepare()` 返回 `ESP_ERR_NOT_SUPPORTED`（**不许静默截断**）。
>
> ⚠️ 具体的索引**数值**（4/5/6/7）是"注册顺序 + 内容去重"的**结果**，不是可独立指定的量。
> 冻结的是**这条分配规则**与"总数必须 ≤ 8"。改动注册顺序会改变这些数值 —— 这属于 §8 的冻结项。

---

## 5. MS OS 2.0 / WCID（只给 IF2「Vendor 控制」）

延用 probe-contract §2 已验证的规则，逐条**不得改动**：

- Registry Property 必须是**单数 `DeviceInterfaceGUID` + `REG_SZ (0x0001)`**，值
  `{A5DCBF10-6530-11D2-901F-00C04FB951ED}`。复数 / `REG_MULTI_SZ` 会让 `winusb.sys` 起不来（真机 Code 31）。
- CompatibleID = `WINUSB\0\0`；SubCompatibleID = 8×0x00。
- 只给 **IF2** 发 CompatibleID + Registry Property；MTP / NCM / IF1 都不发。
- `wIndex` 必须同时接受 `0` 与 `0x07`。
- `bVendorCode`：WebUSB = `0x01`，MS OS 2.0 = `0x02`。
- BOS 里 WebUSB 平台能力的 `iLandingPage` **必须为 0**（我们不做着陆页，且声明非 0 会诱导 Chrome 发 `GET_URL`）。

### 5.1 ⚠️ 关于 probe-report §5 **U8** 的更正（Lead 依据源码复核，本文件为准）

probe-report §5 U8 声称"单数 RegProp + 带 subset header"的组合**未验证**，与同报告 §2.5 冲突。**U8 的说法是错的**，依据：

- `components/usb_probe/usb_probe.c:261-267` 显示：**模式 B（复合）就带 `Function Subset Header`**，
  且模式 B 的固件（PID 0x4002）真机：`MI_01` → `Service=WINUSB` / `CompatibleIds` 含 `USB\MS_COMP_WINUSB`
  / `problem=0`（probe-report §2.1），Chrome 155 `requestClaim` + `claimInterface` **成功**（§2.5）。
- 同一份源码 `:252-260` 记录：**非复合**模式 A 带 Function Subset Header 才会失败
  （Windows 读完整 248 B 但**不应用**兼容 ID → `ProblemCode=28` → 从未被 SetConfiguration）。
  也就是说"subset header 是否该出现"取决于**设备是不是复合**，而不是"单数/复数 RegProp"。

**结论：模式 B 的形态（Set Header → Function Subset Header → CompatibleID → 单数 REG_SZ RegProp = 166 B）
是已真机验证过的形态**，完整版照抄它，因此 `CONFIG_USB_COMPOSITE_MSOS_FUNCTION_SUBSET=y` 默认开。

### 5.2 仍未验证的那一处 → 默认关掉

模式 B 的 166 B 里**没有** `Configuration Subset Header (0x0001)`。
完整版多一个功能（NCM），"是否该补 Configuration Subset"没有任何真机证据。
所以 `CONFIG_USB_COMPOSITE_MSOS_CONFIG_SUBSET` **默认 `n`**（= 与已验证形态逐字节一致），
留作上电后的 A/B 实验项。要开它只需改一个 Kconfig，不改代码。

### 5.3 ✅ 2026-10-31 T2 修订：**IF1（视频）也发 WCID**，MS OS 集 166 → 322 B

真机实测（`docs/findings-t2-2026-10-31.md` §1.2）证明上面的"只给 IF2"不够：

- 旧固件在 Windows 上：`MI_02`（IF2 控制）= WinUSB、有 device interface；
  **`MI_01`（IF1 视频）没有驱动、没有 device interface**。
- 后果：Windows 的 USB 复合父设备虽然枚举成功，但 **WinUSB 打不开 EP3**；
  Chrome（以及主机 WinUSB 工具）**拿不到任何视频帧** —— 表现是"设备枚举正常、视频永远 0 帧"。
- Windows 只会给 MS OS 2.0 **Function Subset 里列出的接口**装 `winusb.sys`，
  所以"两个 vendor 接口"必须各自有一个 subset。

修订内容（自本日起为冻结值）：

| 项 | 旧值 | 新值 |
|---|---|---|
| WCID 功能数 | 1（仅 IF2） | **2（IF1 视频 + IF2 控制）** |
| MS OS 2.0 集长度 | 166 B（10 + 1×156） | **322 B（10 + 2×156）** |
| `bcdDevice` | 0x0100 | **0x0110**（Windows 按硬件 ID 缓存 MSOS 结果，必须递增 REV 才会重取） |
| 自检条数 | 211 | **231** |
| `usb_composite` 组装器 | 只允许恰好 1 个 WCID | **支持 N 个**，逐个校验 subset 的 `bFirstInterfaceNumber` |

其余规则（单数 REG_SZ GUID、`WINUSB\0\0`、`bVendorCode`、`iLandingPage=0`、`wIndex` 0/0x07、
无 Configuration Subset）**全部不变**。

⚠️ 升级/改描述符时的操作提醒：改完 WCID 相关内容后**必须递增 `bcdDevice`**，
否则 Windows 会沿用旧缓存（真机实测：`0x0110` 一烧进去，`MI_01` 立刻变成 WinUSB）。


---

## 6. IAD（Interface Association）

- **NCM（IF3+IF4）必须带 IAD**：`bFirstInterface=3`、`bInterfaceCount=2`、
  `bFunctionClass=0x02 / SubClass=0x0D / Protocol=0x00`。
  ⚠️ `esp_tinyusb 2.3.0` 的**默认** NCM 描述符缺 IAD（2.4.0 才修）。我们**自带**描述符，因此不受影响 ——
  这是"必须自带描述符"的又一个理由。
- MTP / Vendor 单接口功能**不加 IAD**（单接口 IAD 是非法的且会干扰 `usbccgp` 分发）。
- `bDeviceClass = 0xEF/0x02/0x01` 只在**配置里存在多接口功能时**才设；本方案恒为复合，恒设。

---

## 7. 构建槽位（并发纪律）

沿用 `tools/build.ps1`，但完整版新增 `-Mode prod`：

```powershell
pwsh -File tools/build.ps1 -Mode prod -Slot <你的名字>
# == idf.py -B build-<slot>-prod -D SDKCONFIG=sdkconfig.<slot>-prod `
#           -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.prod.defaults" build
```

| 写入者 | Slot | 备注 |
|---|---|---|
| Lead | `main` | 唯一改 `main/idf_component.yml` / `dependencies.lock` / `managed_components/` 的人 |
| WS-storage | `storage` | |
| WS-net | `net` | |
| WS-webusb | `webusb` | |
| WS-video | `video` | |
| verifier | `verify` | **只读**代码，只写 `build-verify-prod` |

**并发纪律**：
- `managed_components/`、`dependencies.lock`、`main/idf_component.yml` 是**共享且互斥**的：**只有 Lead 改**。
  其他写入者一旦需要新依赖，**报 Lead**，不要自己动 manifest。
- **`IDF_COMPONENT_MANAGER=0` 不可用**（2026-10 实测 + IDF 源码复核）：
  IDF v6.1 只在管理器**启用**时才注册 `managed_components/`（`build.cmake:653-724`）；
  关闭后目录完全不参与构建，而 `espressif__esp_tinyusb/CMakeLists.txt:78-84` 会去要求一个
  **名字就叫 `tinyusb`** 的组件（注册表装出来的目录名是 `espressif__tinyusb`）→ configure 直接失败。
  把 `managed_components` 塞进 `EXTRA_COMPONENT_DIRS` 也无效（组件名取自目录基名，`component.cmake:159`）。
- 因此大家**都带着组件管理器构建**：`tools/build.ps1` 已内置**锁冲突自动重试**（默认 3 次、间隔 20 s）。
  它还会在检测到环境里残留 `IDF_COMPONENT_MANAGER=0` 时清掉并告警。
- 绝不用裸 `idf.py build`（会写共享 `build/` 与根 `sdkconfig`）。不要 `idf.py fullclean`。
- ⚠️ **新增一个组件目录后，必须至少 `-Clean` 一次**（WS-net 2026-10 实测）：
  组件目录列表是 **configure 期 glob** 的，只在 build 目录里跑 ninja **不会重跑 cmake**，
  新组件不会进构建图 —— 表现是"我明明加了 CMakeLists.txt，符号还是 undefined"。
  ```
  pwsh -File tools/build.ps1 -Mode prod -Slot <slot> -Clean
  ```
  （`-Fresh` 更彻底，但会重生成 sdkconfig。）
- ⚠️ **`-ExtraDefaults` 实验之后，回到正式构建必须重新生成 sdkconfig**（WS-storage 2026-10 实测）：
  IDF 6.1 的 Kconfig 策略是 **`use sdkconfig`**（构建日志原文）⇒ **已存在的 sdkconfig 会覆盖 defaults 文件**。
  两个方向都会产生**假绿**：
  1. 加了 `-ExtraDefaults` 但槽位已有 sdkconfig ⇒ 实验配置**没生效**（你以为在做诊断构建，其实没有）；
  2. 用实验配置生成过 sdkconfig 后，再用**正式命令**构建 ⇒ 正式命令**也没生效**，
     你拿到的 `exit 0` 是**实验配置**的。

  **`tools/build.ps1` 已加结构性防护**：把"生成这份 sdkconfig 时用的 defaults 链"戳进
  `sdkconfig.<slot>-<mode>.defaults-stamp`，一旦不一致就**自动删除 sdkconfig** 强制重新生成，
  并打一条醒目的横幅。所以你**不需要**再手工记这件事 —— 但请理解它为什么存在。
  **验收证据里请附上 `sdkconfig.<slot>-<mode>` 的关键项**（例如 `CONFIG_TINYUSB_NET_MODE_NCM=y`），
  用来证明"这个 ELF 的配置确实是冻结配置"。
- **任何时刻 `components/**` 都必须能编译通过**：所有组件共用一个构建图，一个人的半成品会让所有人挂。

---

## 8. 冻结清单（改任何一项都必须 Lead 批准 + 同步通知）

1. 接口号与功能归属（§1）
2. 端点号、方向、类型、`wMaxPacketSize`（§2）
3. `bcdDevice` / PID / 字符串索引（§3/§4）
4. MS OS 2.0 描述符形状与 GUID（§5）
5. IAD 归属（§6）
6. `components/usb_composite/include/usb_composite.h` 里的任何结构体字段或函数签名（I1）

**牵一发动全身**：以上任何改动都会同时影响固件描述符、`web/proto.js` 侧的能力协商、以及 Windows 实例身份。
