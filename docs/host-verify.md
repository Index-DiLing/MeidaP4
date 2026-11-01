# 主机侧验收清单 · Win11 + Chrome/Edge

> 对象：`web/probe.html`（单文件 WebUSB 探测页）+ 探测固件 `mode a` / `mode b`。
> 契约：`docs/probe-contract.md` §4（线协议 v0）、§6（主机侧验收）；冒烟项：`docs/probe-plan.md` §3 A/B。
> **本清单目前全部处于「未实测」状态**：编写时本机未插 ESP32-P4 开发板，没有任何一步被真机跑通。
> 下面每步都写成可复现的「操作 / 预期现象 / 失败判读」，等板子插上后由用户或 Lead 逐步执行。
> 执行人请在 §10 结果表里逐行填数，再由 Lead 回填 `docs/probe-report.md`。

---

## 0. 交付物静态自检（不需要板子，已完成）

| 检查 | 命令 | 结果 |
|---|---|---|
| 内联 `<script>` 无语法错误 | 抽出内联脚本 → `node --check` | 见 §11（执行记录） |
| 无外部依赖 | grep `src=` / `href=` / `@import` / `require(` | 见 §11：0 处指向外部 URL |
| 单文件、零构建 | 目录里只有 `probe.html` 一个文件 | 成立 |

约束：页面只使用标准 WebUSB API（`navigator.usb`），不依赖任何 CDN / 框架 / 构建步骤 / 实验特性。

---

## 1. 准备

| 项 | 要求 |
|---|---|
| 主机 | Win11（x64） |
| 浏览器 | **Chrome** 或 **Edge**，桌面版，版本尽量新（WebUSB 在桌面端默认开启，无需改 flag）。Firefox / Safari **不支持** WebUSB |
| 固件 | `mode a`（PID `0x4001`）先跑；`mode b`（PID `0x4002`）后跑 |
| 数据线 | 插 **ESP32-P4 的 HS USB 口**（不是 UART 口），线材必须是数据线（不是纯充电线） |
| 页面路径 | `E:\ESP_PROJECT\MeidaP4\web\probe.html` |
| 固件日志 | 另开一个窗口跑 `tools/flash.ps1` + 串口监视，后面每步都要对照设备侧日志 |

> ⚠️ **不要**从 `http://192.168.x.x` 打开这个页面。见 §9.5。

---

## 2. 步骤 A · 环境自检：证明 `file://` 是安全上下文

**操作**
1. 在资源管理器里双击 `web\probe.html`，或者地址栏输入 `file:///E:/ESP_PROJECT/MeidaP4/web/probe.html`。
2. 看页面 §1「环境自检」表格。
3. 顺手把地址栏来源改成 `http://localhost:8000/`（在 `web/` 下跑 `python -m http.server 8000`）再看一次。

**预期现象**
| 字段 | `file://` 预期 | `http://localhost` 预期 |
|---|---|---|
| `isSecureContext` | `true` | `true` |
| `location.protocol` | `file:` | `http:` |
| `navigator.usb 存在` | `存在 (navigator.usb)` | `存在 (navigator.usb)` |
| 总体判定徽章 | 绿色「可用：安全上下文 + navigator.usb 存在」 | 绿色 |
| 提示条 | 蓝色，明确写出「已确认：file:// 在 Chromium 下 isSecureContext === true」 | 蓝色 |

**失败判读**
- `isSecureContext` 是 `false` → 你打开的不是 `file://` 也不是 `localhost`。**这是契约 R1 的关键判据**：`file://` 方案若为 false，整个"双击即用"路线不成立，必须改用 `http://localhost`。
- `navigator.usb 不存在` → 你用的是 Firefox/Safari，或者浏览器版本太老。换 Chrome/Edge。
- 页面完全空白 → 不该发生（本页所有入口都包了 try/catch 且有全局 `error` / `unhandledrejection` 兜底）。真白屏请按 F12 看 Console 首条红色报错，连截图一起贴回会话——这是页面 bug，不是固件问题。

---

## 3. 步骤 B · 设备出现在 `requestDevice` 列表里

**操作**
1. 确认板子已插好、串口日志能看到 `tud_mount_cb()`（或等价"已挂载"日志）。
2. 点页面 §2 的 **「请求设备 requestDevice(VID 0x303A)」**。
3. 看弹出的设备选择框。

**预期现象**
- 选择框里出现一条设备，名字类似 `MeidaP4 Probe`（或 `MeidaP4 Probe (PROBE-0001)`）。
- 选中并点「连接」后，页面 §2 出现设备标识表与描述符清单，§1 无红色报错。

**失败判读 —— 「列表里根本看不到设备」**（冒烟 A 最常见的卡点）
按下面顺序排查，**逐条独立**：

1. **缺 WebUSB BOS 平台能力**（最可能）。
   契约 §2 要求 BOS 里有两项 device capability，其中 WebUSB 平台能力的 UUID 必须是
   `{3408b638-09a9-47a0-8bfd-a0768815b665}`。**没有它，Chrome 永远不会把这个设备暴露给 `navigator.usb`** ——
   设备在设备管理器里可能一切正常，但 WebUSB 就是看不见。
   判据：`chrome://usb-internals` 里能看到设备（见 §9.4），但 `requestDevice` 列表里没有 → 就是缺 WebUSB 平台能力。
   补充：`esp_tinyusb 2.3.0` **完全没有** BOS/WebUSB 支持（见 `probe-plan.md` §4.1），必须由 `usb_probe` 自己实现
   `tud_descriptor_bos_cb()` + `TUD_BOS_WEBUSB_DESCRIPTOR()`。若固件没写，这一步必然失败。
2. **`bcdUSB < 0x0210`**。
   Windows 与 Chrome 都不会去取 BOS。页面 §2 的 `USB 版本 (bcdUSB)` 字段会显示 `< 0x0210` 并标红。
   契约冻结值是 **`0x0210`**（`probe-contract.md` §2）。
3. **`bDeviceClass` 不是 `0xEF/0x02/0x01`**。复合设备（Misc/IAD）这一组不对时，Windows 不会挂 `usbccgp`
   按接口分发，接口级的 WebUSB 能力也就无从谈起。页面 §2 的 `设备类/子类/协议` 字段会标注是否匹配。
4. **设备根本没枚举成功**（连设备管理器里都看不到）→ 先解决 §9.1，再回来做这步。
5. **Windows 已经给它绑了别的驱动**（不是 WinUSB）→ WebUSB 仍可能看不到；先做 §9.2 的驱动检查。
6. **选择框里出现过但你没勾选「记住此设备」**，刷新后需要重新 requestDevice。`file://` 来源下授权通常**不持久**
   （origin 是 opaque 的），刷新后请点「列出已授权设备 getDevices()」确认；若为空属正常，重新 requestDevice 即可。

---

## 4. 步骤 C · 核对描述符清单（契约 §1 / §2）

**操作**
1. 点「打开 open()」。
2. 若 `当前配置 configuration` 显示 `null（未配置）`，点「执行」（`selectConfiguration()`）。
3. 读 §2 的「描述符全清单」与「接口分类」表。

