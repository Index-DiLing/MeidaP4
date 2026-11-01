# 主机侧 WinUSB 枚举 / claim / 回环验证工具

> 交付物：`tools/winusb_loopback.py`（纯 Python 3 + 标准库 `ctypes` 直调 SetupAPI / WinUSB）。
> 本文档记录**用法**、**输出怎么读**、**失败怎么判读**，并**严格区分**
> 「已在本机实测通过」与「仅代码完成、未实测」。
> 生产者：host-usb。关联任务：`task-6`。

---

## 0. 一句话状态（先读这段）

### 0.1 现状总表

| 项目 | 状态 |
|---|---|
| 枚举 / 诊断（`--list`、`--diagnose`） | **已在真机实测通过** |
| **真机 bulk 回环 1 KB / 64 KB / 1 MB** | **✅ 已实测通过（逐字节 0 失配，多轮复跑全绿）** —— 见 §0.2 |
| 串口侧 `out=` / `in=` 增量对账 | **✅ 已闭环，逐包精确** —— 见 §0.2 |
| pipe 清单 / 协商速率 | **✅ 已取到**：IN `0x83` / OUT `0x03`，Bulk 512，`HighSpeed` |
| GUID 语义查证（哪个 GUID 是谁注册的） | **已在真机实测通过**（§3 有完整证据） |
| 离线自测（心跳剥离状态机、payload、结构体尺寸、参数解析） | **已实测通过，34/34** |
| `WinUsb_ClaimInterface` | **该 API 在本机 winusb.dll 中不存在**，见 §6.2 |
| 心跳 `\nHB <ms>\n` 的真机解析、设备 uptime | **✅ 已实测**；并发现「心跳积压」这一关键副作用，见 §0.2 |
| 协商速率（`WinUsb_QueryDeviceInformation(DEVICE_SPEED)`） | **✅ `HighSpeed`（3）** |

### 0.2 修复后真机复测（**已闭环**）

固件在 MS OS 2.0 描述符集里加了 Registry Property 之后，MI_01 立刻注册出接口，
**原先被阻塞的整条回环链路一次打通**：

```
--diagnose  [5] [2] USB\VID_303A&PID_4002&MI_01\8&39523820&0&0001
                declares        : DeviceInterfaceGUID (legacy) = {A5DCBF10-6530-11D2-901F-00C04FB951ED}
                registered GUIDs: {A5DCBF10-...}, {DEE824EF-729B-4A0E-9C14-B7117D33A817}
                -> registers 2 interface(s)
--diagnose  verdict: PASS  (退出码 0)

--pid 0x4002 --mi 01
  interface: bInterfaceNumber=1  bNumEndpoints=2  class=0xFF/0x00/0x00  iInterface=4
  pipes (2):
    idx  PipeId  dir  type   wMaxPacketSize  Interval
    0    0x03    OUT  Bulk   512             0
    1    0x83    IN   Bulk   512             0
  negotiated speed : HighSpeed

  case      chunk  bytes      Mbit/s   RTT min/avg/max (ms)   mismatch  verdict
  1.000 KiB 4096   1.000 KiB   6.10    0.543/0.543/0.543     0         PASS
  64.000 KiB 4096  64.000 KiB 49.94    0.559/0.610/0.769     0         PASS
  1.000 MiB 4096   1.000 MiB  51.75    0.556/0.627/0.972     0         PASS
  VERDICT: PASS: 3/3 cases byte-exact, 1.063 MiB echoed at 51.29 Mbit/s
```

**串口对账（受控夹取，见 §0.3）—— 逐包精确：**

```
I (7023)  stats | mounted=1 out=0B/0pkt         in=0B/0pkt         ctrl=1 stall=0   <- 基线
I (12023) stats | mounted=1 out=66560B/130pkt   in=66560B/130pkt                     <- 1k+64k 之后
I (17023) stats | mounted=1 out=1115136B/2178pkt in=1115136B/2178pkt                 <- +1m 之后
```

| 判定 | 结论 | 依据 |
|---|---|---|
| **A**：`transferOut` 的数据到设备了吗？ | **到了** | `out` 增量 = `1115136 B`，恰好 `1024+65536+1048576`；`2178 pkt`，`2178×512 = 1115136` 逐包精确 |
| **B**：设备把数据回出来了吗？ | **全部回了** | `in` 增量 = `out` 增量 = `1115136 B / 2178 pkt`；主机侧逐字节 0 失配（含 1 MiB） |
| **C**：测试期间复位/崩溃了吗？ | **没有** | 唯一 `rst:0x1 (POWERON)` 在抓取自身开串口时（DTR/RTS）；之后 uptime 单调 7023→57023 ms、计数器单调递增；无 `Guru Meditation` / `Backtrace` / `echo 缓冲满` |

> `echo_errors` **无法从串口读**：固件打印的 `stats` 行只有 `mounted/out/in/ctrl/stall`，没有 `echo_errors` 字段；
> 溢出时才会打 `echo 缓冲满，丢弃 N B（累计 echo_errors=…）` 警告 —— 本次全程未出现，故 **`echo_errors` 无增量（视作 0）**。
> 若需要它的**数值**，得让固件把它加进 `stats` 行。

**⚠️ 本次复测新发现的操作性副作用：心跳会积压。**
固件在**已挂载且空闲时每秒无条件发一次** `\nHB <ms>\n`，而**没人读 IN 时这些行会堆在设备侧 FIFO 里**。
实测：一次 `--diagnose` 之后的首次运行里，工具一口气排空并剥离了 **374 行**心跳（uptime 1.9 s→374.9 s，约 4 KB）。
这对本工具无影响（LF-free 载荷 + 逐行剥离 ⇒ 偏移恒为 0），**但对任何"先对齐再比对"的主机实现是致命的**：
积压的心跳字节如果被当成数据、或流的起点不在行边界上，就会造成**整体位移 k**，
而位移 k 下的逐字节比对恰好会呈现「约 1/256 命中」的指纹（见 §0.4）。

**`k = rawInBytes − size` 的正确读法（重要）**：
`k` **包含合法的被剥离心跳字节**，因此 `k > 0` 本身**不证明**错位。
必须用 **`k − hbBytes`**：

- 本工具：`k = hbBytes`（或 `k = 0`），`strayLFlines = 0` ⇒ **对齐干净**。
  实测三例：`rawIn = 1033 (size 1024, k=+9, hbBytes=9)` / `rawIn = 65536 (k=0)` / `rawIn = 1048576 (k=0)`。
- 只有 **`k > hbBytes`** 才说明有「非心跳的额外字节」混进来 —— 那才是错位。
  （**反例**：§6.9 那次 `k=+11` 而 `hbBytes=0`、`strayLFlines=2` —— 典型的起点错位。）
- **稳定性**：修掉 §6.9 的状态泄漏后，**连续 8 次整套运行 8/8 PASS、9 个用例失配全 0**，
  吞吐 51.8–54.1 Mbit/s。

