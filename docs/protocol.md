# I0 · 线协议规约（WebUSB vendor 通道）—— 冻结 v1

> 上位：[usb-composite-plan.md](./usb-composite-plan.md) §2/§5 · 分配：[contracts/frozen-allocation.md](./contracts/frozen-allocation.md)
> 生产者：Lead。消费者：`components/webusb_proto/`（C 实现）· `web/proto.js`（JS 实现）· `tools/verify-proto.ps1`（离线校验）
> **状态：冻结。** 改这里 = 同时改 C 与 JS 两侧 + 重新生成 `tools/proto_vectors.json`，必须 Lead 批准。

---

## 0. 为什么要有它

探测期（批 0）的 vendor 通道是"裸回环 + 心跳行"，真机暴露了三个主机侧缺陷
（`probe-report.md` §3.4/§2.6）：心跳行跨用例状态泄漏、测量前未排空、**ZLP 被当致命错误**。
根因是**没有帧头** ⇒ 主机只能靠猜行边界。所以完整版第一件事就是定死帧格式。

**同一套帧格式由 C 和 JS 各实现一次**（`webusb_proto.c` / `web/proto.js`），
用 `tools/proto_vectors.json` 做**逐字节**双向校验 —— 这是 I0 唯一的客观验收方式（离线）。

---

## 1. 承载与端点

| 逻辑通道 | 走哪个物理端点 | 方向 | 可靠性 |
|---|---|---|---|
| `CTRL (0)` | IF2 · **EP4 OUT**（请求）/ **EP4 IN**（响应） | 双向 | **可靠**：ACK + 重传 |
| `FILE (2)` | 同上（EP4） | 双向 | **可靠**：ACK + 重传 |
| `VIDEO (1)` | IF1 · **EP3 IN**（独占） | 单向上行 | **不可靠**：宁可丢帧不重传 |
| `LOG (3)` | IF2 · **EP4 IN** | 单向上行 | 不可靠 |
| `TELEM (4)` | IF2 · **EP4 IN** | 单向上行 | 不可靠 |

> **为什么视频独占 EP3**：plan §5.2 要求"视频通道与大文件传输分用不同 EP（避免队头阻塞）"。
> 若视频与 FILE 共用 EP4，一个 200 KB 的 JPEG 会把一次文件 READ 响应推迟到 200 ms 之后。

**每个物理端点上的字节流是"若干帧首尾相连"**，没有分隔符，靠 magic + length 自解析。

---

## 2. 帧格式（定长 12 字节头，小端）

```
偏移  宽度  字段        说明
  0    1    magic0      'M' = 0x4D
  1    1    magic1      'P' = 0x50
  2    1    version     = 0x01
  3    1    channel     0=CTRL 1=VIDEO 2=FILE 3=LOG 4=TELEM
  4    2    flags       见 §3
  6    2    length      payload 字节数，0 .. 1024
  8    2    seq         通道内递增序号（0..65535 回绕）
 10    2    crc16       CRC-16/CCITT-FALSE，覆盖 **字节 [0,10) 然后 payload**
--- 12 字节头结束，紧随 length 字节 payload ---
```

- **`magic` 是 2 字节字面量 `4D 50`**（ASCII "MP"）。
- **`version` 不匹配 ⇒ 该帧作废**，按 §6 重新同步。
- **`length > 1024` ⇒ 该帧作废**（防越界；接收方缓冲上限 `WU_MAX_PAYLOAD = 1024`）。
- **`crc16` 算法（冻结）**：`poly=0x1021, init=0xFFFF, refin=false, refout=false, xorout=0x0000`
  —— 即 CRC-16/CCITT-FALSE。计算顺序：先喂 `hdr[0..10)`，再喂 payload。
  ⚠️ 不是 Modbus CRC（那个 init=0xFFFF 但 poly=0x8005），**不要搞混**。
- 最大帧 = 12 + 1024 = **1036 字节**。

### 2.1 为什么 payload 上限只有 1024