**预期现象**

模式 A（PID `0x4001`）：

| 项 | 预期 |
|---|---|
| `vendorId / productId` | `0x303A` / `0x4001` |
| `productName` | `MeidaP4 Probe` |
| `serialNumber` | `PROBE-0001` |
| `bcdUSB` | `0x0210`（页面显示 `2.1.0`） |
| `bcdDevice` | `1.0.0`（`0x0100`） |
| 设备类/子类/协议 | `0xEF / 0x02 / 0x01` ✔ |
| 配置数 | 1 个配置 |
| 接口数 | **1** 个（IF0） |
| IF0 | class `0xFF`（vendor-specific）→ 判定列写「Vendor-specific (0xFF) — WebUSB 回环候选」 |
| IF0 端点 | 2 个：`EP 0x01 OUT bulk 512` + `EP 0x81 IN bulk 512`（**HS bulk packetSize 应为 512；若显示 64 说明枚举在了 FS 口**） |

模式 B（PID `0x4002`）：

| 项 | 预期 |
|---|---|
| `productId` | `0x4002` |
| 接口数 | **2** 个 |
| IF0 | class `0x06`（Still Image/PTP）→ 判定列写「疑似 MTP，不要 claim」；alternate 里有 interrupt(64B) 事件端点 + bulk 数据端点 |
| IF1 | class `0xFF` → 「Vendor-specific (0xFF) — WebUSB 回环候选」；端点 `EP 0x03 OUT bulk 512` + `EP 0x83 IN bulk 512` |
| 页面自动选择 | 会自动选中 **IF1**（第一个 0xFF），日志里会打印「自动选中 vendor 接口 IF1 alt0（IN EP 3 / OUT EP 3）」 |

**失败判读**
- **模式 A 只看到 1 个 IN 端点（`EP 0x81`）而没有 OUT，或反之** → 固件配置描述符组装错了。契约 §1 要求模式 A 是 1 IN + 1 OUT。
- **`packetSize` 显示 64 而不是 512** → 设备跑在 **Full Speed** 而不是 High Speed。检查是否用了
  `TINYUSB_PORT_HIGH_SPEED_0`，以及数据线/口是否正确。
- **`bcdUSB` 不是 `0x0210`** → 见 §3 失败判读第 2 条。
- **`serialNumber` 为空** → 字符串描述符索引 3 没做，或 `esp_tinyusb` 字符串数组没配到。模式 B 会话稍后仍能跑，但请记录。
- **模式 B 下 IF0 的 class 不是 `0x06`** → Windows 不会把 MTP 挂到 `wpdmtp.sys`，§8 的 MTP 验收会失败。
- **配置数为 0 / 描述符清单是空的** → 设备未配置：点「执行」`selectConfiguration()`；仍为空说明配置描述符没被主机接受，回到 §9.1。

---

## 5. 步骤 D · `claimInterface`（**冒烟 A 的关键证据**）

**操作**
1. 在 §3 选好接口：模式 A 选 IF0，模式 B **选 IF1**（页面一般已自动选好）。
2. 点「claimInterface()」。
3. 看结果横幅，以及 `err.name（原文）` / `err.message（原文）` 两个字段。

**预期现象**
- 绿色横幅：`claimInterface SUCCESS`，`err.name (原文)` 与 `err.message (原文)` 都是 `(无异常)`。
- 页面日志出现 `claimInterface(0/1) 成功`。
- 之后「开始回环」可用。

**失败判读（按 `err.name` 原文分流；页面已内置同样的提示）**

| `err.name` | 含义 | 处置 |
|---|---|---|
| **`SecurityError`** | **该接口被内核驱动占用 —— Windows 没给它绑 `winusb.sys`** | 见下方专项排查 |
| `NetworkError` | 设备已断开或正在复位 | 检查插口是否松动、是否刚点过 reset、串口日志是否见掉线 |
| `NotFoundError` | 接口号 / alternate 不存在 | 模式 A 只有 IF0；模式 B 是 IF0(MTP)+IF1(Vendor)。**模式 B claim IF0 会失败**，必须用 IF1 |
| `InvalidStateError` | 设备未打开，或该接口已被本页 claim 过 | 先「打开 open()」；若重复 claim，先「releaseInterface()」 |
| 其他 | 未归类 | 把 `err.name` 原文贴回会话，并查 §9.3 `chrome://device-log` |

**`SecurityError` 专项排查（对应风险点 R1）**
1. 「设备管理器」里找到 Vendor 接口对应的子节点（模式 B：实例 ID `...&MI_01`；模式 A：无 MI 或 `MI_00`）。
2. 右键 → 属性 → **驱动程序 → 驱动程序文件**：**必须出现 `winusb.sys`**。
   - 若出现 `usbccgp.sys` 之外的其它函数驱动、或根本没有驱动 → WCID 没生效。
3. 检查 MS OS 2.0 描述符是否生效：
   - 该接口的 **Compatible ID 应为 `WINUSB`**。设备管理器 → 属性 → 详细信息 → 选「**匹配设备 ID**」/「硬件 ID」，
     正常应能看到 `USB\MS_COMP_WINUSB` 或 `USB\VID_303A&PID_4002&MI_01` 这类条目。
   - 若只有 `USB\VID_...&MI_01` 而没有 `MS_COMP_WINUSB` → **MS OS 2.0 描述符没生效**（BOS 里缺 MS OS 2.0 平台能力，
     或 `Function Subset Header` 的 `bFirstInterface` 指错了接口）。
   - 契约 §2 要求：MS OS 2.0 平台能力 UUID `{d8dd60df-4589-4cc7-9cd2-659d9e648a9f}`、`dwWindowsVersion=0x06030000`、
     `bVendorCode=0x02`；描述符集里 `CompatibleID="WINUSB\0\0"`。**MTP 那一路不要发兼容 ID**。
4. 兜底：用 **Zadig** 给该接口手动装一次 WinUSB（`Options → List All Devices`，选到 Vendor 接口那一项，
   注意别选错成 MI_00/MTP）。装完立刻重试 claim——若 Zadig 一装就成功，则**结论是固件 WCID 有问题**，
   而不是 WebUSB 方案不可行（这条正是 `probe-plan.md` §R1 要区分的）。

---

## 6. 步骤 E · 心跳读取（协议 v0 的 IN 侧特征）

**操作**
1. 保持 claim 状态，**不要发任何 OUT 数据**，让设备空闲 ≥ 1 s。
2. 点 §5 的「读心跳」。
3. 看「设备运行时间」与「最近原始字节」。

**预期现象**
- 日志出现 `#n 心跳: HB <ms> ms`，`设备运行时间` 显示一个递增的毫秒值（如 `12345 ms (12.35 s)`）。
- 「最近原始字节」的 ascii 一栏能看到 `\nHB 12345\n` 这样的转义形式。
- 连续点多几次，「收到心跳行」计数增加，且 uptime 单调递增。
- 设备侧串口日志按契约 §3.3 每 5 s 打印 `usb_probe_get_stats()`。