### 0.3 串口对账的正确做法（本机踩过的坑）

`tools/serial_capture.py` **打开 COM16 时 CH343 驱动会抬 DTR/RTS，必定复位芯片**，
所以「抓基线 → 跑工具 → 再抓一次」**在本机永远读不到增量**（两次抓取各自把计数器清零）。
必须让**一次抓取把测试夹在中间**：

```powershell
$cap = Start-Process $py -ArgumentList @('-B','tools\serial_capture.py','--port','COM16',
       '--baud','115200','--seconds','60','--no-reset','--out',$log) -PassThru -NoNewWindow
Start-Sleep -Seconds 8            # 让它的复位把计数器清零，并打出第一条 stats = 基线
& $py -B tools\winusb_loopback.py --pid 0x4002 --mi 01 --size 1k
& $py -B tools\winusb_loopback.py --pid 0x4002 --mi 01 --size 64k
& $py -B tools\winusb_loopback.py --pid 0x4002 --mi 01 --size 1m
Wait-Process -Id $cap.Id
Select-String -Path $log -Pattern 'stats \|'
```

此后 `stats` 行按时间顺序给出「基线 → 1k/64k 后 → 1m 后」，**逐条做差即增量**。
`serial_capture.py` 检测到横幅会打印 WARNING（说明计数器已被它自己的复位清零）——那是**预期**行为。

### 0.4 浏览器侧失败（4098 字节）的定性结论

用户页面 1 MiB 的结果是「一致 4098 B / 失配 1044478」，`4098 ≈ 1048576/256`。
这正是**整体位移 k** 的统计指纹：位移之后逐字节比对会随机命中约 `1/256`。
而本工具的受控复测在同一固件、同一条链路上 **`k − hbBytes = 0`、0 失配**，
且串口 `out`/`in` 逐包精确 ⇒ **固件回环与 USB 链路本身是正确的**，
问题在**主机侧（页面）的流对齐**。

页面里两处可复现的隐患（`web/probe.html`，交由 web-host 复核）：

1. **回环开始前没有"排空/同步"步骤**。设备侧积压的心跳行会最先到达；
   只有当 IN 流的起点恰好落在行边界上时过滤器才能干净剥离。起点若落在行中间，
   前导残片会被当作**数据**输出，形成整体位移。
   （`.probe.html:1266` 每次运行新建 `createHbFilter()`，但没有任何 drain/prime。）
2. **`appendParts()` 会把超出当前块所需的数据静默丢弃**（`.probe.html:1192-1201`：
   `if (accLen >= chunk.length) break;` / `take = Math.min(...)`），
   而 `echoChunkPipelined` 只在 `accLen < chunk.length` 时继续读，
   没有任何"surplus 结转"路径 —— 一旦某个 `push()` 返回的数据多于本块所需，
   **多出来的字节就永久消失**，同样表现为整体位移。
   （当前 LF-free 载荷下 `carry` 只含心跳字节，所以这条难以触发；但它是一个潜伏 bug，
   且是页面里**唯一**能让字节凭空消失的地方。）

### 0.5 历史记录：修复前的阻塞判据（已解除，保留作为回归判据）

根因是**设备侧在 Windows 上注册不出任何 device interface**，因此**根本不存在可 `CreateFile` 的 device path**，
自然也就到不了 `WinUsb_Initialize` / pipe / 回环。判据有三条，任一条都能独立复现：

| # | 命令 | 判据（复现现象） |
|---|---|---|
| J1 | `--pid 0x4002 --mi 01 --size 1k` | `No candidate matched vid=0x303a pid=0x4002 mi=01` —— **枚举阶段就选不到设备**，代码根本没走到 `CreateFile` |
| J2 | `--diagnose` 的 `[5]` 节 | `USB\VID_303A&PID_4002&MI_01\...` 的 `registered GUIDs: (none)` / `-> registers NOTHING` |
| J3 | `--diagnose` 的 `[2]` 节 | MI_01 的实例哈希 `8&2b768ee9&0&0001` → `0 interface GUIDs`（对比：父节点 `probe-0001` → `{a5dcbf10-…}` ×1；MI_00 → 4 条） |

因果链（每一环都有实测支撑）：

```
MI_01 的 Device Parameters 里没有 DeviceInterfaceGUIDs        (§5 对照实验)
        -> winusb.sys 不注册任何 device interface
        -> SetupDiEnumDeviceInterfaces 对任何 GUID 都枚举不到 MI_01（计数 = 0）
        -> 没有 \\?\... 符号链接 -> CreateFile 不可能成功
        -> 无 WinUsb_Initialize -> 无 pipe -> 无回环
```

**修复生效的正向判据**（同一个 `--diagnose`，无需改工具）：
`[5]` 中 MI_01 的 `declares` 由 `*** NONE ***` 变为 `DeviceInterfaceGUIDs = {A5DCBF10-…}`，
且 `registered GUIDs` 非空；随后 `--pid 0x4002 --mi 01 --size 1k` 应打印出 pipe 清单并跑出 `PASS`。
这条把「不可验证」变成「可验证」，且**不需要重新解读任何日志**。

---

## 1. 用法

```powershell
$py = 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe'   # ESP-IDF v6.1 python 3.10.9

# 只读诊断（推荐先跑这个，不碰设备）
& $py tools\winusb_loopback.py --diagnose

# 只枚举 + 打印候选清单，不打开设备
& $py tools\winusb_loopback.py --list

# 模式B：只枚举 + 打开 + pipe 清单，不跑回环
& $py tools\winusb_loopback.py --pid 0x4002 --mi 01 --no-loopback

# 完整回环（1 KB / 64 KB / 1 MB）
& $py tools\winusb_loopback.py --pid 0x4002 --mi 01

# 自定义规模与分块
& $py tools\winusb_loopback.py --pid 0x4002 --mi 01 --size 4k,256k --chunk 2048

# 只等心跳
& $py tools\winusb_loopback.py --pid 0x4002 --mi 01 --hb-only

# 落一份纯文本报告
& $py tools\winusb_loopback.py --pid 0x4002 --mi 01 --report host-usb-report.txt
```

> 不装 `pyusb`/`libusb`，也不需要 Zadig —— 见 §2。全部只读操作（`--diagnose`/`--list`）不写设备、不 claim pipe。

### 1.1 参数