- 512 字节 bulk 端点 ⇒ 一帧 = 3 个 USB 包（含头），主机侧解析缓冲小、延迟低；
- 大消息（视频帧、文件块）靠 §4 的 MORE 位**分片**，分片粒度小反而降低尾延迟；
- 设备侧发送队列可静态分配（无动态内存）。

---

## 3. `flags` 位定义（冻结）

| 位 | 名字 | 含义 |
|---|---|---|
| `0x0001` | `MORE` | 本帧之后**还有**属于同一逻辑消息的分片 |
| `0x0002` | `ACK` | 本帧需要接收方回一个 `ACK`（**可靠通道的每一帧都置位**） |
| `0x0004` | `RETX` | 本帧是重传 |
| `0x0008` | `ABORT` | 发送方放弃当前消息，接收方**必须丢弃**已拼装的部分 |

其余位保留，必须为 0。

---

## 4. 消息与分片

- **消息（message）** = 1 个或多个帧。第一帧开头即消息内容，帧头 `seq` 逐帧 +1（**同通道内**）；
  最后一个分片的 `MORE = 0`。
- 收到新消息的第一帧时若上一个消息未完成 ⇒ **丢弃上一个未完成消息**并计数（`msg_aborted`）。
- 收到 `ABORT` ⇒ 丢弃当前拼接，且该 `seq` 之后的帧到达时若 `MORE` 链已断也丢弃。
- **`seq` 空间是"每通道每方向"独立的**：设备→主机的 VIDEO `seq` 与主机→设备的 CTRL `seq` 互不相干。
- 分片后单帧 payload ≤ 1024；一条消息最大 65536 × 1024 字节（实际不受限，因为 `seq` 是逐帧递增）。

### 4.1 ⚠️ ZLP（零长度包）语义 —— 冻结（真机踩过的坑）

`probe-report.md` §3.4/§2.6 缺陷 3 实测：当一次 IN 传输长度**恰好是 `wMaxPacketSize` 的整数倍**
且设备 FIFO 已空时，TinyUSB 会补一个 ZLP，在 WebUSB 侧表现为**一次独立的 0 字节 `transferIn`**。

**冻结规则（两侧都必须遵守）**：

1. **0 字节 IN 读是合法且非致命事件**。主机必须把它计入 `zeroLenReads` 并**继续读**，
   用"用例总超时"兜底；**不得**当成错误中止。
2. 0 字节读**不产生任何帧**，**不推进任何状态**。
3. 设备侧：从"空闲"转为"有数据要发"时，**必须先丢弃残留的 TX 队列**再发第一帧
   （即：不允许把上一次的陈旧字节混进这一次的流）。帧头 + CRC 使这一点不再是正确性必需，
   但仍是**性能必需**（避免主机先解析一堆过期遥测）。

---

## 5. 通道语义

### 5.1 CTRL（0）· 可靠

payload 前 4 字节是固定子头，后续为 `arg` 相关体：

```
 0  1  opcode
 1  1  status       请求帧填 0；响应帧填 0=OK，非 0=错误码
 2  2  arg          小端，语义由 opcode 决定
 4  .. body
```

| opcode | 名字 | 方向 | arg | body |
|---|---|---|---|---|
| `0x01` | `HELLO` | 设备→主机 | 协议版本 | `u32 caps`（能力位图） |
| `0x02` | `HELLO_ACK` | 主机→设备 | 协议版本 | 空 |
| `0x03` | `PING` | 双向 | 0 | 空 |
| `0x04` | `PONG` | 双向 | 0 | 空 |
| `0x05` | `ACK` | 双向 | **被确认的 seq** | 空 |
| `0x06` | `CREDIT` | 双向 | **通道号** | `u16 bytes`（该通道可用额度） |
| `0x10` | `WIFI_SCAN` | 主机→设备 | 0 | 空 |
| `0x11` | `WIFI_SCAN_RSP` | 设备→主机 | 条目数 | `N × {i8 rssi, u8 authmode, char ssid[33]}` |
| `0x12` | `WIFI_CONNECT` | 主机→设备 | 0 | `char ssid[33] + char pass[65]` |
| `0x13` | `WIFI_STATUS` | 设备→主机 | `0=down 1=connecting 2=up` | `u32 ipv4_be, u8 mac[6], i8 rssi` |
| `0x20` | `UI_CMD` | 主机→设备 | 命令 | 见 WS-video |
| `0x30` | `FILE_*` | 双向 | 由 FILE 通道承载，见 §5.3 | |
| `0x40` | `STATS_REQ` / `0x41 STATS_RSP` | 双向 | 0 | 见 §5.4 |