**失败判读**
- **超时（3 s 内 0 字节）** → 设备没有实现"空闲 1 s 主动发心跳"，或心跳发到了别的端点。
  对照契约 §4：心跳必须发在**回环那条 IN 端点**上。注意本步超时**不代表失败**——若设备刚好在忙，重试一次。
- **收到字节但解析不出 `HB` 行** → 日志会打印原始 ascii。把 `hb-raw` 的 hex 原文贴回会话，让 usb-core 对齐格式
  （契约 §4 是 `\nHB <uptime_ms>\n`，注意前后各有一个 `\n`）。
- **`设备运行时间` 不递增** → 设备侧 uptime 计数器用的不是毫秒，或没随 tick 更新。
- 若这一步超时且页面提示"超时"，**不要**以为板子挂了：心跳是"空闲才发"的，属正常分支。

---

## 7. 步骤 F · 回环 1 KB / 64 KB / 1 MB（冒烟 A 的收尾项）

**操作**
1. 保持 claim 状态。
2. §4 选择数据量 **1 KB** → 点「开始回环」。
3. 依次重跑 **64 KB**、**1 MB**；**每个数据量各跑两次**（第二次专门验证"用例间不再状态泄漏"）。
4. 每次记录「一致字节数」「`rawInBytes`/`hbBytes`/`strayLFlines`/`k`/`k − hbBytes`」「首处失配」「测量前排空」「总耗时」「吞吐量」「RTT min/avg/max」。

**预期现象**
- 「逐字节比对」显示绿色 `全部一致 ✔（0 字节失配）`，且 **`k − hbBytes` 显示 PASS 徽章（= 0）**。
- 「测量前排空」显示 `✔ 已对齐到行边界`，并给出丢弃了多少字节（这些**不**计入 `rawInBytes`）。
- 「疑似残留 LF 行 strayLFlines」为 `0`。
- 「**ZLP 次数 zeroLenReads**」**通常 > 0**（预计 64 KB 约 16 次、1 MB 约 256 次，即"每 4096 B 块一次"）—— 这是**正常现象**，见 §7.4。
- 「一致字节数」等于目标字节数（**这不是 IN 总量**，见 §7.1）。
- 1 MB 一次跑完，不卡死；连续跑两次结果都 PASS。
- RTT 量级：HS bulk 4 KB 一块，正常情况下应在**亚毫秒到几毫秒**；1 MB 吞吐视固件实现，量级 MB/s。
- 页面进度条推进到 100%。

**失败判读**

| 现象 | 判读与处置 |
|---|---|
| **卡死不动，`transferOut` 永不 resolve** | 这就是「先发 1 MB OUT、再读 IN」的经典死锁：设备 vendor RX/TX FIFO 各 4 KB + 8 KB stream buffer，FIFO 满了就一直 NAK。**本页已按流水线实现（先挂 IN 读、再发 OUT，分块 ≤ 4 KB）**，若仍卡死说明固件侧没有及时把 RX 数据搬走（回环任务优先级/阻塞问题），属 `usb_probe` 缺陷 |
| **首处失配在全局偏移 0，且只有约 1/256 字节偶然对上** | **典型的"常量位移 k"指纹** → 起点未对齐/状态泄漏。看 `strayLFlines`（应 0）与「测量前排空」是否 `✔`；详见 §7.1 / §7.2 |
| **失配发生在固定偏移（比如正好 4096、8192…）** | 设备侧回环缓冲区边界处理错了（丢弃/重复了跨块数据）。记录首处失配偏移+块号 |
| **失配是零星随机字节** | USB 层不可能出错（有 CRC），所以这必然是固件回环代码的数据竞争/缓冲区复用问题 |
| **`transferIn` 超时** | 勾选「超时后自动 device.reset()」会让页面立即复位设备（因为**WebUSB 没有 `cancelTransfer`**，挂起的传输只能靠 reset 终结）。复位后需重新 `selectConfiguration` + `claimInterface` 再跑。若频繁超时，说明固件回环任务停了 |
| **吞吐量明显低于预期（比如 < 1 MB/s）** | 每次 `transferIn` 只等 1 个 4 KB 块、串行往返，理论吞吐受 RTT 限制。可用更小的数据量做 RTT 观察；真正的吞吐结论要等完整版做双向流水线后才有意义。**本页只证明"通路成立"，不做性能结论** |
| **`claim` 之后仍报 `SecurityError`** | 回到 §5 的专项排查 |
| **1 KB 通过、1 MB 失配** | 先看 §7.1 的字节对账：若 `k − hbBytes != 0` 或 `strayLFlines > 0` 则是**页面侧对齐问题**（已修，若复现请贴报告）；若字节账平但仍有失配，才怀疑固件跨块逻辑 |
| **`测量前排空` 显示 `✘ 未对齐`** | 页面**拒绝**在未对齐的流上测量（这是设计行为）。设备侧积压的开头是半行、读到上限仍对不齐 → 等设备稳定后重跑，或把 `strayLFlines`/`drain` 细节贴回 |
| **`用例开始时在飞行的传输数 > 0`** | 上一用例异常收尾，本轮已自动 `reset()` 回收。本轮结果可信，但请把上一用例的「中止原因」一并记录 |
| **`zeroLenReads` 超过 4096** | 触发防自旋保护，报 `ProbeZeroLenSpinError` 并由总超时接管 → 设备疑似只发 ZLP 不再给数据，贴回报告 |
| **用例中止时 `k`/`k − hbBytes`/`无法解释` 显示 `N/A（用例中止）`** | **这是正确行为，不是缺陷**。中止时 `k = rawInBytes − size` 是负数（根本没读完），派生量在数学上无意义。旧版把它标成 `✘ 页面 bug`，会把"中止"误报成"字节消失"。中止的用例只报「已比对 X / 期望 Y B」，**不做通过判定** |

### 7.1 字节对账（task-10 新增的验收判据，**比"失配为 0"更硬**）

页面每个回环用例在测量前会先**排空 IN 并对齐到心跳行边界**，然后只对测量期做字节记账。指标位于页面 §4 的指标区，也在导出报告第 5 节：

| 字段 | 含义 | 预期 |
|---|---|---|
| `rawInBytes` | **测量期**从 IN 读到的原始字节总数（不含排空丢弃的字节） | ≥ `size` |
| `hbBytes` | 其中被完整剥离的心跳行字节数（`\nHB <ms>\n` = `数字位数 + 5` 字节） | 任意 |
| `strayLFlines` | 不构成心跳行、被**当成数据**吐出去的 `0x0A` 个数 | **必须 0** |
| `zeroLenReads` | 0 字节 `transferIn` 完成次数 = **ZLP 次数** | 通常 > 0（正常，见 §7.4） |
| `k = rawInBytes − size` | 流的位移量 | 应等于 `hbBytes` |
| **`k − hbBytes`** | ★ **验收判据** | **必须 0（PASS 徽章）** |
| `一致字节数（= size − 失配）` | 与目标逐字节相同的字节数 | `= size` |
| `测量前排空` | 排空次数 / 丢弃字节 / 是否对齐 | `✔ 已对齐到行边界` |
| `surplus 结转残留` | 跨块余量 | `0 B` |
| `账目分解` | `k − hbBytes = surplus + carry`，且 **`无法解释` 必须 0** | `✔ 拆解干净` |
| `字节对账不变量` | `partsIn == delivered + surplus` | `✔ 总收到字节 == 各段之和` |