| 参数 | 默认 | 含义 |
|---|---|---|
| `--vid` | `0x303A` | USB vendor id 过滤。接受 `0x303A` / `303A`（含 a-f 按十六进制解析） |
| `--pid` | 任意 | product id 过滤。**模式A `0x4001` / 模式B `0x4002`**。裸写 `4002` 会被当作**十进制**，请写 `0x4002` |
| `--mi` | 任意 | 接口号过滤。**模式B = 1（vendor）/ 模式A = 0**。`01`、`1`、`0x01` 均可 |
| `--size` | `1k,64k,1m` | 回环用例字节数，可重复或逗号分隔。支持 `1k/64k/1m` 与裸字节 |
| `--chunk` | `4096` | 分块字节数，**强制上限 4096**（设备侧缓冲约 12 KB，见 §6.4）。超出会被夹住并告警 |
| `--timeout` | `3000` | 单次管道操作超时（ms），写入 `PIPE_TRANSFER_TIMEOUT` |
| `--case-timeout` | `60` | 单个用例总超时（秒），防死锁 |
| `--pipeline` | `1` | 同时在外挂起的 OUT 写块数。`>1` 提高吞吐但会模糊 RTT |
| `--io-mode` | `overlapped` | `overlapped`（`FILE_FLAG_OVERLAPPED` + `OVERLAPPED`）或 `sync`（同步句柄 + 读线程） |
| `--no-loopback` | 关 | 只枚举 + 打开 + pipe 清单 |
| `--list` | 关 | 只列候选，不打开。**恒定退出 0**（"没匹配到"是结论而非工具错误） |
| `--diagnose` | 关 | 只读诊断，见 §4 |
| `--hb-wait` | `4.0` | 等待空闲心跳的秒数 |
| `--hb-only` | 关 | 只跑心跳探测 |
| `--quiet` | `0.4` | 每个用例前等待 IN 流静默的秒数 |
| `--open-retries` | `2` | `CreateFile` 失败时的尝试次数（每次间隔 10 s）。`ACCESS_DENIED` 不重试 |
| `--guid` | — | 额外/覆盖要探测的 device interface GUID，可重复 |
| `--device-path` | — | 跳过枚举，直接用给定 device path |
| `--report FILE` | — | 额外把纯文本报告写文件 |

### 1.2 退出码

| 码 | 含义 |
|---|---|
| `0` | 全部通过；`--list` 恒为 0；`--no-loopback` / `--hb-only` 按预期完成 |
| `1` | 回环失配 / 超时 / 传输不完整 |
| `2` | 工具、枚举、打开或 WinUSB 错误（**当前本机 `--diagnose` 与一切打开操作都是 2**，见 §0.1）；参数用法错误（argparse）也是 2 |

实测退出码矩阵（本机）：

```
--help                                   exit=0
--list                                   exit=0
--diagnose                               exit=2      <- 阻塞，§0.1
--no-loopback                            exit=2      <- 选中父节点后 WinUsb_Initialize 失败
--pid 0x4002 --mi 01 --size 1k           exit=2      <- J1：枚举阶段无候选
--size notasize                          exit=2      <- argparse 用法错误
```

---

## 2. 前置条件，以及"WinUSB 已绑定，无需 Zadig"的准确含义

Lead 在真机上确认的是：`USB\VID_303A&PID_4002&MI_01` 的 `Service = WINUSB`、`InfPath = winusb.inf`、
`CompatibleID` 含 `USB\MS_COMP_WINUSB`、友好名 `MeidaP4 WebUSB`。**这部分完全正确，确实不需要 Zadig 换驱动。**

但"驱动绑好了" **≠** "应用能打开它"。Windows 上要打开一个 WinUSB 接口，应用需要一个
**device interface path**（`\\?\usb#vid_…&pid_…&mi_01#…#{GUID}` 形式的符号链接）。
这个符号链接由 **`winusb.sys` 在设备键里存在 `DeviceInterfaceGUIDs` 时才注册**（§5 对照实验）。
WCID 走 inbox `winusb.inf` 绑定的设备**不会**被写入这个值（§3.2），所以：

> **驱动绑定 OK + 没有 device interface path = 应用层完全看不见这个接口。**

本机 MI_01 正好落在这个组合上。这不是工具缺陷，也不是驱动装错，而是**「复合设备（usbccgp 子接口）+ 纯 WCID」缺少接口注册**这一设计组合的必然后果。

---

## 3. GUID 语义（本机权威证据）

### 3.1 结论表

| GUID | 真名 | 谁注册它 | 本机实测 |
|---|---|---|---|
| `{DEE824EF-729B-4A0E-9C14-B7117D33A817}` | **`GUID_DEVINTERFACE_WINUSB`** | `winusb.sys`（硬编码），且**仅当设备键有 `DeviceInterfaceGUID(s)`** | 硬编码位置 `winusb.sys` offset `0x146F0`（全机 `.sys` 唯一命中）；枚举到 1 台 WINUSB/USBDevice 设备 |
| `{A5DCBF10-6530-11D2-901F-00C04FB951ED}` | **`GUID_DEVINTERFACE_USB_DEVICE`** | USB **hub** 驱动，注册给 USB **设备节点**；usbccgp 子接口**从不**注册它 | 硬编码于 hub 驱动；**Chrome/Edge 二进制里就是它**；本机 38 条记录中 `&MI_` 命中 **0** |
| `{F72FE0D4-CBCB-407D-8814-9ED673D0DD6B}` | winusb.inf 里 **ADB 段**声明的 GUID，**不是**通用 WinUSB 值 | 仅 `winusb.inf` 的 `[ADB.HW.AddReg]` | 任何 `.sys` 里 **0 命中**；SetupDi 枚举 **0 个接口** |
| `{E5B3B5AC-9725-4F78-963F-03DFB1D828C7}` | 与本问题无关的残留空壳 | `MI_00`/`MI_01` 的 `Device Parameters` 各有一条空子键 | 任何 `.sys` **0 命中**；SetupDi 枚举 **0 个接口** |

> ⚠️ **两个常见误传**，本项目都踩过，记下来：
> 1. `{A5DCBF10-…}` **不是** WinUSB 的接口 GUID，它是 USB **设备**接口 GUID。
> 2. `{F72FE0D4-…}` **不是**通用 WinUSB GUID，它是 `winusb.inf` 里 **ADB** 段的值。

### 3.2 `winusb.inf`：`[WINUSB.NT.HW]` 是**空段**

```
C:\Windows\INF\winusb.inf:52  [WINUSB.NT.HW]
C:\Windows\INF\winusb.inf:53  ; Keep empty section placeholder for Include/Needs
...
C:\Windows\INF\winusb.inf:96  HKR,,DeviceInterfaceGUIDs,0x10000,"{F72FE0D4-CBCB-407d-8814-9ED673D0DD6B}"
```

`[Generic.Section.NTamd64]` 把 `USB\MS_COMP_WINUSB` 映射到 `WINUSB` 段，而 `WINUSB.NT.HW` 是空段
（第 96 行那条属于 **`[ADB.HW.AddReg]`**，是 ADB 设备专用的）。所以**走 WCID 自动绑定的设备不会得到 device interface**。

### 3.3 Chrome / Edge 要的是 `{A5DCBF10-…}`（字节模式扫描）

16 字节 GUID 在内存中的布局是 `Data1(LE) Data2(LE) Data3(LE) Data4`，据此直接扫二进制：

```
{ A5DCBF10-... }  ->  C:\Program Files\Google\Chrome\Application\155.0.8059.80\chrome.dll        (x2)
                      C:\Program Files (x86)\Microsoft\Edge\Application\155.0.4283.45\msedge.dll (x2)
{ DEE824EF-... }  ->  NOT FOUND in any scanned browser binary
{ F72FE0D4-... }  ->  NOT FOUND in any scanned browser binary
```