- **能力位图 `caps`**（`HELLO` 的 body）：`bit0 VIDEO_MJPEG`、`bit1 VIDEO_H264`、`bit2 FILE`、
  `bit3 LOG`、`bit4 TELEM`、`bit5 WIFI`、`bit6 UI_CMD`。
- **`status` 错误码**：`0x00 OK`、`0x01 EINVAL`、`0x02 ENOENT`、`0x03 EIO`、`0x04 ENOSPC`、
  `0x05 EBUSY`、`0x06 ENOTSUP`、`0x7F EUNKNOWN`。

### 5.2 VIDEO（1）· 不可靠

一条消息 = **一整个编码帧**。payload 前 16 字节固定：

```
 0  1  fmt      0=MJPEG 1=RGB565 2=H264
 1  1  flags    bit0 = 关键帧（H.264 用；MJPEG 恒 1）
 2  2  width
 4  2  height
 6  2  fps_num      分子
 8  2  fps_den      分母
10  4  frame_seq    编码帧序号（只在完整帧上 +1）
14  2  chunk_index  本帧在本消息里的分片序号（从 0 起）
--- 16 字节结束，紧随编码数据 ---
```

- **丢弃策略（冻结）**：发送方**永不阻塞**视频发送。IN 端点瓶颈时：
  1. 若当前消息还没发完，立刻丢弃剩余分片；
  2. 已发出的分片无法收回 ⇒ 接收方会因缺少 `MORE=0` 的收尾帧而**丢弃整个消息**，
     并由 `frame_seq` 跳变检测到丢帧（计入 `dropped_frames`）；
  3. 图省事也可发一个 `ABORT` 帧（可选优化，不强制）。
- 接收方：`frame_seq` 不连续 ⇒ 前向丢帧计数 +1，不必也不能请求重传。

### 5.3 FILE（2）· 可靠

一条消息 = 一个文件操作请求/响应。payload 前 8 字节：

```
 0  1  op        0=LIST_REQ 1=LIST_RSP 2=OPEN_REQ 3=OPEN_RSP 4=READ_REQ 5=READ_RSP
                 6=WRITE_REQ 7=WRITE_RSP 8=CLOSE_REQ 9=CLOSE_RSP 10=DELETE 11=MKDIR 12=RENAME
 1  1  status    见 §5.1 错误码（请求帧填 0）
 2  2  xid       事务 id，请求与响应配对
 4  4  offset    文件内偏移（READ/WRITE）
 8  .. body      READ_RSP/WRITE_REQ 直接跟数据；LIST_RSP 跟 N × {u32 size, u8 is_dir, char name[256]}
```

> 详细文件操作语义由 WS-webusb 在 `web/file.js` + `components/usb_vendor/file_service.c` 里定；
> **但上面 8 字节头与 `xid` 配对规则属于 I0，不得改。**

### 5.4 TELEM（4）· 不可靠，36 字节定长

```
 0  4  uptime_ms
 4  4  heap_free
 8  4  psram_free
12  4  usb_in_bytes
16  4  usb_out_bytes
20  4  video_kbps
24  4  video_dropped
28  4  video_fps_x100
32  1  wifi_rssi (i8)
33  1  flags      bit0=wifi_up bit1=sd_mounted bit2=video_active
34  2  reserved  必须 0
```

**发送节奏**：心跳（`PING`/`PONG`）1 s；`TELEM` 1 s；两者都只在链路上行有余量时发。
`TELEM` 与 `PING` **绝不占用视频端点**。