**恒等式（页面内部据此自检）**
```
rawInBytes = hbBytes + partsIn + carry      partsIn = delivered + surplus      delivered = size（跑满时）
⇒ k = rawInBytes − size = hbBytes + surplus + carry
⇒ k − hbBytes = surplus + carry             ← 仅有的两个"合法"差值来源
⇒ 无法解释 = (k − hbBytes) − surplus − carry   ← 必须恒为 0，否则是页面 bug（字节真的消失）
```
- `carry > 0`（用例结束时扣着一个尾部半行心跳）属**排布现象**，不是丢字节；
- `无法解释 != 0` 才是真 bug（对应旧的 `appendParts()` 静默丢 surplus 那类问题）；
- **以上判据只在用例正常跑完时给出**。用例**中止**时 `k = rawInBytes − size` 会是负数
  （根本没读完），派生量在数学上无意义 → 页面显示 **`N/A（用例中止）`**，并只报「已比对 X / 期望 Y B」。
  **中止绝不等于通过**。
- 验收结论要求 **正常跑完 且 `mismatchCount == 0` 且 `k − hbBytes == 0` 且 `无法解释 == 0` 且不变量成立**。

**⚠ 别把「一致字节数」读成「实收字节数」。** 旧版把这个字段标成"实收"，用户那次看到 `4098 B` 以为"设备只回了 4098 字节"——其实设备回满了 1 MB；`4098 = 1048576 − 1044478` 只是**偶然对上的字节数**（随机数据下期望命中率 1/256 ≈ 4096，与 4098 吻合）。IN 总量看 `rawInBytes`。

**判读口诀（与 host-usb 的工具口径一致）**

| 现象 | 结论 |
|---|---|
| `k − hbBytes == 0` 且 `mismatchCount == 0` | **PASS**，链路与页面对齐都正确 |
| `hbBytes == 0` 而 `rawInBytes > size` 且 `strayLFlines > 0` | **状态泄漏 / 起点未对齐**：有非心跳的 `0x0A` 被当成数据 → 整条流位移 |
| 首处失配在**全局偏移 0** 且命中率 ≈ 1/256 | "常量位移 k"指纹，k 就是漏进来的额外字节数 |
| `测量前排空` 为 `✘ 未对齐` 或用例直接中止 | 积压开头是半行、排空到上限仍对不齐 → 页面已拒绝测量，先解决积压 |
| `surplus 结转残留 > 0` 或 `不变量` 报 `✘` | 页面侧字节丢失（页面 bug），请连报告一起贴回 |
| `k − hbBytes != 0` 但 `mismatchCount == 0` | 字节账不平（多读/少读），仍判 **FAIL** |

**排空这一步做了什么**：设备只要"已挂载且无人读 IN"就每秒无条件发一次 `\nHB <ms>\n`，会积满设备侧 FIFO（host-usb 实测 ≈4 KB / 374 行）。带着积压开测时，若积压开头是**半行**，剥离器识别不出 → 那些字节被当成 payload 数据 → **整个 IN 流从偏移 0 位移 k 字节**（用户那次 k ≈ 4098）。所以每个用例开始前先把 IN 读到"行边界 + 刚消费过一个完整心跳行"，并**把丢弃字节数记进日志与报告**（丢弃量**不**计入 `rawInBytes`，因此 `k − hbBytes == 0` 仍成立）。

> 想**复现**错位以验证因果：取消勾选「测量前排空 IN、对齐到心跳行边界」，让设备空闲几秒积压心跳，再跑 1 MB —— 应重新出现 `strayLFlines > 0` 且首处失配在偏移 0。复现完记得勾回去。

### 7.2 跨用例泄漏（task-10 的另一处修复）

页面现在保证：
1. **每个用例把心跳过滤器状态彻底清零**（`reset()` 清 carry 与全部计数），不允许任何跨用例残留 —— host-usb 已 hexdump 证实：残留 `_in_line/_pending` 会让下一个用例的第一个 `0x0A` 去闭合陈旧残片，把 `\nH` 当数据吐出，**从偏移 0 整体错位**；
2. **用例开始时若仍有"在飞行"的传输**（上一用例或心跳读取留下的、WebUSB 无法 cancel 的挂起 promise），会**先 `device.reset()` 回收**再测，否则那个挂起读会偷走本次的 IN 字节；
3. 排空阶段首个读超时且**零积压**（设备不发心跳的情况）时，那个超时的读会被**收养**进测量阶段，而不是留下并发读互相抢字节。

报告里对应字段：`用例开始时在飞行的传输数`（>0 说明发生过自动回收）、`用例结束时在飞行的传输数`、`因超时废弃的挂起传输数`。

### 7.3 ZLP（零长度包）语义 —— 真机踩到的新缺陷，已修（task-10 后续）

**用户实测现象**（64 KB 用例）：
```
测量前排空：读 1 次 / 9 B（完整心跳行 1 行 / 9 B），残留 LF 0，carry 0 B，对齐=是(alignedAfterHb)
第 1 块失败 @offset 4096: ProbeEmptyReadError: transferIn 返回 0 字节（设备无数据/可能已停）
结束：中止（第 1 块失败）
一致 65536 / 65536 B；失配 0 B；rawInBytes=4318；hbBytes=222
k=-61218；k−hbBytes=-61440 ✘（surplus=0 + carry=0，无法解释=-61440 ✘ 页面 bug）
字节对账不变量：partsIn=4096 delivered=4096 surplus=0 检查 2 次，破坏 0 次 ✔
```
用户补充：**"1 KB 可以，超过 4 KB 就不行"**。

**根因（页面侧，不是设备）**：`transferIn` 返回 **0 字节是合法的 ZLP**，不是错误。
设备侧（`tud_vendor_write`）写出的长度恰好是 **max packet(512) 的整数倍**、且写完流就空了时，
TinyUSB 会补一个 ZLP（`tu_edpt_stream_write_zlp_if_needed` 那类逻辑）；
WebUSB 把它作为**一次独立的 0 字节 `transferIn` 完成**返回。旧版代码把它当致命错误直接中止了用例。
- **为什么 1 KB "可以"**：1 KB 那次请求的读长度（4096）大于实际可用（1024）→ 收到**短包**即完成，不需要 ZLP。
- **为什么"第 1 块 @offset 4096"失败**：第 0 块正好 4096 B（512×8）→ 设备补 ZLP →
  这个 ZLP 成了**第 1 块的第一次读**。`partsIn=4096 delivered=4096` 也证明第 0 块本身完好无损。