（扫了 22 个 >5 MB 的浏览器二进制，含 `chrome.dll` 305 MB / `msedge.dll` 353 MB。）
→ **Chrome 155 / Edge 155 的 Windows WebUSB 后端枚举 `{A5DCBF10-…}`**，这是"找不到兼容设备"的机制性原因。

### 3.4 hub 驱动硬编码 `{A5DCBF10-…}` 的位置

```
C:\WINDOWS\System32\drivers\USBHUB3.SYS   offset=0x62EE0
C:\WINDOWS\System32\drivers\UsbHub3.sys   offset=0x62EE0
C:\WINDOWS\System32\drivers\usbhub.sys    offset=0x67D50
C:\WINDOWS\System32\drivers\tsusbhub.sys  offset=0x1B4D0
C:\WINDOWS\System32\drivers\OrayUSBVHCI.sys / OrayVGC.sys   (第三方虚拟 USB，旁证)
```

因为是**设备节点**级注册，复合设备（usbccgp）的子接口拿不到它 —— 这正是浏览器看不见 mode B vendor 接口的原因。

### 3.5 行业做法：`DeviceInterfaceGUIDs` 就是唯一机制

`C:\Windows\INF` 全量 grep（`DeviceInterfaceGUID`）：

```
oem6.inf:83,86  / oem60.inf:93,96 / oem53.inf:52 / oem93.inf:66   HKR,,DeviceInterfaceGUIDs,0x10000,%STLink_GUID% / %STBridge_GUID%
oem69.inf:47    HKR,,DeviceInterfaceGUIDs,0x10000,"{A50B306A-33F2-4694-B7C3-0FF565215732}"
oem88.inf:57    HKR,,DeviceInterfaceGUIDs,0x10000,"{CDB3B5AD-293B-4663-AA36-1AAE46463776}"
transfercable.inf:55   HKR,,DeviceInterfaceGUID,,"{1EB7CB03-60A9-4d28-934F-4ABEEEBD0019}"   ← 旧式单数写法
oem73.inf:8,58-65      DeviceGUID = "{07337CAB-8372-41F6-B1E8-6D4989D7D73A}"；[USB_Install.HW] AddReg=AddDeviceInterfaceGUID；
                       [AddDeviceInterfaceGUID] HKR,,DeviceInterfaceGUIDs,0x10000,%DeviceGUID%
oem73.inf:61-62        [NoDeviceInterfaceGUID]  ; Avoids adding a DeviceInterfaceGUID for generic driver   ← Zadig/libwdi 的 INF
```

`oem73.inf` 就是 Zadig/libwdi 生成的 WinUSB INF：**它做的唯一一件"关键"事就是写 `DeviceInterfaceGUIDs`。**
这解释了为什么 Zadig 能"修好"这类设备 —— 修的不是驱动，是**接口注册**。

### 3.6 STLink 的 1:1 旁证

`oem6.inf:134` 声明 `STLink_GUID="{DBCE1CD9-A320-4b51-A365-A0C3F3C5FB29}"`，而本机
`{dbce1cd9-…}` 有 **4** 条记录、`{dee824ef-…}` 也正好有对应的 **4** 条 STM32 设备
→ winusb.sys 在**设备声明了 `DeviceInterfaceGUID(s)` 的前提下**会同时注册「声明的 GUID」**和**「硬编码的 `{DEE824EF-…}`」。

### 3.7 给固件的精确建议（MS OS 2.0 Registry Property）

> 这一条已转给 usb-core 作为 `task-8` 实验 B 的依据；此处只作工具视角的记录。

在 MS OS 2.0 描述符集中增加 **Registry Property feature**：

| 字段 | 值 |
|---|---|
| `wDescriptorType` | `4` |
| `wPropertyDataType` | `0x0007`（**`REG_MULTI_SZ`**；注意 **不是** `0x04`，`0x04` 是 `REG_DWORD`） |
| `wPropertyNameLength` / `PropertyName` | `"DeviceInterfaceGUIDs"` UTF-16LE + NUL |
| `wPropertyDataLength` / `PropertyData` | UTF-16LE，每串以 NUL 结尾，**最后再补一个 NUL（双 NUL 收尾）** |
| 值 | `{A5DCBF10-6530-11D2-901F-00C04FB951ED}`（**必须有**，Chrome 唯一认的）；可选再加 `{DEE824EF-729B-4A0E-9C14-B7117D33A817}` |

这是 **function-level** 描述符集：请求打到 vendor 接口（模式B `wIndex=1`）时必须返回那一份。

---

## 4. `--diagnose` 怎么读

`--diagnose` 完全只读（只做 SetupAPI 枚举、PnP 属性查询、DeviceClasses 注册表扫描、
以及 `CreateFile`+`WinUsb_Initialize`+立即 `WinUsb_Free` 的可打开性探测；不写设备、不做任何管道 IO）。

| 节 | 回答的问题 |
|---|---|
| `[1]` | 每个候选 GUID 各自枚举到哪些接口、它们的 `service`/`class`/device path/instance |
| `[2]` | 本设备各 devnode（父 + 各 MI_）的**实例哈希**在全机 `DeviceClasses` 里命中哪些接口 GUID、各几条 |
| `[3]` | 每个 devnode 的 `Device Parameters` 下**有没有 `DeviceInterfaceGUIDs`**，以及它实际注册出了哪些 path |
| `[4]` | 对**每一条**已注册 path 直接尝试 `CreateFile` + `WinUsb_Initialize`，打印 `GetLastError`（含 sync 与 overlapped 两种） |
| `[5]` | **门控证明**：全机所有 `service==WINUSB` 的设备，逐条对照「是否声明 GUID」与「实际注册出的 GUID」 |
| verdict | 结论 + 根因 + 三条修法（含可直接复制的 `reg add` 命令） |

**读法要点**：
- `[1]` 里**没有**我们的设备 = 很正常（没注册就没 path），不代表设备不在。
- `[2]` 里 MI_01 是 `0 interface GUIDs` 而父节点/MI_00 有命中 = **子接口注册缺失**，这就是病征。
- `[4]` 里同一路径 `sync` 与 `overlapped` 结果不同（`err=6` vs `err=8`）是 WinUSB 的正常行为，不是 bug。
- `[5]` 的相关系数表一眼就能判断修复是否生效。

---

## 5. 「我们 vs 华硕」对照表（修复判据，`--diagnose` `[5]` 节原样输出）

本机所有 present 且 `service == WINUSB` 的设备只有 2 个，且**同为 usbccgp 子接口、同为 `USBDevice` 类、同为 `winusb.inf`**，唯一差别就是那个值：