---

## 6. 流控 / ACK / 重传 / 重连（冻结）

### 6.1 可靠通道（CTRL / FILE）

- **滑动窗口 `W = 8` 帧**（发送方最多 8 个未确认帧）。
- 接收方见到 `ACK` 位置位的帧 ⇒ 回一个 `ACK` 帧（opcode `0x05`），
  `arg = 下一个期望的 seq`（**累计确认**）。
- 发送方收到 `arg = X` ⇒ seq < X 的帧全部释放。
- **超时 200 ms** 未推进窗口 ⇒ 重传窗口内最老一帧并置 `RETX`。
- **连续 5 次重传失败** ⇒ 判定链路死亡：清空该通道 TX 队列、置 `link_down`、
  回到 §6.3 的"等 `HELLO_ACK`"状态。
- 接收方对**重复 seq** 必须幂等：直接再回一次 `ACK`，**不得**把数据重复投递给上层。

### 6.2 不可靠通道（VIDEO / LOG / TELEM）

- 无 ACK、无重传、无窗口。发送方若端点忙则按 §5.2 丢弃。
- 接收方按 `seq` 连续性统计丢帧，但不报错。

### 6.3 建立与重连（**设备是序号主导方**）

```
上电/挂载:
  设备 每 2 s 发 HELLO 直到收到 HELLO_ACK（同时把 TELEM/PING 也发出去，让主机能看见）
  主机 收到 HELLO → 回 HELLO_ACK → 把自己所有 TX seq 归 0，RX 期望值归 0，清空拼接状态
  设备 收到 HELLO_ACK → 把自己所有 TX seq 归 0，RX 期望值归 0，清空发送队列，link_up

运行中:
  任一侧连续 3 s 没收到任何**有效帧** ⇒ 标记 link_down（UI 显示"重连中"），
      清空拼接状态与未确认窗口；设备额外把自己的 TX 队列清空。
  设备随后重新进入"每 2 s 发 HELLO"状态。
  主机在 link_down 期间不做任何可靠发送（避免与设备的序号重置打架）。
```

- **为什么让设备主导**：设备可能因为看门狗复位而"忘记"一切，而主机不知道。
  让设备在每个新 `HELLO` 周期里强制重置，是最简单的收敛方式。
- `HELLO` 的 `arg` = 协议版本（本文件 = `0x01`）。

### 6.4 帧级容错（必须，不是可选）

- **CRC 失败**：丢该帧并 `crc_errors++`。可靠通道靠 ACK 超时自动重传；不可靠通道直接放弃。
- **magic/version/length 非法**：丢弃当前候选帧，**从下一个字节开始重新扫 `4D 50`**（resync）。
  **绝不**中止流、**绝不**要求重连 —— 这是对 §4.1 的补充：链路上永远可能存在我们没预料到的字节。
- 每侧都必须维护 `resync_bytes` / `crc_errors` / `zero_len_reads` 三个计数器并允许读取（诊断用）。

---

### 6.5 补充冻结（2026-10 裁决）—— ACK 归属 / seq 消耗 / 引导帧

> 来源：ws-webusb2 在交接时提出这三处"§5–§6 未写清、但 C 与 JS 必须两侧一致"的口径。
> Lead **逐条批准**，正式并入 I0。**两侧实现统一按本节，不得各自发挥。**

**① 每个通道的 ACK 走它自己的通道（不集中到 CTRL）。**
理由：ACK 帧的 `arg` 只能表达**一个**通道的"下一个期望 seq"。若把 FILE 的 ACK 发到 CTRL，
CTRL 无法同时表达"CTRL 的期望值"和"FILE 的期望值"→ **FILE 永远无法被确认**。
⇒ `ACK` 帧与它确认的数据帧**同通道**。

**② seq 消耗规则：某方向某通道的每一个**数据**帧都吃掉一个 seq；`ACK` 帧**不占 seq**。**