- **设备没坏**：host-usb 的 WinUSB 工具在同一块板、同一版固件上 1 KB/64 KB/1 MB 全部 `mismatch=0`。

**修复**
1. 0 字节 `transferIn` 记为合法事件：计 `zeroLenReads`、**不计入** `rawInBytes`/`hbBytes`、不消耗 `want`、**继续读**；
2. 防自旋：累计超过 **4096** 次才判失败（`ProbeZeroLenSpinError`）；**用例总超时是最终兜底**（读循环内部也检查）；
3. `zeroLenReads` 进日志与导出报告 —— 它是"设备在发 ZLP"的直接证据；
4. 中止的用例不再把 `k`/`k − hbBytes`/`无法解释` 标成 `✘`，改为 `N/A（用例中止）`；
   并把误导性的「一致 65536 / 65536 B」改为「**已比对 4096 / 期望 65536 B**」，中止**不做通过判定**；
5. 同一端点**绝不留悬空读**：排空失败/未消费的收养读一律 `device.reset()` 回收（WebUSB 会拒绝同一端点的并发读）。

**判读**：`zeroLenReads > 0` 是**正常**的，预计 4096 B 块 → 每块一次 ZLP（64 KB≈16 次、1 MB≈256 次）。
> ⚠ **这条要写进正式 I0 协议**：协议必须把 ZLP 语义定死（"ZLP 是合法的零长度包，主机必须忽略并继续读"），
> 否则每个主机实现都会踩同一个坑（本页、host-usb 的工具都踩过）。

### 7.4 关于"取消/超时"的实现说明（写进报告的注意项）