```
    [1] USB\VID_303A&PID_4002&MI_01\8&2B768EE9&0&0001
        declares        : *** NONE ***
        registered GUIDs: (none)
        -> registers NOTHING

    [2] USB\VID_0B05&PID_19AF&MI_00\9&DE2EAF5&0&0000
        declares        : DeviceInterfaceGUID (legacy) = {6987B675-8CBC-4BD3-A557-C627CEA223DC}
        registered GUIDs: {6987B675-8CBC-4BD3-A557-C627CEA223DC}, {DEE824EF-729B-4A0E-9C14-B7117D33A817}
        -> registers 2 interface(s)

    correlation over 2 WINUSB devnode(s):
      declares GUID  + registers >=1 : 1
      declares GUID  + registers 0  : 0  (expected 0)
      declares NONE  + registers >=1 : 0  (expected 0)
      declares NONE  + registers 0  : 1
    => registration is GATED on a DeviceInterfaceGUID(s) value.
```

**这张表就是修复判据**：修好之后，第 `[1]` 行的 `declares` 应变成
`DeviceInterfaceGUIDs = {A5DCBF10-…}`，`registered GUIDs` 非空，`-> registers 1 interface(s)`。
（相关系数表的结论只在**两组都非空**时才断言，避免"只有一行"时给出过强结论。）

---

## 6. 失败怎么判读

### 6.1 `--list` / 不回环也无法选中设备（`No candidate matched`）
→ 先跑 `--diagnose`。若 `[2]` 显示目标 MI 的接口命中数为 0，就是**接口未注册**（本机现状）。
不要怀疑 VID/PID/MI 参数；`--diagnose` 会同时打印实际探测的 GUID 列表。

### 6.2 `CreateFile` 报 `ERROR_ACCESS_DENIED (5)` → 别的进程占着
WinUSB 的 claim 是**独占**的。常见占用者：浏览器标签页里已经 `claimInterface` 的页面、
另一个本工具实例、Zadig 的调试器。
**本工具不重试 `ACCESS_DENIED`**（重试无意义），直接报错退出 2。
> **重要更正**：`WinUsb_ClaimInterface` **在本机 `winusb.dll` 中不存在**。
> 导出表实测只有 `WinUsb_Initialize / QueryInterfaceSettings / QueryPipe / QueryPipeEx /
> ReadPipe / WritePipe / ControlTransfer / GetDescriptor / SetPipePolicy / GetPipePolicy /
> GetAssociatedInterface / AbortPipe / ResetPipe / FlushPipe / Free / QueryDeviceInformation /
> ParseDescriptors / Isoch 系列`。**claim 由 `WinUsb_Initialize` 隐式完成（且是独占的），
> 由 `WinUsb_Free` 释放。** 工具里也据此实现：按 device path 直接 `WinUsb_Initialize`，
> 不去找 claim API；`--diagnose` 里若发现该符号存在才调用（向前兼容）。
> 任务描述里"调用 `WinUsb_ClaimInterface`"这一条**物理上无法满足**，属于任务书的笔误。

### 6.3 `WinUsb_Initialize` 报 `err=6 ERROR_INVALID_HANDLE` / `err=8 ERROR_NOT_ENOUGH_MEMORY`
→ **该 path 的驱动不是 WinUSB**。`CreateFile` 能成功不代表能 WinUsb_Initialize。
本机最典型的就是 usbccgp **父节点**（`#probe-0001#{a5dcbf10-…}`）：`CreateFile` OK，
`sync` 下 `err=6`、`overlapped` 下 `err=8`，两种模式都失败。
这与 `ERROR_ACCESS_DENIED` 的区别很重要：后者是"被占用"，这里是"根本不是 WinUSB 设备"。

### 6.4 `CreateFile` 报 `err=2 ERROR_FILE_NOT_FOUND`
→ **符号链接悬空**：设备正在复位/重枚举（注册表里的 DeviceClasses 条目还在，但设备对象没了）。
**这不是本工具的 bug。** 工具对这种情况会在 `[4]` 里标注
`-> dangling/absent symbolic link: the device was re-enumerating or is gone`，
并在正式运行路径上等 10 s 重试一次（`--open-retries`）。
> 实测记录：usb-core 反复烧写期间，同一路径的 8 次连续采样**全部成功**，
> 而一次耗时数秒的 `--diagnose` 运行正好撞上重枚举窗口、报 `err=2` —— 即
> 该失败是**时间窗口**问题，与代码无关。

### 6.5 回环**首字节就失配** → 先怀疑心跳没剥离干净
设备空闲 ≥1 s 会主动发 `\nHB <ms>\n`，而**心跳与回环数据共用同一个 IN 流**。
若剥离不干净，echo 缓冲里会多出 `HB`/数字/换行字节，造成**从第一个字节开始整体错位**。

本工具用一条硬性质消除歧义：**回环 payload 永不包含 `0x0A`**（生成时把 `0x0A` 重映射为 `0x0B`），
而心跳固定写作 `\nHB <n>\n`。因此：
> **IN 流里出现的每一个 `0x0A` 都只可能来自心跳。**

剥离状态机据此把 `\n…\n` 行识别为心跳、从 echo 流中移除、其余字节**按原顺序**接回
（所以心跳即使插在数据中间也不影响逐字节比对）。`--diagnose` 无关；回环报告里的
`pre-case junk` 与 `hb lines before/after` 可用于交叉确认。
若仍失配，报告会给出**失配首偏移 + 期望字节 + 实际字节**，据此判断是错位还是个别位翻转。

### 6.6 回环超时 / 死锁 → 先怀疑**串行读写**
设备侧仅约 12 KB 缓冲（vendor RX/TX FIFO 各 4 KB + 8 KB stream buffer）。
"先 `transferOut(N)` 全部写完再 `transferIn`"在 N ≳ 12 KB 时必然 NAK 死锁。
本工具的对策：
1. **读线程先挂上**：`run_case()` 里先 `Reader.start()` 再 `sleep(50 ms)`，确保**第一个 IN 读请求在第一次 OUT 写之前**已经 post；
2. 分块 **≤ 4096**（`--chunk` 强制夹住）；
3. 双超时：单块 `--timeout`（写进 `PIPE_TRANSFER_TIMEOUT`）+ 用例总超时 `--case-timeout`；
4. 退出时 `WinUsb_AbortPipe` + `WinUsb_ResetPipe` 兜底，保证不永久卡死。

若报告里 `write-timeouts > 0` 且几乎没写出字节 → 典型的**设备没在收**（FIFO 满/固件未起回环任务）。
若 `echo-timeouts > 0` 而写全部成功 → **设备收了但没回**。

### 6.7 用例总超时 / `--case-timeout` 到期
→ 用于区分「慢」和「卡死」。卡死时报告会给出卡在哪个 offset、已写/已校验多少字节。