> ⚠️ **2026-10-31 修订（真机实测后推翻原裁决）**：原文是"含 ACK 帧本身"，真机证明这条会制造
> **不可恢复的死锁**：
> - `ACK` 帧按 ③ 是"终结点"，不进重传窗口、丢了也不补发；
> - 但它又吃掉一个 seq ⇒ 只要**任意一条 ACK 帧在 USB 上丢了**，接收方的"下一个期望 seq"就
>   **永久**停在旧值：之后所有数据帧都被判 gap 丢弃 → 发送方重传 5 次 → `link_down` →
>   重连后同一条 ACK 再丢 → 无限循环。
> - 真机指纹：浏览器 LOG 面板 `gap` 单调增长、`dup=0`、`retx` 增长、每 1–2 s 一次
>   `retx_exhausted`；设备侧状态行 `rxexp` 很小而 `txnext` 已走远。
>
> **新规则（冻结）**：
> 1. `ACK` 帧一律 `seq = 0` 发送，**不推进发送方的通道 seq**（`next_seq` 不变）；
> 2. 接收方在 **seq 门之前**按"`payload` 长度恰为 4 且首字节 `0x05`"识别 `ACK`，
>    直接消费其 `arg`（累计确认），**不** `expected += 1`、不进入 seq 门判定；
> 3. 其余所有可靠帧（数据 `PING`/`FILE` 请求与响应等）仍按原规则吃 seq、参与重传。
>
> 两侧实现（`components/webusb_proto/webusb_proto.c`、`web/proto.js`、`tools/webusb_app.py`）
> 已同步；`HELLO`/`HELLO_ACK` 的引导豁免见 ⑥/⑦，与本节互不冲突。
> 原 ② 的文字仅作历史保留，**以本节修订为准**。

**③ ACK 帧本身不置 `WU_FLAG_ACK` 位。**
理由：§6.1 规定"可靠通道里每个置 `ACK` 位的帧都要被确认"；若 ACK 帧也置位，
就会要求对方再确认 ACK → **ACK 风暴**（指数级帧放大）。⇒ ACK 帧是"终结点"，不再要求回执。

**④ 引导阶段（`HELLO` 与主机 `HELLO_ACK` 之间）的主机侧 `PING`：`seq = 0`、`flags = 0`，不占可靠窗口。**
理由：§6.3 规定"主机收到 `HELLO` 后把自己所有 TX seq 归 0"，而设备的 `HELLO` 是**周期性重发**的；
若引导期的 `PING` 占窗口，它会在下一次 `HELLO` 触发重置时变成"悬空未确认帧"，让窗口永远推不动。
⇒ 引导期的 `PING` 是**无状态探测**，不入窗口、不参与重传。
（进入 `link_up` 之后，`PING` 恢复为普通可靠帧，正常占 seq 与窗口。）

**⑤ `resync_bytes` 口径（与 `wu_codec.h` 冻结文字一致，避免 C/JS 分叉）。**
非法候选帧头**自身的 12 字节不计入** `resync_bytes`（该段由 `bad_header` 以**事件**计）；
只计入其后到下一个 `4D 50` 之间被跳过的垃圾字节。
基准：`stream/length1025_illegal` → `bad_header=1`、`resync_bytes=32`，
且该值与**喂入粒度无关**（整块喂 = 逐字节喂）——**这是一条必须被两侧断言住的不变量**。

**⑥ 引导帧必须绕开 seq 门（**这是 ④ 的必要配套，缺了它链路永远收敛不了**）。**
`HELLO` 与 `HELLO_ACK` 一律 `seq = 0`、`flags = 0`，且**接收方对它们不看 seq 门、直接处理**。
理由：④ 规定引导期主机 `PING` 也用 `seq=0`，它会把该方向"下一个期望 seq"推到 1；
随后设备的 `HELLO_ACK`（仍是 `seq=0`）就会被 seq 门判为**重复帧丢弃** →
主机永远等不到 `HELLO_ACK` → **永久停在 link_down，链路永不收敛**。
⇒ 两侧实现都必须对 `HELLO`/`HELLO_ACK` 单帧做**豁免**。