WebUSB **没有** `cancelTransfer`（[WICG/webusb#25](https://github.com/WICG/webusb/issues/25) 仍未关闭；
[MDN `USBDevice`](https://developer.mozilla.org/en-US/docs/Web/API/USBDevice) 的方法列表里没有它）。
所以本页的"IN 侧超时与取消"实现为：
1. `Promise.race([transferIn(...), 定时器])` 做超时；
2. 超时后把那个挂起的 promise 标记为**废弃**（`deprecated` 计数会写进报告）；
3. 勾选「超时后自动 device.reset()」时，用 `device.reset()` 让设备重新枚举，使所有挂起传输以 rejected 结束；
4. 「停止回环」按钮同样用 `device.reset()` 兜底。

`reset()` 之后设备被**取消配置**、所有 claim 失效，需要重新 `selectConfiguration` + `claimInterface`。页面已自动处理重选配置。

---

## 8. 步骤 G · 模式 B：MTP + Vendor 同时成立（冒烟 B）

**前置**：重新烧 `mode b` 固件（PID 会变成 `0x4002`），插拔一次让 Windows 重新枚举。

### 8.1 MTP 侧

**操作**
1. 打开「此电脑 / 资源管理器」。
2. 找名为 **`MeidaP4 Storage`** 的便携设备（在「便携设备」或「此电脑」下方）。
3. 双击进入，看片内 `storage` 卷，确认存在 `PROBE.txt`，并尝试复制一个文件进去/出来。

**预期现象**
- 资源管理器出现 **`MeidaP4 Storage`**。
- 里面能看到 `PROBE.txt`；能读能写。
- 设备管理器里该节点位于「便携设备」类别下。

**失败判读**
- **完全没有便携设备** → MTP 那一路没起来：`mtp_probe_start()` 没在 `usb_probe_start()` 之前调用，或 FATFS `storage` 分区挂载失败，
  或 IF0 的 class 不是 `0x06`（见 §4）。先看串口日志里 `mtp_probe_start()` 的返回值。
- **出现"未知设备"/黄色感叹号** → 见 §9.1。
- **能看到设备但进去是空的 / 打不开** → FATFS 挂载或格式化失败，看串口日志。
- **`PROBE.txt` 不在** → `mtp_probe_start()` 里写探测文件那步失败。

### 8.2 同一时刻，WebUSB 侧仍能 claim Vendor 接口

**操作**
1. 页面点「请求设备」（此时应出现 `MeidaP4 Probe`，PID `0x4002`）。
2. 打开 → `selectConfiguration` → 确认页面自动选中 **IF1**（0xFF）。
3. `claimInterface()` → 「开始回环」1 MB。

**预期现象**
- MTP 的便携设备**仍然在**资源管理器里（互不干扰）。
- claim IF1 成功；回环逐字节一致。

**失败判读**
- claim IF1 抛 `SecurityError` → 见 §5；注意**不要**去 claim IF0（MTP，必然失败）。
- MTP 在网页 claim 之后**消失**了 → 说明固件把两个功能耦合了（比如复位了整个设备），这直接推翻 R2，
  需要报回会话作为"决策变更"候选。

### 8.3 怎么判断 MTP 与 WinUSB 各绑到哪个接口（三种独立方法，至少做两种）

**方法 1 · 页面里看接口类（最直接）**
在 §2 的「接口分类」表里：interface `0` 的 class 是 `0x06`（Still Image/PTP）→ 那就是 MTP；
class 是 `0xFF`（vendor-specific）的那个接口 → 那就是 WebUSB/WinUSB 那一路。

**方法 2 · 设备管理器看实例 ID 的 `&MI_xx`（复合设备子节点）**
`bDeviceClass=0xEF/0x02/0x01` 让 Windows 挂 `usbccgp`，父节点 `MeidaP4 Probe` 下面会分出**每个接口一个子节点**，
实例 ID 里的 `MI_xx` 就是**接口号**（十六进制）：

| 实例 ID | 对应 | 应绑的驱动 |
|---|---|---|
| `USB\VID_303A&PID_4002&MI_00` | **接口 0 = MTP** | `wpdmtp.sys`（WPD 走 UMDF，服务名一般是 `WUDFRd`）→ 显示为「便携设备 / `MeidaP4 Storage`」 |
| `USB\VID_303A&PID_4002&MI_01` | **接口 1 = Vendor** | `winusb.sys`（服务名 `WINUSB`）→ 显示为「通用串行总线设备」 |

模式 A（PID `0x4001`）只有一个接口，实例 ID 通常**没有** `MI_` 后缀（或只有 `MI_00`）。

**方法 3 · PowerShell 一行看全（推荐，可复制进报告）**

```powershell
# 列出所有 MeidaP4 相关节点（状态 / 类别 / 名字 / 实例 ID）
Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'USB\VID_303A*' } |
  Select-Object Status, Class, FriendlyName, InstanceId | Format-Table -AutoSize

# 每个节点绑的函数驱动服务名：模式 B 期望看到 WUDFRd(MTP) 和 WINUSB(Vendor)
Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'USB\VID_303A*' } | ForEach-Object {
  $svc = (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName 'DEVPKEY_Device_Service').Data
  '{0,-52} service={1}' -f $_.InstanceId, $svc
}
```

**预期输出（模式 B）**
```
USB\VID_303A&PID_4002\...              service=usbccgp     <- 父设备（复合）
USB\VID_303A&PID_4002&MI_00\...        service=WUDFRd      <- MTP  (wpdmtp.sys)
USB\VID_303A&PID_4002&MI_01\...        service=WINUSB      <- Vendor (winusb.sys)
```

**失败判读**
- 只有一行（没有 `MI_00`/`MI_01` 子节点）→ Windows **没有**按接口分发，说明 `bDeviceClass/SubClass/Protocol`
  不是 `0xEF/0x02/0x01`，或描述符里缺 IAD。回到 §4 检查设备类三元组。
- `MI_01` 的 service **不是** `WINUSB` → WCID 没生效，WebUSB 的 claim 会抛 `SecurityError`（见 §5）。
- `MI_00` 变成「未知设备」→ 见 §9.1，多半是 MTP 那一路的接口描述符（`bInterfaceClass=0x06`）没写对。
- 「驱动程序文件」才是最终判据：右键节点 → 属性 → 驱动程序 → 驱动程序文件，应能看到 `wpdmtp.sys` / `winusb.sys`。
  Windows 有时把服务名折叠成 UMDF 宿主，**别只看服务名**。

> ⚠️ 判据优先级：**「驱动程序文件里出现 `winusb.sys`」>「service=WINUSB」>「设备名里带 WinUSB」**。

---

## 9. 真实故障排查手册

### 9.1 设备管理器里显示「未知设备」/ 黄色感叹号 → WCID / MS OS 2.0 描述符没生效

**现象**：`MeidaP4 Probe` 或其子节点带黄色感叹号，属性→常规里写「该设备的驱动程序未被安装 (代码 28)」
或「此设备无法启动 (代码 10)」。

**判读**
- **代码 28（驱动未安装）+ Vendor 接口** → Windows 没拿到 `CompatibleID="WINUSB"`，所以不知道用哪个驱动。
  这就是 `probe-plan.md` R1 要证伪/证实的那件事：**MS OS 2.0 描述符没生效**。
- 检查清单（逐条）：
  1. `bcdUSB ≥ 0x0210`（页面上能看，见 §4）。
  2. BOS 里**有** MS OS 2.0 平台能力：UUID `{d8dd60df-4589-4cc7-9cd2-659d9e648a9f}`、`dwWindowsVersion=0x06030000`、
     `bVendorCode=0x02`（契约 §2）。
  3. 描述符集响应正确：`bmRequestType=0xC0, bRequest=0x02, wIndex=0`（**注意 `wIndex` 必须是 0**，
     非 0 时 Windows 会当成 per-interface 请求）。
  4. `Function Subset Header` 的 `bFirstInterface` 指向**正确的 vendor 接口号**（模式 A=0，模式 B=**1**），
     `bInterfaceCount=1`。**模式 B 若把 `bFirstInterface` 写成 0，兼容 ID 就发给了 MTP 接口**，
     会出现「MTP 变未知设备 + Vendor 没驱动」的双错。
  5. 字符串描述符里**不要**试图用 `0xEE`（MS OS 1.0 路线）：`esp_tinyusb` 数组上限 8，放不进去（`probe-plan.md` §4.4）。
- **代码 10（无法启动）+ Vendor 接口** → 驱动匹配上了但启动失败，多半是 WinUSB 拿到的是一个端点数不合法的接口，
  或接口描述符长度/顺序有问题。
- **兜底**：Zadig 手动装 WinUSB（见 §5）。装完能跑通 → 结论是固件 WCID 缺陷，**不是** WebUSB 方案不可行。

### 9.2 `claimInterface` 抛 `SecurityError` → 接口被内核驱动占用（没用 WinUSB）

见 §5 的「`SecurityError` 专项排查」。一句话：**Chrome 只有在接口由 WinUSB 承载时才允许 claim**；
被 `wpdmtp.sys` / `usbccgp` 自身 / 其它类驱动占着的接口，claim 一定抛 `SecurityError`。
最常见的两个具体原因：
1. 固件 WCID 没生效 → 接口没绑 WinUSB；
2. **接口选错了** —— 模式 B 下误选了 IF0（MTP）。这是"人祸"，页面已内置提醒。

### 9.3 `chrome://device-log` 怎么看

1. 地址栏输入 `chrome://device-log`。
2. 左侧/顶部过滤里**勾选 `USB` 类别**，并点「Clear」清屏。
3. 回到 probe.html 点「请求设备」/「claim」。
4. 回到 device-log 看新增行：

| 日志内容 | 含义 |
|---|---|
| `USB device added: ...` / `USB device removed` | 枚举/掉线，用来确认插拔时序 |
| `Failed to open USB device` / `Access denied` | 权限或占用问题 |
| `Failed to claim interface` + 原因 | **claim 失败的原始原因**，比页面里的 `err.message` 更底层 |
| `Interface is not available` / `already claimed` | 被内核驱动或别的进程占用 |
| `No WebUSB platform capability` 之类的字样 | **直接坐实"缺 WebUSB BOS 平台能力"**（§3 失败判读第 1 条） |

5. device-log 里的行可右键复制，**请把这些原文贴回会话**。

### 9.4 `chrome://usb-internals` 怎么看

1. 地址栏输入 `chrome://usb-internals`（Edge 同样可用，`chrome://` 是共用内核的）。
2. 上方「Devices」列表里找到 `VID:PID = 303A:4001`（模式 A）或 `303A:4002`（模式 B），点进去。
3. 能看到（**不需要页面，也不需要 claim**）：
   - **Device Descriptor**：`bcdUSB`、`idVendor`、`idProduct`、`bDeviceClass/SubClass/Protocol`、
     `iManufacturer/iProduct/iSerialNumber` → **直接核对契约 §2 的冻结值**。
   - **Configuration / Interface / Endpoint** 树：接口号、alternate、接口类、端点号/方向/类型/`wMaxPacketSize`
     → 核对契约 §1 的端点分配表。
   - 注意：**usb-internals 不显示 BOS 描述符**（Chrome 层面就没暴露）。所以 BOS 只能靠
     "设备出现在 `requestDevice` 列表里" 间接反推，或上 USB 协议分析仪/`Wireshark+USBPcap` 抓包。
4. `usb-internals` 还有 Control Transfer / Bulk Transfer 的测试页签，可以做**与页面无关**的独立复现：
   - 用 Bulk Transfer 页签直接对 Vendor 端点发一段数据看回显 → 若这里通、页面不通，问题在 claim 环节；
   - 若这里也不通，问题在固件/枚举。

> 两个诊断页 + 页面三方对照，是判断"固件问题 vs 页面问题 vs 驱动问题"的最快路径。

### 9.5 来源（origin）约束：`file://` / `http://localhost` 可以，`http://192.168.x.x` 绝对不行

| 打开方式 | `isSecureContext` | `navigator.usb` | 结论 |
|---|---|---|---|
| `file:///E:/ESP_PROJECT/MeidaP4/web/probe.html` | **`true`** | 存在 | ✅ **推荐**。双击即用，零服务器 |
| `http://localhost:8000/` | `true` | 存在 | ✅ 可以。`localhost` 被视为可信来源 |
| `http://127.0.0.1:8000/` | `true` | 存在 | ✅ 可以 |
| `http://192.168.1.50:8000/`（局域网明文） | **`false`** | **不存在（被隐藏）** | ❌ **绝对不行** |
| `https://` 任意域名 | `true` | 存在 | ⚠️ 可以但不必要；需要证书 |

原因：WebUSB 是"强特权" API，只在**安全上下文**里暴露。局域网明文 `http://` 不是安全上下文，
浏览器**根本不会**把 `navigator.usb` 挂上去——页面会显示"不可用：非安全上下文"红色横幅，这是预期行为，不是 bug。

> 常见踩坑：用手机/另一台机器访问开发者机的 `http://192.168.x.x:8000/` 来做 USB 测试 —— 那里没有本地 USB 设备，
> 而且连 API 都没有。**必须在插着板子的那台机器上本地打开。**

---

## 10. 结果记录表（执行后逐行填写，交回 Lead 回填 `docs/probe-report.md`）

| # | 步骤 | 预期 | 实测 | 判定 |
|---|---|---|---|---|
| A1 | `file://` 的 `isSecureContext` | `true` | | ☐ PASS ☐ FAIL |
| A2 | `file://` 下 `navigator.usb` | 存在 | | ☐ PASS ☐ FAIL |
| B1 | `requestDevice` 列表出现设备 | 出现 `MeidaP4 Probe` | | ☐ PASS ☐ FAIL |
| B2 | `bcdUSB` | `0x0210` | | ☐ PASS ☐ FAIL |
| C1 | 接口数（模式 A / B） | 1 / 2 | | ☐ PASS ☐ FAIL |
| C2 | 端点表（模式 A） | 1 IN + 1 OUT，bulk 512 | | ☐ PASS ☐ FAIL |
| C3 | 端点表（模式 B Vendor IF1） | EP 0x03/0x83 bulk 512 | | ☐ PASS ☐ FAIL |
| D1 | `claimInterface` 结果 + `err.name` 原文 | SUCCESS | | ☐ PASS ☐ FAIL |
| D2 | Windows 驱动文件 | `winusb.sys`（服务 `WINUSB`） | | ☐ PASS ☐ FAIL |
| E1 | 心跳 `HB <ms>` | 收到且递增 | | ☐ PASS ☐ FAIL |
| F1 | 回环 1 KB（第 1 次 / 第 2 次） | `mismatchCount==0` 且 `k−hbBytes==0` | | ☐ PASS ☐ FAIL |
| F2 | 回环 64 KB（第 1 次 / 第 2 次） | 同上 | | ☐ PASS ☐ FAIL |
| F3 | 回环 1 MB（第 1 次 / 第 2 次） | 同上 | | ☐ PASS ☐ FAIL |
| F4 | `测量前排空` | 每次 `✔ 已对齐到行边界` | | ☐ PASS ☐ FAIL |
| F5 | `strayLFlines` | 每次 `0` | | ☐ PASS ☐ FAIL |
| F6 | `字节对账不变量` | `✔ partsIn == delivered + surplus` | | ☐ PASS ☐ FAIL |
| F7 | 连续两次结果一致（无跨用例泄漏） | 两次都 PASS | | ☐ PASS ☐ FAIL |
| F8 | `zeroLenReads`（ZLP 次数） | 如实报出，通常 > 0（见 §7.4）；**不因它中止** | | ☐ PASS ☐ FAIL |
| F9 | 吞吐 / RTT | 记录数字 | | — |
| G1 | 资源管理器 `MeidaP4 Storage` | 出现、可浏览 | | ☐ PASS ☐ FAIL |
| G2 | MTP 与 WebUSB 同时可用 | 两者并行不干扰 | | ☐ PASS ☐ FAIL |
| G3 | 驱动分布 | MI_00→`WUDFRd` / MI_01→`WINUSB` | | ☐ PASS ☐ FAIL |

**回环数字（每次跑都记一行 —— 1 KB / 64 KB / 1 MB 各两次，共 6 行）**

| 数据量 | 第几次 | 分块 | 失配 | 一致字节数 | rawInBytes | hbBytes | strayLFlines | zeroLenReads | k | **k−hbBytes** | 无法解释 | 排空丢弃 | 对齐 | 总耗时 | 吞吐 (MB/s) | RTT min/avg/max | 首处失配 | 判定 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 KB | 1 | | | | | | | | | | | | | | | |
| 1 KB | 2 | | | | | | | | | | | | | | | |
| 64 KB | 1 | | | | | | | | | | | | | | | |
| 64 KB | 2 | | | | | | | | | | | | | | | |
| 1 MB | 1 | | | | | | | | | | | | | | | |
| 1 MB | 2 | | | | | | | | | | | | | | | |

---

## 11. 静态自检执行记录

| 检查 | 命令 | 结果 |
|---|---|---|
| 内联脚本语法 | 抽出 `<script>` 内容（81533 字符，全文仅 1 个 `<script>`）→ `node --check` | ✅ `exit 0` |
| DOM id 交叉核对 | Node 脚本比对 HTML `id=` 与 JS 中 `$()` / `getElementById()` / `setText()` / `setHtml()` / `show()` / `hide()` / `on()` / `dis()` 的引用 | ✅ HTML 定义 **99** 个 id，**0 个悬空引用**（`sec-*` 与 `untested-note` 为静态区块，不需 JS 引用） |
| **出厂源码回归测试**（无硬件） | 从 `web/probe.html` 的**出厂源码文本**抽取 `hex`/`nowMs`/`withTimeout`/`trackTransfer`/`createHbFilter`/`echoChunkPipelined`，在 Node 里跑 11 组共 **43** 条断言 | ✅ **43 passed / 0 failed** |
| ↳ A. HB 过滤器（7 组 27 条） | 干净剥离 / 泄漏回归 / 行跨边界 / strayLF / reset / 字节账 / 40 组模糊测试 | ✅ 全通过 |
| ↳ 泄漏回归 | 半行中断后用/不用 `reset()` 各推一次完整心跳行 | ✅ 不复位 → 泄漏前导 `0x0A`（错位 1 B）、`strayLF=1`；`reset()` 后 → 无泄漏、`strayLF=0` |
| ↳ 行跨边界 | 心跳行被两个 `transferIn` 拆开 | ✅ 正确重组，`hbBytes=7`，尾部数据 `AA` 正确吐出 |
| ↳ 字节账 | `hbBytes + data == rawIn` | ✅ 成立（这条是 `k − hbBytes == 0` 的基础） |
| ↳ **模糊测试** | 40 组随机：心跳行插在 LF-free payload **任意位置** + 流按 1~512 B **任意包边界**切分 | ✅ 40/40：payload 逐字节还原、`hbBytes` 精确、字节账平、`strayLF==0`（**证明心跳交错不会造成位移或损坏**） |
| ↳ 识别性验证 | 故意不 `reset()` 时能复现泄漏 | ✅ 说明该测试确实能抓到 host-usb 那个 bug，不是假通过 |
| ↳ **B1 ZLP 复现+修复** | 用 mock 设备复现用户那次中止点：块0(4096B) → **ZLP** → 块1(4096B)，驱动**出厂源码**的 `echoChunkPipelined` | ✅ 不抛错；两块逐字节正确；`zeroLenReads=1`；`rawInBytes=8192`（**ZLP 不计入**）；不变量未破坏 |
| ↳ B2 ZLP 多处 + 心跳混入 | ZLP + 心跳 + 两块数据 | ✅ 数据全对；`zeroLenReads=3`；`hbBytes=8`；`rawInBytes=8192+8`；**`k − hbBytes == 0`** |
| ↳ B3 防自旋 | 只会发 ZLP 的设备（上限设为 3） | ✅ 抛 `ProbeZeroLenSpinError`，未无限自旋 |
| ↳ B4 总超时兜底 | 无数据设备 + 已到期的 deadline | ✅ 抛 `ProbeTimeoutError` 且带 `isProbeTimeout`（触发自动 reset 路径） |
| 外部依赖 | grep `src=` / `href=` / `@import` / `<link` | ✅ **`src=`/`href=` 属性 0 处**；`<link>` 0 个、`@import` 0 个 |
| 外部 URL | grep `https?://` | ✅ 全部是提示性纯文本（`file://`、`http://localhost`、`http://192.168.x.x` 的说明，以及 GET_URL 的 scheme 解释），无一处是资源引用 |
| 框架痕迹 | grep react/vue/jquery/angular/bootstrap/tailwind | ✅ 0 命中 |
| 文件数 | `web/` 目录 | ✅ 只有 `probe.html`（单文件） |
| 反死锁顺序 | 静态检查 `echoChunkPipelined` 内 `readP` 与 `outP` 的声明顺序 | ✅ `readP` 在前，确认"先挂 IN 读、再发 OUT" |
| 分块上限 | grep `MAX_CHUNK` | ✅ `MAX_CHUNK = 4096`，且 `selectedChunk()` 会强制降档并告警 |
| task-10 修复在位 | `filter.reset()` / `drainIn` / `ctx.surplus` / `invariantViolations` / `zeroLenReads` / `completed` | ✅ `filter.reset()` 在 `runLoopback` 内调用 2 次；`drainIn` 在位；surplus 结转 + 不变量断言在位；**`ProbeEmptyReadError` 已彻底移除**；中止时派生量走 `N/A` 分支 |
| 临时文件清理 | `E:\ESP_PROJECT\dsh-scratch\` | ✅ 用完已删，目录为空，未污染工作区 |

> 回环的**真机**部分仍未实测（见 §12）。


---

## 12. 明确「未实测」清单（不要当成已验证结论）

1. **页面从未由本人（web-host）在真机上端到端跑过。** 编写时本机未插板（`probe-plan.md` §5：无 CP210x/CH34x/USB-JTAG 设备）。
   所有预期现象都是**基于契约与 USB/WebUSB 规范的推断**，不是本人的实测。
   *（更新）* 回环侧已做了**无硬件回归测试**：从出厂源码文本抽取函数，**43 条断言全通过**，
   并用 host-usb 抓到的 hexdump 现场**复现了泄漏**、用 mock 设备**复现了 ZLP 中止点**，再验证修复。
   但"1 KB/64 KB/1 MB 各两次全 PASS"这一条**仍需板子**。
2. `claimInterface` 在 Windows + WinUSB 下是否真的成功 —— **这才是 R1 的核心**。*（用户已实测过一次成功路径）*
3. `file://` 下 `isSecureContext === true` 已由 Chromium 行为确定；步骤 A 用来留下本机实测证据。
4. 回环吞吐与 RTT 数字 —— 参考值见 host-usb 的 WinUSB 工具（1 MB ≈ 51 Mbit/s）；页面侧的吞吐**不作为验收依据**。
5. 心跳是否真的"空闲 1 s 后"发送、格式是否严格为 `\nHB <ms>\n` —— 依赖 `usb_probe` 实现。
   *（更新）* 这一条会直接影响排空策略：排空假定"空闲时会周期性收到心跳"。
   若新固件改成不发心跳，排空会走 `emptyFifoNoHb` 分支（首个读超时且零积压 → 视为已对齐，并把那个超时读收养），
   该分支**未在真机验证过**。
6. BOS / WebUSB 平台能力的**原文**无法从 JS 读取（WebUSB 不暴露 BOS）。页面只能通过"设备出现在
   `requestDevice` 列表"间接反推；原文需 `chrome://usb-internals` 之外的手段（USB 抓包）确认。
   → 契约 §6 / `probe-plan.md` §3 冒烟 A 第 ⑤ 条写的是"页面能读出 BOS/WebUSB 平台能力"，
   **这一条用纯 WebUSB JS 做不到**，建议改为"设备出现在 requestDevice 列表 + usb-internals 对照"。
   已作为契约不一致项报给 Lead。
7. 模式 B 的 MTP（资源管理器出现 `MeidaP4 Storage`）—— 未实测。
8. WebUSB **没有** `cancelTransfer`，所以"取消挂起传输"只能靠 `device.reset()`（会让设备重新枚举）。
   这一点已按此实现，但 reset 在真实 Windows 栈上的副作用（是否触发重新枚举、是否保留授权）**未实测**。
9. *（task-10 新增）* 「排空丢弃字节数」与 `k − hbBytes == 0` 的对应关系**未在真机验证**：
   host-usb 的 `1 KB : rawInBytes=1033, k=+9, hbBytes=9` 是在**没有排空**的对账工具里测到的；
   本页**先排空**再测量，因此 1 KB 用例的 `rawInBytes` 可能恰好 `= 1024`（`k=0`）或略大（夹到一个活心跳）。
   两种都算 PASS（只要 `k − hbBytes == 0`）。
10. *（task-10 新增）* 取消勾选「测量前排空」应能**复现**错位（`strayLFlines > 0`、首处失配在偏移 0）——
   这条因果验证**未在真机做**，但它不需要板子也能由 HB 过滤器回归测试间接支持（见 §11）。
11. *（ZLP 修复后新增）* **ZLP 的存在已在真机被观察到**（用户那次 64 KB 中止的日志 + host-usb 在同板同固件上
   1 KB/64 KB/1 MB 全 PASS），但：
   - 「每 4096 B 块恰好触发一次 ZLP」这个**频率预期未在真机确认**（只在 mock 里验证过处理逻辑）；
   - 修复后 64 KB / 1 MB 能否各自连续两次跑通 —— **未实测**；
   - WebUSB 对**同一端点并发 `transferIn`** 的确切行为（是否 `InvalidStateError`、还是排队）**未实测**：
     本页设计上保证串行读 + 不留悬空读，因此不依赖该行为，但真机若出现该错误即可确认此假设。
12. *（待办，不是本页问题）* **协议文档必须补 ZLP 语义**：正式 I0 协议要写明
    "ZLP 是合法的零长度包，主机必须忽略并继续读，不得视为错误或流结束"。
    本页与 host-usb 的工具都在这里踩过同一个坑（见 §7.4）。