### 6.8 「0 失配却判 FAIL」→ 管道状态污染（已修）
真机踩到过：某次 64 KB 用例 `wrote=65536 verified=65536 mismatch=0` 却报 `FAIL`。
原因是**同一个 IN 管道上换 reader 时没有清 abort 状态**：
心跳探测的 reader `AbortPipe` 之后，紧随其后的回环 reader 首次 `WinUsb_ReadPipe` 直接失败，
于是 `reader.error` 被置位、用例被误判。
修法（`Reader.stop()` 里）：`AbortPipe` 之后补一次 **`ResetPipe`**，
并且当 `stop_flag` 已置位时不再把读失败记为错误。
**USB 纪律：`WinUsb_AbortPipe` 只取消在途请求，必须再 `WinUsb_ResetPipe` 清 halt/abort 态。**

### 6.9 用例起点必须重置「心跳剥离状态」（**本项目实际踩到过**）
症状：1 KB 用例 `wrote=1024 verified=1024` 但 `mismatch=1`，**首处失配在全局偏移 0**，
期望 `0xEF` 实际 `0x0A`；`rawInBytes = 1035 (k=+11)` 但 **`hbBytes = 0`**、
**`strayLFlines = 2`**、`strippedEcho = 1037`（多 13 字节）。

`--hexdump` 一跑就看清了：

```
RAW IN (before stripping):
  0000  0A 48 42 20 34 36 33 38 33 37 0A EF F6 4E CC ED   |.HB 463837...N..|
实际收到的 echo（剥离后）:
  0000  0A 48 0A 48 42 20 34 36 33 38 33 37 0A EF F6 4E   |.H.HB 463837...N|
```

**根因**：上一步的「心跳探测」reader 在**一行心跳中间**被 `AbortPipe` 停掉，
剥离状态机里留下了 `_in_line=True` + `_pending=b"H"`。
下一个用例的第一个 `0x0A` 去闭合那个**陈旧残片**，
于是 `\nH` 被当数据吐出（`strayLFlines=1`），随后 `HB 463837` 也失去行首 `\n` 而被当数据 →
**从偏移 0 起整体错位**，且真正的心跳一个都没识别出来（`hbBytes=0`）。

**修法**：`InStream.begin_case()` 除了清 `echo`/`raw_head`，还必须
`self._pending.clear()` 且 `self._in_line = False`
（新用例必须从**定义明确的干净流状态**开始）。修后 **8 次连续整套运行 8/8 PASS、失配全 0**。

> ⚠️ **这条与浏览器侧 1 MiB 的失败是同一类错误**（§0.4）：
> 「流的起点不在行边界 / 上一步的状态没清干净」⇒ 整体位移 k。
> 本工具先前也踩了同一个坑，说明这个坑**很容易踩**、**必须在用例起点显式处理**。
> 判读口诀：**`hbBytes = 0` 但 `rawInBytes > size` 且 `strayLFlines > 0` ⇒ 状态泄漏/起点不对齐。**

### 6.10 「会 drain 的主机」测不出「不 drain 的主机」的 bug（重要认知）
本工具在**每个用例前都会 drain**（等 IN 静默并丢弃残留，§6.4/§6.9），
所以它**永远触发不到**固件里那条"空闲期积压心跳排在 echo 前面"的修复点：
真正开始写 OUT 时设备 TX FIFO 已被清空。**能触发它的是"不 drain 的主机"（旧版页面）。**

因此这类问题的判据在**固件侧可观测日志**，而不是主机侧回环结果：

```
I (38108) usb_probe: idle->data flush TX: dropped 332 B queued heartbeat before echo
```

该行只在 `queued > 0` 时打印。实测：主机**只写不读**、先闲置 35.8 s 再写 1 KB →
`dropped 332 B`，`332 B ÷ ~11 B/行 ≈ 30 行 ≈ 35 s` 数量级自洽。

> ⚠️ **别用"TX FIFO 为空"当判据**：第一条心跳进 FIFO 后该条件恒假 →
> 心跳不再累积、flush 也永不触发。必须用**显式状态位**记录"FIFO 里只有心跳"。
> 这个坑正是靠上面那条日志可观测才被发现的。
>
> 也正因如此，**"给本工具加一个 `--no-prime`（写完再读、完全不预读）模式"曾被提出又撤回**：
> 它只能用于**观察**，而固件日志已能直接给出同一结论。**当前工具只保留已验证的 drain 语义**，
> 不为一个观察性功能去改动已经真机验证过的核心路径。

---

## 7. 实测记录（原始输出）

### 7.1 `--diagnose`（**修复前**的阻塞现场，退出码 2 —— 保留作为历史证据）