**⑦ 引导期的 `HELLO` / `HELLO_ACK` 同样无状态**：`seq=0`、`flags=0`、**不占可靠窗口、不参与重传**。
理由与 ④ 完全相同：设备的 `HELLO` 是周期性重发并会触发序号重置，
若引导帧占窗口，它会在下一次重置时变成悬空未确认帧，窗口永久推不动。

**⑧ FILE 通道的 ACK 识别规则**：FILE 通道上的 ACK 靠"**payload 长度恰为 4 且首字节 = `0x05`**"无歧义识别。
理由：ACK 载荷就是 CTRL 的 4 字节子头，而 FILE 消息最短 8 字节 ⇒ 长度本身即判别式，不需要额外标志位。

> ⚠️ **⑥⑦⑧ 的来历与教训（Lead 留档）**：这三条由 ws-webusb 在实现时发现并**在两侧一致实现**，
> 但 Lead 曾只把它们写进**消息**、**没有及时落进本文件**（verifier 于 2026-10 复核时证明
> 全仓 `*.md` 检索 `⑥/豁免/seq 门` **零命中**）。这造成了本工程最危险的一种形态：
> **规约存在于实现里而不在文档里 —— 任一侧将来按文档重写，豁免会静默消失，
> 而文档不会提醒他**；偏偏 ⑥ 是"链路永不收敛"唯一的回归防线。
> ⇒ **教训：口头/消息裁决不算冻结。裁决必须落进本文件才算数。**
> 落档后，verifier 的向量 `link/bootstrap_frames_bypass_seq_gate` 才从 `requires_spec`(SKIP) 转为硬断言。

---

## 7. 主机侧最小实现清单（`web/proto.js` 必须提供）

```js
export const MAGIC = 0x504D;           // 小端读出的值；字节序为 4D 50
export function crc16(bytes) {}         // 与 C 侧逐字节一致
export function encodeFrame({channel, flags, seq, payload}) {}  // -> Uint8Array
export class FrameDecoder {             // 流式；喂 Uint8Array，吐出帧
  constructor(); push(bytes) {}         // 内部处理 resync / CRC
  get stats() {}                        // {crcErrors, resyncBytes, frames}
}
export class ReliableChannel {}         // 窗口 W=8 + ACK + 200ms 重传 + 5 次失败
export class LinkSession {}             // HELLO/HELLO_ACK 状态机 + 3s link_down
```

**ZLP**：`LinkSession` 的读取循环必须把 `transferIn` 返回 0 字节当合法事件（§4.1），
计入 `zeroLenReads` 并继续。

---

## 8. 离线验收（客观、可复现）

1. `tools/proto_vectors.json` —— 由 Lead 生成并冻结的黄金向量：
   `{name, channel, flags, seq, payload_hex, frame_hex, crc16}`。
2. `pwsh -File tools/verify-proto.ps1` 必须同时：
   - 用 **原生 gcc** 编译 `components/webusb_proto/wu_codec.c` + `test/host/test_codec.c` 并运行；
   - 用 **node** 运行 `web/proto.js`；
   - 两侧对每个向量断言：`encode == frame_hex`、`crc16 == crc16`、`decode(encode(x)) == x`。
   **任何一侧不一致即 FAIL。**
3. 额外鲁棒性向量（必须包含）：
   - 拼接多个帧后再喂（应解出全部）；
   - 帧前/帧间**插入垃圾字节**（应 resync 且不丢后续帧）；
   - **被改坏 CRC 的帧**（应丢弃，且不影响后续帧）；
   - `length = 0` 的合法帧；
   - `length = 1025` 的非法帧（应丢弃并 resync）；
   - 一次只喂 1 字节（分片喂入）应得到完全相同的结果。

> ⚠️ **边界声明**：这些都只证明"两侧实现符合本文档"，**不能**证明真机 USB 链路成立。
> 真机验收见 `docs/device-test-requests.md`。