```
MeidaP4 WinUSB host-side loopback tool
  GUID_DEVINTERFACE_WINUSB      : {DEE824EF-729B-4A0E-9C14-B7117D33A817}
  GUID_DEVINTERFACE_USB_DEVICE  : {A5DCBF10-6530-11D2-901F-00C04FB951ED} (Chrome/Edge WebUSB)

[2] DeviceClasses registry entries whose symbolic link contains the devnode instance hash
  USB\VID_303A&PID_4002&MI_00\8&2B768EE9&0&0000   hash=8&2b768ee9&0&0000  -> {10497b1b-ba51-44e5-8318-a65c837b6661} (x1)
  USB\VID_303A&PID_4002&MI_00\8&2B768EE9&0&0000   hash=8&2b768ee9&0&0000  -> {6ac27878-a6fa-4155-ba85-f98f491d4f33} (x1)
  USB\VID_303A&PID_4002&MI_00\8&2B768EE9&0&0000   hash=8&2b768ee9&0&0000  -> {6bdd1fc6-810f-11d0-bec7-08002be2092f} (x1)
  USB\VID_303A&PID_4002&MI_00\8&2B768EE9&0&0000   hash=8&2b768ee9&0&0000  -> {f33fdc04-d1ac-4e8e-9a30-19bbd4b108ae} (x1)
  USB\VID_303A&PID_4002\PROBE-0001                hash=probe-0001        -> {a5dcbf10-6530-11d2-901f-00c04fb951ed} (x1)
  USB\VID_303A&PID_4002&MI_01\8&2B768EE9&0&0001   hash=8&2b768ee9&0&0001  -> 0 interface GUIDs          <-- 病征

[3] devnode detail: DeviceInterfaceGUIDs presence and registered paths

  instance : USB\VID_303A&PID_4002\PROBE-0001
    class  : USB {36fc9e60-c465-11cf-8056-444553540000}
    service: usbccgp
    DevParam subkeys: ['Ceip']
    DeviceInterfaceGUIDs : *** ABSENT ***
    => registered under {A5DCBF10-6530-11D2-901F-00C04FB951ED}
       path: \\?\usb#vid_303a&pid_4002#probe-0001#{a5dcbf10-6530-11d2-901f-00c04fb951ed}

  instance : USB\VID_303A&PID_4002&MI_01\8&2B768EE9&0&0001
    class  : USBDevice {88bae032-5a81-49f0-bc3d-a4ff138216d6}
    service: WINUSB   <-- WinUSB function driver
    hwids  : ['USB\\VID_303A&PID_4002&REV_0100&MI_01', 'USB\\VID_303A&PID_4002&MI_01']
    DevParam subkeys: ['e5b3b5ac-9725-4f78-963f-03dfb1d828c7', 'WDF']
    DeviceInterfaceGUIDs : *** ABSENT ***
        => winusb.sys registers NO interface for this devnode (registration is gated on this value)
    => NOT openable: no device interface registered          <-- 病征

[4] WinUsb_Initialize probe on every registered candidate path

  candidate: \\?\usb#vid_303a&pid_4002#probe-0001#{a5dcbf10-6530-11d2-901f-00c04fb951ed}
    guid={A5DCBF10-6530-11D2-901F-00C04FB951ED}  instance=USB\VID_303A&PID_4002\PROBE-0001
      CreateFileW (sync      ) FAILED  err=2 ERROR_FILE_NOT_FOUND  -> dangling/absent symbolic link: the device was re-enumerating or is gone
      CreateFileW (overlapped) FAILED  err=2 ERROR_FILE_NOT_FOUND  -> dangling/absent symbolic link: the device was re-enumerating or is gone

  candidate: \\?\usb#vid_0b05&pid_19af&mi_00#9&de2eaf5&0&0000#{dee824ef-729b-4a0e-9c14-b7117d33a817}
    guid={DEE824EF-729B-4A0E-9C14-B7117D33A817}  instance=USB\VID_0B05&PID_19AF&MI_00\9&DE2EAF5&0&0000
      CreateFileW (sync      ) OK; WinUsb_Initialize FAILED  err=6 ERROR_INVALID_HANDLE
      CreateFileW (overlapped) OK; WinUsb_Initialize OK  speed=LowSpeed

[5] ... (见 §5)

-- verdict --
  path exists but driver is not WinUSB: USB\VID_303A&PID_4002\PROBE-0001 ({A5DCBF10-6530-11D2-901F-00C04FB951ED})
  BLOCKED: no devnode with VID 0x303a exposes a WinUSB-usable device
           interface, so it cannot be opened with
           CreateFile / WinUsb_Initialize.

  Root cause: C:\WINDOWS\INF\winusb.inf maps 'USB\MS_COMP_WINUSB' to the WINUSB section, but that section's .HW sub-section is empty, so no DeviceInterfaceGUIDs is written (the only DeviceInterfaceGUIDs in that INF belongs to the ADB section).
  Plus: interface registration is gated on a DeviceInterfaceGUID(s) value in the device key.

  Fixes (pick one):
   (a) firmware (recommended): add an MS OS 2.0 Registry Property
       feature (wDescriptorType=4, wPropertyDataType=0x0007 REG_MULTI_SZ,
       PropertyData UTF-16LE with a trailing double NUL) on the VENDOR
       interface, name 'DeviceInterfaceGUIDs', value
         {A5DCBF10-6530-11D2-901F-00C04FB951ED}
   (b) host workaround (needs Administrator):
       reg add "HKLM\SYSTEM\CurrentControlSet\Enum\USB\VID_303A&PID_4002&MI_01\8&2B768EE9&0&0001\Device Parameters" /v DeviceInterfaceGUIDs /t REG_MULTI_SZ /d "{A5DCBF10-6530-11D2-901F-00C04FB951ED}" /f
       then disable/enable the device (Device Manager or pnputil).
   (c) design: make the vendor function non-composite (bDeviceClass=
       0xFF, no IAD/0xEF).
```

> `[4]` 里我们父节点的 `err=2` 是重枚举瞬态（§6.4）。同一路径的独立采样（8 次 / 8 s，全部成功）：

```
  0 CreateFile OK handle=524  Initialize=False err=8     ... (8/8 相同)
  7 CreateFile OK handle=536  Initialize=False err=8
```

### 7.2 阻塞判据 J1（退出码 2）

```
& $py tools\winusb_loopback.py --pid 0x4002 --mi 01 --size 1k
...
No candidate matched vid=0x303a pid=0x4002 mi=01.
Note: a device that registers NO device interface cannot appear
      here at all - there is no device path to match.
      Run with --diagnose to see exactly which devnodes exist and
      which device interfaces (if any) they register.
      Probe GUIDs used: {DEE824EF-729B-4A0E-9C14-B7117D33A817}, {A5DCBF10-6530-11D2-901F-00C04FB951ED}
```

### 7.3 打开失败路径（把父节点当候选时；退出码 2）

```
& $py tools\winusb_loopback.py --no-loopback
selected: \\?\usb#vid_303a&pid_4002#probe-0001#{a5dcbf10-6530-11d2-901f-00c04fb951ed}
  open attempt 1/2 failed: WinUsb_Initialize failed: 8 ERROR_NOT_ENOUGH_MEMORY  -> this device path is not bound to WinUSB (usbccgp parent nodes behave this way).
  waiting 10 s before retrying (device may be re-enumerating) ...

FATAL: WinUsb_Initialize failed: 8 ERROR_NOT_ENOUGH_MEMORY  -> this device path is not bound to WinUSB (usbccgp parent nodes behave this way).

Diagnosis hints:
  * Service on the interface must be WINUSB (winusb.inf / WCID).
    Check: Get-PnpDevice -PresentOnly | ? InstanceId -like '*VID_303A*'
  * Device Manager -> the MI_0x interface should show no warning icon.
  * If a browser tab holds claimInterface, close it first.
```

### 7.4 离线自测（不需要设备，34/34 通过，退出码 0）

```
payload LF-free                                 OK
payload deterministic                           OK
HB pure HB echo / uptime                        OK  OK
HB interleaved echo / uptime                    OK  OK
HB non-HB LF echo / uptime                      OK  OK          (不是心跳的 \n 行原样保留)
HB byte-at-a-time echo / uptime                 OK  OK          (逐字节喂也不丢状态)
HB malformed HB echo / uptime                   OK  OK          (HB 后非数字 -> uptime=None)
HB split-across-feeds echo / uptime             OK  OK          (心跳被切成 3 段)
wait_echo timeout / satisfied                   OK  OK
parse_size(1k/64k/1m/1KiB/1024)                 OK x5
auto_int(0x303A/303A/01/1/0x01/255/0x4002)      OK x7
WINUSB_PIPE_INFORMATION == 12                   OK   <-- 结构体布局关键
OVERLAPPED == 32                                OK
USB_INTERFACE_DESCRIPTOR == 9                   OK
SP_DEVINFO_DATA == 32                           OK
DEVPROPKEY == 20                                OK
SP_DEVICE_INTERFACE_DETAIL_DATA_W == 8          OK   <-- 64 位上必须是 8
GUID == 16 / GUID round-trip                    OK  OK

ALL OFFLINE CHECKS PASSED (0 failures)
```

> 开发期间这个自测**抓到过两个真 bug**，都已修：心跳状态机用「缓冲非空」判断"是否在某行里"，
> 导致 `\nHB 1234\n` 被当成数据（已改为显式 `_in_line` 状态位）；
> 以及 `WinUsbDevice.open()` 用关键字参数只传了 7 个参数中的 5 个，
> 会让**整条回环主路径**在第一次 `CreateFile` 就抛 `TypeError`（已修）。

### 7.5 设备侧状态确认（只读）

```
PS> Get-PnpDevice -PresentOnly | ? InstanceId -like '*VID_303A*'
Status FriendlyName         InstanceId
OK     MeidaP4 Storage      USB\VID_303A&PID_4002&MI_00\8&2B768EE9&0&0000
OK     USB Composite Device USB\VID_303A&PID_4002\PROBE-0001
OK     MeidaP4 WebUSB       USB\VID_303A&PID_4002&MI_01\8&2B768EE9&0&0001
```

所有 devnode 状态 `OK`、无警告标志 —— 再次说明**问题不在驱动/枚举，而在接口注册**。

---

## 8. 已验证 / 未验证清单

### 8.1 已在本机实测通过 ✅
- SetupAPI 枚举（`SetupDiGetClassDevs` / `EnumDeviceInterfaces` / `GetDeviceInterfaceDetailW`）
  与 PnP 属性（`SPDRP_HARDWAREID` / `COMPATIBLEIDS` / `FRIENDLYNAME` / `SERVICE` / `CLASS`、
  `DEVPKEY_Device_BusReportedDeviceDesc`）。
- `--diagnose` 全部 5 节 + verdict：修复前退出码 2、修复后**退出码 0 + `PASS`**。
- `--list` 候选清单（含 `WinUSB-compat` 判定）。
- DeviceClasses 注册表扫描（父节点 1 条 / MI_00 4 条 / MI_01 0 条 → 修复后 MI_01 变 2 条）。
- `CreateFile` + `WinUsb_Initialize` 探测，两种 io 模式，含 `GetLastError` 归因。
- 对**另一台** WinUSB 设备（华硕 `VID_0B05&PID_19AF&MI_00`）实测到 `WinUsb_Initialize OK` + `speed=LowSpeed`
  → 证明本工具的 WinUSB 调用链（结构体布局、argtypes、overlapped 路径）**本身是通的**，不是工具写错。
- **本设备的 `WinUsb_Initialize` / `QueryInterfaceSettings` / `QueryPipe`**：pipe = IN `0x83` + OUT `0x03`，
  均 Bulk `wMaxPacketSize=512` `Interval=0`，接口 `bInterfaceNumber=1`、`0xFF/0x00/0x00`、`iInterface=4`。
- **`WinUsb_QueryDeviceInformation(DEVICE_SPEED)`** → `HighSpeed (3)`。
- **`WinUsb_SetPipePolicy(PIPE_TRANSFER_TIMEOUT / ALLOW_PARTIAL_READS)`** 已生效（超时可复位、无永久卡死）。
- **并发读线程 + 分块回环 + 逐字节校验 + 吞吐 / RTT / 失配首偏移**：
  1 KB / 64 KB / 1 MB 全部 `mismatch=0`，1 MiB 达 ~51 Mbit/s（6.2 MiB/s），RTT 0.56/0.63/0.97 ms。
- **串口 `out`/`in` 增量逐包对账**：`+1115136 B / +2178 pkt`，`out == in`（§0.2）。
- **心跳真机解析**：一次运行内剥离 374 行积压心跳（uptime 1.9 s→374.9 s），`k − hbBytes = 0`。
- 心跳剥离状态机、LF-free payload、结构体尺寸、参数解析（34/34 离线自测）。
- GUID 语义查证（§3，含 `winusb.sys` 字节模式定位、hub 驱动定位与 Chrome/Edge 二进制扫描）。
- 退出码语义（含 `--help`/`--list` 恒 0）、`--open-retries` 的 10 s 重试路径、`err=2` 重枚举标注。
- 管道状态卫生：`AbortPipe` 后必须 `ResetPipe`，否则**下一个** reader 的首次读会失败
  （实测踩到过，表现为"0 失配却判 FAIL"，已修，见 §6.6）。

### 8.2 仅代码完成、未实测 ⚠️
- `--io-mode sync`（同步句柄 + 读线程）路径未在真机跑过（默认 `overlapped` 已充分验证）。
- `--pipeline > 1` 未跑过（默认 1）。
- `--hb-only` 独立模式的端到端行为（心跳探测已在正常流程里实测）。
- `--report FILE` 落盘（正常回环路径未带 `--report` 跑过；`--diagnose` 会提前 return 不写报告）。
- `--device-path` 覆盖路径未实测。
- `echo_errors` 的**数值**无法从串口读取（固件 `stats` 行没有该字段，见 §0.2）。

### 8.3 已知无法满足 / 与本机不符 ❗
- **`WinUsb_ClaimInterface` 在本机 `winusb.dll` 中不存在**（§6.2）。任务书要求"调用它"无法满足；
  已按实际机制实现（claim 隐式于 `WinUsb_Initialize`），并在文档中明确记录。
- 任务书给出的 `GUID_DEVINTERFACE_WINUSB = {A5DCBF10-…}` **不正确**：该 GUID 是
  `GUID_DEVINTERFACE_USB_DEVICE`（§3）。真正的 `GUID_DEVINTERFACE_WINUSB` 是 `{DEE824EF-…}`。
- 任务书建议的 `{F72FE0D4-…}`（来自 winusb.inf）是 **ADB 段**的值，不适用于本项目（§3.1、§3.5）。
- "抓基线 → 跑工具 → 再抓一次"这种对账法在本机**不成立**（开串口必复位，见 §0.3）。

---

## 9. 后续与联动

1. **等 usb-core 完成 `task-8`（MS OS 2.0 Registry Property）后复跑 `--diagnose`**：
   看 `[5]` 里 MI_01 是否出现 `DeviceInterfaceGUIDs` 且 `registered GUIDs` 非空。
2. **模式 A 改成非复合（`bDeviceClass=0xFF`、单接口、无 IAD）后复跑 `--diagnose`**：
   预期父节点自己就是 WinUSB 设备，`{A5DCBF10-…}` 的 path 应能**直接 `WinUsb_Initialize` 成功**
   —— 这是"复合 + 缺 `DeviceInterfaceGUIDs` 才是根因"的**第二条独立证明**。
3. ~~接口一旦可打开，立即执行 §8.2 的未验证项~~ → **已完成，见 §0.2**（1 KB / 64 KB / 1 MB 全绿）。
4. **交回 web-host**：浏览器侧 1 MiB 失败的三项根因线索见 §0.4
   （① 回环前无 drain/prime；② `appendParts()` 静默丢弃 surplus）。
   修复后在页面导出里核对 **`rawInBytes − size − hbBytes`**：该值应为 **0**。
5. **需要固件配合的一件小事**：把 `echo_errors` 加进 `stats` 打印行，
   否则主机侧无法从串口确认设备 stream buffer 是否丢过回环字节（§0.2）。
6. 本文档 §0 与 §8 的状态表**已按真机结果更新**：原先"未验证（硬阻塞）"现为"已通过"。
