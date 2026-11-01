# MeidaP4 完整版 · 离线验证报告（verifier / task-4）

> 快照时间：**2026-10-17 09:09 +08:00**（本报告所有结论都绑定到这个时刻的文件 sha256，
> 见 §2。并发写入环境里，**不带时间戳与哈希的结论不可复核**。）
> 规约：[docs/protocol.md](../protocol.md) · 冻结分配：[docs/contracts/frozen-allocation.md](../contracts/frozen-allocation.md)
> 前序：[docs/probe-report.md](../probe-report.md)（批 0）
> 一句话：**离线能证明"实现与冻结规约逐字节一致"，不能证明真机 USB 链路成立。设备未连接。**

---

## 0. 一句话结论（快照 09:17:23 —— **两个维度都绿**）

| 验证面 | 对象 | 结果 |
|---|---|---|
| 黄金向量完整性 | `tools/proto_vectors.json`（29 个向量，verifier 独立生成） | ✅ PASS（生成器重算与仓库文件逐字节一致） |
| **I0 线协议 · C 侧** | `components/webusb_proto/wu_codec.c` | ✅ **PASS 679/679** |
| **I0 线协议 · JS 侧** | `web/proto.js` | ✅ **PASS 345/345**（14 条 SKIP：JS stats 未暴露 `badHeader`，`protocol.md §7` 未强制该字段） |
| **I0 线协议 · C↔JS 逐字节一致** | 同一批 29 向量 | ✅ 一致 |
| **I1 描述符装配** | `components/usb_composite/usb_composite_desc.c` | ✅ **PASS 389/389**（含冻结表 §1.1 的双 alt 规则、§4 更正后的字符串索引、884/140 FIFO） |
| I1 负例（超 IN 预算） | 同上 | ✅ PASS 2/2（正确拒绝） |
| **I1 集成：真实 NCM 片段 × 真实装配器** | `components/usb_net/usb_net_ncm.c`（ws-net 的真实注册函数） | ✅ **PASS 389/389**（F9 已闭环；与 mock 路径**共用同一份期望值**） |
| **整树 `-Mode prod` 构建** | 8 个完整版组件 | ✅ **exit 0**（`[build] OK -> MeidaP4.bin`，**828,528 B**，app 分区余 80%） |

**构建覆盖的精确口径**（不含糊）：8 个组件全部进入 `build_components` 并各自编出目标文件；
唯一未编译的源文件是 `components/usb_net/usb_net_bridge.c`（**按设计**只在 `CONFIG_APP_USBNET_MODE_BRIDGE=y` 时加入 SRCS，
prod 下 L2 桥接是降级选项）；`mtp_backend` 的另 2 个 `.c` 位于 `test/host/`（**不是组件源码**）。
其余每个组件的每个组件内 `.c` 都出现在 `compile_commands.json` 中（`count_build_coverage.py` 可复核）。

`tools/verify-proto.ps1` exit code = **0**（集成步骤转绿：**F9 已闭环**，见 §7.5）；
`pwsh -File tools/build.ps1 -Mode prod -Slot verify` exit code = **0**。
**两者都是离线结论：离线 PASS ≠ 真机通过**（边界见 §9）。

> ⏱ **快照推进记录**（并发写入环境里必须留痕）：
> - 09:09：`webusb_proto.c`/`usb_vendor_video.c`/`usb_net.c` 三组件编译失败；`verify-proto` exit 1（I1 11 条字符串索引 FAIL）。
> - 09:11:23：前两个组件已编过；`net_hosted/CMakeLists.txt` 已补；新增 **F8**（`usb_net_ncm.c` 不在 SRCS）。
> - 09:14:48：**`verify-proto.ps1` 全绿、exit 0**（F3/F4/F5/F6 已处置，I1 389/389）。
> - 09:15：`usb_net/CMakeLists.txt` 把 `usb_net_ncm.c` 放进 SRCS（**F8 闭环**）；构建只剩 `usb_net.c:116 unterminated #if`。
> - **09:16:51 / 09:17:08：`-Mode prod` 构建 exit 0**（`build.ps1 -Slot verify` 官方入口），8 个组件全部进入构建图并编译。
> - 09:2x：按 ws-net 建议新增**集成步骤 4b**（真 NCM 片段 × 真装配器）→ 立刻抓到 **F9**（`uc_prepare()` 读到悬垂栈内存），`verify-proto` 回到 exit 1（**这是真缺陷，不是噪声**）。
> - **09:39：F9 双向闭环 → `verify-proto.ps1` 全绿、exit 0**（集成步骤 389/389）。Lead 选了我建议的 **(A)**：
>   `usb_composite_desc.c:269-271` 在 **`uc_register()` 内部**深拷（`static usb_function_t s_fn_copy[...]`）；
>   ws-net 同时把 `usb_net_ncm.c:266` 改成 `static`（两种修法下都正确，属"不返工"的改法）。**
>   我的集成桥等价性因此仍然成立**（拷贝发生在 `uc_register()` 内，而桥正是转发到它；已在该文件注释里写明"拷贝若搬到公开层则等价性会悄悄失效"）。
> - 报告主体若与本节冲突，**以带时间戳的小节为准**。

---

## 1. 复现方式（全部一条命令或一行命令）

```powershell
# 主入口：向量完整性 + C codec + JS codec + I1 描述符 + I1 负例
pwsh -File tools/verify-proto.ps1
pwsh -File tools/verify-proto.ps1 -SkipComposite        # 只验 I0
pwsh -File tools/verify-proto.ps1 -HarnessSelftest      # 只验"测试骨架本身"（用临时参考实现，非产品）

# 只重算黄金向量（不引用任何产品代码）
C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe test\host\gen_vectors.py

# 手动跑 C 侧（不依赖验证脚本）
E:\MINGW\bin\gcc.exe -std=c11 -Wall -Wextra -O1 -I components/webusb_proto/include `
    test/host/test_codec.c components/webusb_proto/wu_codec.c -o out.exe ; .\out.exe tools\proto_vectors.json

# 构造覆盖统计（判断"我的组件到底有没有被编译"）
C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe test\host\out\count_build_coverage.py build-verify-prod

# 新鲜构建（写入者交付前必做；必须用**新槽位**或先删旧 build 目录）
pwsh -File tools/build.ps1 -Mode prod -Slot verify
```

产物（全部在 verifier 写作用域内）：`test/host/gen_vectors.py`、`test/host/shim/esp_err.h`、
`test/host/test_codec.c`、`test/host/test_composite_desc.c`、`test/host/run_proto_js.mjs`、
`test/host/harness_selftest/*`（仅用于自检测试骨架）、`tools/proto_vectors.json`、
`tools/verify-proto.ps1`、`test/host/out/*`（构建产物与日志）。

---

## 2. 快照（时间戳 + sha256 前 16 位）—— 结论的"坐标系"

| 文件 | mtime | sha256[0:16] |
|---|---|---|
| `docs/protocol.md` | 08:57:46 | `46DC734A01576E91` |
| `docs/contracts/frozen-allocation.md` | 09:03:42 | `A3A30726F421C79F` |
| `components/webusb_proto/include/wu_codec.h` | 08:54:43 | `ACC44E619970FFCF` |
| `components/webusb_proto/wu_codec.c` | 08:52:45 | `58A0888B9B249939` |
| `web/proto.js` | 09:00:01 | `EECEF6DA28A93E9F` |
| `components/usb_composite/include/usb_composite.h` | 08:45:05 | `622AADCC15D7A146` |
| `components/usb_composite/usb_composite_desc.c` | 09:01:29 | `BF982DF9AD732125` |
| `components/usb_composite/usb_composite_internal.h` | 08:41:55 | `D7135B9A370D7BB3` |
| `tools/proto_vectors.json` | 08:58:00 | `0D20A414D3BCEC15` |
| `test/host/test_codec.c` | 08:55:36 | `A3EE0D3900EAEC13` |
| `test/host/test_composite_desc.c` | 09:02:41 | `70648C4D32792993` |
| `tools/verify-proto.ps1` | 08:58:12 | `5427DB8E5B90F8BD` |
| `components/video_pipe/include/video_pipe.h` | 09:05:13 | `4E5D759B88E1E206` |
| `components/usb_vendor/usb_vendor_video.c` | 09:05:42 | `D411809015438395` |
| `components/usb_net/usb_net.c` | 09:08:34 | `A41734F5756FA6DE` |
| `components/webusb_proto/webusb_proto.c` | 09:08:48 | `29A3DAFDEA3F8BAD` |

> ⚠️ **本报告写完后这些文件仍在被并发修改**。任何"当前 PASS/FAIL"都必须重新取时间戳与 sha。
> 本轮已经出现过一次"同一个文件在两个时刻是两个不同真相"（`wu_codec.c` 的 `resync_bytes` 32 vs 44 讨论）。
> 归档的批 0 证据：`archive/probe-batch0/builds/build-final-b/MeidaP4.elf` sha256
> `89D0EA05372FB20F445CF2CE8B17D5CCCD9764F9337B71D1544B5FEE722D58A5`。

---

## 3. 黄金向量：怎么保证不是循环论证

- 生成器 `test/host/gen_vectors.py` **刻意不 import / 不调用 / 不参照** C 实现与 JS 实现；
  它只按 `docs/protocol.md` 的 §2/§3 自己实现 CRC 与帧封装。
- CRC-16/CCITT-FALSE **两种独立算法对撞**（逐位 / 256 项查表），并与 **CRC catalogue 的第三方 check 值**
  对照：`crc16("123456789") == 0x29B1`、`crc16("") == 0xFFFF`、`crc16(0x00) == 0xE1F0`。
- 生成器内部还断言每个 `frame_hex` 都能被自己的 `pack()` 复现、`crc16` 字段等于复算值，
  避免"手写 hex 打错一个字节"。
- `verify-proto.ps1` 第 1 步**用生成器重算一遍并与仓库文件逐字节比对**：向量被手改即 FAIL。

向量构成（29 个）：`frame` 10 个（逐通道各一 + `length=0` + `length=1024` 两种 + 四个 flag 全置位）、
`crc` 5 个、`stream` 14 个。鲁棒性覆盖：

| 场景 | 向量 | 断言要点 |
|---|---|---|
| 两帧首尾相连 | `stream/two_frames_one_push`、`three_frames_chunk7` | 全部帧解出 |
| 1 字节分片喂入 | `stream/two_frames_bytewise` | 结果与整块喂**完全相同** |
| 垃圾前缀 resync | `stream/resync_garbage(_bytewise)` | 7 字节垃圾（含孤立 `4D`/`50` 陷阱）后仍解出后续帧，`resync_bytes=7` |
| 超缓冲垃圾 | `stream/junk3000_then_frame` | 3000 字节无 magic 垃圾（> 1036 B 缓冲）不崩、后续帧照解 |
| 坏 CRC（CRC 字段） | `stream/crc_field_bad` | 丢该帧 `crc_errors=1`，**不影响后续帧** |
| 坏 CRC（payload） | `stream/crc_payload_bad` | 同上 |
| 坏 CRC 帧内嵌完整帧 | `stream/nested_magic_crc_bad` | 必须**整帧跳过**，不得从帧内重扫出伪帧 |
| 非法 `length=1025` | `stream/length1025_illegal(_bytewise)` | `bad_header=1`、丢候选帧、后续帧照解、`resync_bytes=32` |
| ZLP / 0 字节 push | `stream/zlp_interleaved` + `zlp/noop` | 0 字节 push 不产帧、不推进状态；穿插在真实帧中间不影响解码 |
| 半截候选头 | `stream/partial_header_only`、`frame_then_partial` | 不产帧、不误计 `bad_header` |
| **喂入粒度不变量** | `invariant_group` 三组 | 同一字节流整块喂 / 逐字节喂，`frames`/`crc_errors`/`bad_header`/`resync_bytes` **必须完全相同** |

最后一条是本轮最有价值的断言设计：它**不依赖任何一方对口径的解释**，只要求"诊断计数是字节流的函数"。
正是它抓住了 I0 的真实缺陷（F1）。

---

## 4. I0 线协议：验证结果与缺陷记录

### 4.1 结果

| 断言面 | C 侧（679 条） | JS 侧（345 条） |
|---|---|---|
| `crc16` 命中（含 catalogue check 值） | ✅ | ✅ |
| `encodeFrame`/`wu_encode` 逐字节 == `frame_hex` | ✅ | ✅ |
| 返回长度、cap 不足 `-2`、非法参数 `-1` | ✅ | （JS 侧无此边界断言，见 §7） |
| `decode(encode(x)) == x`（整块 + 逐字节） | ✅ | ✅ |
| 输出越界哨兵（`out[frame_len]` 不得被写） | ✅ | — |
| 全部 14 个鲁棒性向量 + 粒度不变量 | ✅ | ✅ |

### 4.2 F1【已闭环】`resync_bytes` 口径冲突 —— 一条真的被抓出来的实现缺陷

**发现过程（原始证据）**：
1. 我独立生成的向量最初只断言 `resync_bytes >= 1`（口径未定时不越权）。
2. ws-webui 先报冲突，实测 C 侧 `stream/length1025_illegal` 与 `_bytewise` 给出 **0 vs 32**。
3. 我把它升级为**与口径无关**的断言（`invariant_group`），实测：
   ```
   [info] stream/length1025_illegal:  frames=1 crc_errors=0 bad_header=1 resync_bytes=0   （整块喂）
   [info] stream/length1025_bytewise: frames=1 crc_errors=0 bad_header=1 resync_bytes=32  （1 字节喂）
   [FAIL] 不变量/喂入粒度 : [length1025] resync_bytes 依赖喂入粒度：…=0 vs …=32
   ```
   ⇒ **同一字节流给出两个答案**，违反 `wu_codec.c:20-21` 自己声明的口径。
4. 根因：`wu_codec.c` 的"候选帧头非法"分支用 `find_magic` 一次跳过整段并 `count_as_resync=false`；
   而逐字节喂入时，头后那 32 个 `0xAA` 走的是另一条 `count_as_resync=true` 的分支。
5. 裁决与修复：Lead 在**冻结头** `wu_codec.h:148-164` 写入成文裁决
   （候选头 12 B 由 `bad_header` 以**事件**计、不计入 `resync_bytes`；其后垃圾计入；**粒度必须无关**），
   并明确声明"Lead 最初口头估计’候选头也计入、约 44~53’**不作为冻结值**"。
   ws-webusb2 按该口径改 C，ws-webui 同步 JS，我复验：**C 679/679、JS 345/345 全过**。

**我的独立判读（留档）**：我收到过 Lead 一条**与 froze n 头冲突**的口头裁决（"resync_bytes 包含非法候选头本身"⇒ 44）。
我按"**成文且明说口头估计不作冻结值**"的冻结头执行（32），并在向 Lead 的汇报里明确写出了这个冲突。
**这正是并发环境里"结论必须有坐标系"的实例。**
附带更正：ws-webui 转述该向量时为"41×0xaa"，实际是 `12 B 头 + 32 B 0xAA + 16 B 合法帧`，
所以"候选头不计"口径下的正确值是 **32**，不是 41。

### 4.3 F2【verifier 自身的缺陷，诚实披露】测试骨架曾漏算 58 条断言

我最初在 `test_codec.c` 里用**全局失败计数 `g_fail`** 当"向量字段解析失败"的守卫，
导致第一个断言失败后，后续 5 个向量被整体 `[SKIP]`（断言数从 679 掉到 621）。
`assert` 总数变化被我注意到后定位并修复（改用独立的 `g_fields_bad`），并把这条写进报告
—— **验证者自己的 bug 也必须留痕**，否则"621 条 620 通过"这种数字会被别人当真。

---

## 5. I1 复合层：验证结果与缺陷记录

### 5.1 结果（快照 09:09，真实 `usb_composite_desc.c`）

- **原料**：4 个 mock `usb_function_t`（MTP / video / ctrl / NCM），**不硬编码任何接口号/端点号**，
  全部号码只从 `usb_fn_alloc_t` 取；期望值**逐行手抄冻结表**（不向实现者索取）。
- **断言方式**：用 raw bytes 解析装配出的 FS/HS 配置描述符、设备描述符、BOS、MS OS 2.0 集，
  **不信任何导出结构体**（避免"实现自己说自己对"）。

已通过（368 条）：
- §1 5 个接口（IF0 `06/01/01`、IF1/IF2 `FF/00/00`、IF3 `02/0D/00`、IF4 `0A/00/00`），`bNumInterfaces=5`
- §2 9 行端点（**FS 与 HS 分别校验**）：EP1 int 64/64 poll 1、EP2 IN/OUT bulk 512/64、EP3 IN bulk、
  EP4 IN/OUT bulk、EP5 int 16/16 poll 8、EP6 IN/OUT bulk；bulk 的 `bInterval` 必须为 0
- IN 端点唯一、合计 **6 个 ≤ 7**；OUT 3 个
- §2.1 分配表：接口号 0..4、IN 号 EP1..EP6 按注册顺序、OUT 由显式配对表得到 EP2/EP4/EP6、
  `vendor_idx`（video=0 / ctrl=1 / 其余 0xFF）、`itf_str_idx`/`extra_str_idx`
- §3 身份：VID `0x303A`、PID `0x4103` **且不在 esp_tinyusb 自动 PID 空间 `0x4000~0x407F` 内**、
  `bcdUSB≥0x0210`、`EF/02/01`、`iManufacturer/iProduct/iSerialNumber = 1/2/3`、HS qualifier 类字段与 device 一致
- §4 字符串**恰好 8 条**、idx0 = LANGID `0x0409` 小端字节、内容与冻结表一致（除编号见 F3）
- §5 BOS 57 B；WebUSB 平台能力 `iLandingPage=0`、`bVendorCode=0x01`；MS OS 2.0 能力 UUID、
  `dwWindowsVersion=0x06030000`、总长 166、`bVendorCode=0x02`
- §5 MS OS 2.0 集 **166 B** = `0x0A+0x08+0x14+0x80`：Function Subset 指向 **IF2**、
  CompatibleID `WINUSB\0\0` + 8×0、Registry Property = **单数 `DeviceInterfaceGUID` + `REG_SZ(0x0001)`**
  + `{A5DCBF10-6530-11D2-901F-00C04FB951ED}`（UTF-16LE + NUL）、**无 Configuration Subset**、
  **集合里不出现复数 `DeviceInterfaceGUIDs`**（真机 Code 31 的元凶）
- §6 IAD 恰好 1 个、`bFirstInterface=3`、`bInterfaceCount=2`、`02/0D/00`、**紧贴 IF3 之前**
- §2.2 FIFO 预算 ≤ 1024（数值见 F4）
- API 契约：`uc_prepare()` 幂等（重复调用不改变分配）；**prepare 之后 `uc_register()` 必须返回
  `ESP_ERR_INVALID_STATE`**；`uc_selfcheck()` 207 条全过
- **负例**：`ep_in_count=8`（超 `MAX_EP_PER_DIR`）被拒；合法计数但总 IN=13>7 时 `uc_prepare()` 被拒
  （`0x106 用户 IN 端点 13 个，超过 HS 上限 7`）

未通过（11 条）：全部是 §4 字符串索引编号 → F3。

### 5.2 F3【待 Lead 裁决】字符串索引编号与冻结 §4 **及批 0 真机布局**相反

- 实测：`MTP→idx4`、`video/ctrl→idx5`；字符串内容 `[4]='MeidaP4 SD Card'`、`[5]='MeidaP4 WebUSB'`。
- 冻结 §4（Lead 也在消息里重申）：**idx4 = `MeidaP4 WebUSB`（IF1+IF2 共用）、idx5 = `MeidaP4 SD Card`（IF0）**。
- **我独立查了批 0 真机验证过的 ELF**（命令与输出见 §6.1）：`idx4='MeidaP4 WebUSB'`、`idx5='MeidaP4 Storage'`
  ⇒ 真机验证过的编号就是 4=Vendor / 5=MTP，与冻结表一致，**与现在实现相反**。
- 影响：**功能无损**（每个接口仍指向正确内容；Windows 的 MTP 友好名走 DeviceInfo，不靠 iInterface），
  属身份/编号漂移；但 §8 把字符串索引列为冻结项，且相对已验证固件的 diff 变大。
- 建议：装配器按冻结表分配（而不是按注册顺序发号）；或由 Lead 明确改 §4 并同步。
  **我保持按冻结表断言**，并把它作为当前唯一让 `verify-proto.ps1` 变红的原因。

### 5.3 F4【冻结表本身错了】DWC2 FIFO 预算：§2.2 的 852 漏了 EPInfo 的 32 word

我做了**源码级独立复算**（不是照抄 §2.2 的数字）：

```
dwc2_esp32.h:80        P4 HS: ep_count=16, ep_in_count=8, otg_dfifo_depth=1024
esp_tinyusb/tusb_config.h:110   CFG_TUD_DWC2_DMA_ENABLE = 1
tinyusb/tusb_option.h:310       CFG_TUD_DWC2_SLAVE_ENABLE_DEFAULT = !DMA = 0  → 只走 DMA
dcd_dwc2.c:34-35                DMA/SLAVE 都关会 #error ⇒ 设备栈必须走 DMA
dcd_dwc2.c:139                  dma_device_enabled() = DMA && ghwcfg2.arch == INTERNAL_DMA
   → 批 0 真机枚举成功（probe-report §2.1/§2.2）⇒ arch 必为 INTERNAL_DMA ⇒ is_dma = true
dcd_dwc2.c:291-294              is_dma ⇒ dfifo_top = 1024 − 2*ep_count = 1024 − 32 = 992
                                ★ 这 32 word = EPInfo / DMA 元数据（Buffer DMA 每端点方向 2 word）
dcd_dwc2.c:232                  GRXFSIZ = 13+1+2*(512/4+1)+2*16 = 304    （§2.2 这行正确）
dcd_dwc2.c:244                  TX FIFO = ceil(mps/4)：EP0 16 + EP1 16 + EP2/3/4/6 各 128 + EP5 4 = 548
合计 = 32 + 304 + 548 = 884 ≤ 1024；余 = (1024−32−548) − 304 = 444 − 304 = 140
```

- 实现输出 `fifo_words_used=884 / free=140`，**与我的复算一致**；**§2.2 的 852 / 余 172 是错的**。
- 结论方向不变：884 ≤ 1024 可行；开 double buffering 后 `884+4×128=1396 > 1024` **仍不可开**。
- ⚠️ 附带发现：`uc_selfcheck()` 报 207 条全过 —— 它用的是同一个 884，
  **所以"实现自检"抓不到"冻结表数字本身错"这类问题**；只有独立复算能。

### 5.4 F5【I1 契约缺口】片段校验器不允许 alternate setting（会给 WS-net 埋雷）

- CDC-NCM / esp_tinyusb 的标准数据接口形态是 **alt0(0 EP) + alt1(2 EP)**，且**复用同一接口号**
  （`managed_components/espressif__tinyusb/src/device/usbd.h:1095-1102` 的 `TUD_CDC_NCM_DESCRIPTOR`）。
- 而 `usb_composite_desc.c:433-443` 要求片段里每个 Interface Descriptor 的接口号**严格递增**并计入 `itf_count`
  ⇒ 我用标准形态注册直接失败：
  ```
  [FAIL] uc_prepare() = 0x103（ncm: 接口号不连续：片段里是 4，应为 5（分配表 itf[2]=5））
  ```
  我的 mock 只好退成"单个数据接口（alt 0，2 个 EP）"才能继续核对 §1/§2。
- 驱动侧我查过：`ncm_device.c:928-941` 用 while 跳过**任意个** CDC_DATA 接口描述符再开 2 个 bulk EP，
  **不检查 `bAlternateSetting`** ⇒ 单 alt 形态驱动能吃。
- 两条路必须**二选一并写进冻结表**：
  (a) 承认 alt：校验器对 `bAlternateSetting != 0` 不递增 `next_itf`、不计入 `itf_count`（约 3 行）；
  (b) 冻结"IF4 无 alternate setting"，并把"Windows usbncm.sys 是否接受单设置数据接口"列入真机必测。
  **我倾向 (a)**（与 Espressif NCM 示例/真机验证过的形态一致，Windows 侧风险最低）。

### 5.5 F6【实现已给出答案，建议改冻结表】IF4 的 protocol 字段：`0A/00/00` vs CDC-NCM 的 `0A/00/01`

- 冻结 §1 写 IF4 = `0A/00/00`（protocol=0）；CDC-NCM 1.0 Table 4.3 与 esp_tinyusb 默认模板用
  **protocol `0x01`**（`NCM_DATA_PROTOCOL_NETWORK_TRANSFER_BLOCK`，
  `managed_components/espressif__tinyusb/src/class/net/net_device.h:47`）。
- **独立佐证**：WS-net 的实现 `components/usb_net/usb_net_ncm.c:46` 自己就定义了
  `#define CDC_DATA_PROTO_NTB 0x01`（该文件 09:10:29 快照），与规范一致。
- ⇒ **建议：冻结表 §1 的 IF4 改成 `0A/00/01`**（实现是对的），并把"Windows `usbncm.sys` 是否接受"列入上电必测。
  （该格由 WS-net 写片段实现；离线无真机证据。）

---

## 5A. I0 状态机（`ReliableChannel` / `LinkSession`）—— 新增（Lead 2026-10 裁决）

### 5A.1 形状（Lead 裁决：抽成**纯转移函数**，时间参数化）

```
reliableInit() / reliableStep(state, event, nowMs) -> { state, actions }
linkInit()     / linkStep(state, event, nowMs)     -> { state, actions }
```
- **时间只从 `nowMs` 参数进来**：转移函数内部不得读时钟 / 起 timer / 用 `setTimeout`；
  真计时器接线只留在外面一层"薄驱动"里。
- 我**没有**用真计时器做向量 —— 那要么得注入假时间、要么会把"实现当前行为"当成真值编码进向量（= 循环论证）。

### 5A.2 交付物

| 文件 | 内容 |
|---|---|
| `test/host/gen_sm_vectors.py` | 向量生成器（按 `docs/protocol.md §6.1/§6.3/§6.5` 逐条推导，**不引用任何实现**；`verify-proto.ps1` 会重算比对防手改） |
| `test/host/sm_vectors.json` | **10 个**向量（reliable 5 + link 5），含状态字段/事件/动作词表（`interface` 段）与每条的规约出处 |
| `test/host/run_proto_sm.mjs` | 断言器（`actions` **子集匹配** + state 的 `eq/min/max` 边界；**只锁规约写死的事实**） |
| `test/host/harness_selftest/ref_sm_for_harness_selftest.mjs` | 仅用于自检"我的向量没写错"的临时参考实现（不是产品代码，默认不参与验证） |

覆盖：窗口 ≤ 8（不变量）、累计 ACK 释放、199 ms 不重传 / 200 ms 重传且置 `RETX`、
连续 5 次失败 → `link_down` + 清空队列、重复 seq 幂等（回 ACK 不重复投递）、
HELLO 每 2 s 直至 `HELLO_ACK`、`HELLO_ACK` 收敛并归零 seq、有效帧把 3 s 掉线推后、
**0 字节读不算有效帧**（把 ZLP 当"链路活动"的实现会永远不掉线）、引导帧绕开 seq 门。

### 5A.3 当前状态与边界

| 项 | 状态 |
|---|---|
| 向量 + 断言器 | ✅ 就绪，并用参考实现自检通过（**23/23**，1 条按 `requires_spec` 跳过） |
| 真实 `web/proto.js` | ⏳ **SKIP —— 接口未就绪**：尚未导出 `reliableInit/reliableStep/linkInit/linkStep`。`verify-proto.ps1` 把这一步记为 **SKIP**（不计 FAIL），并打印所需接口与词表；**同时意味着"状态机语义"目前未被验证** |
| **计时器真实接线**（timer 精度、浏览器节流、后台标签页降频） | ❌ **不做向量**，列入 §9 的 D 清单（离线不可判定） |

### 5A.4 🔴 规约缺口：Lead 指令引用的 **§6.5⑥** 在冻结文档里**不存在**

- Lead 的裁决要求把"**§6.5⑥ 的 seq 门豁免**"纳入向量，并称之为"链路永不收敛的那个漏洞的唯一回归防线"。
- 实测：`docs/protocol.md`（sha `46DC734A01576E91`，08:57:46）的 **§6.5 只有 ①–⑤**，
  全文检索 `⑥ / 豁免 / seq 门` 在**任何 .md 里都没有命中**。
- 但**两侧实现都已经做了这条**：
  `components/webusb_proto/webusb_proto.c:27-28`（"引导帧必须绕开 seq 门……两侧都实现了这条豁免"）、
  `:693`（"引导帧绕开 seq 门（§6.3 + §6.5④）"）、`web/proto.js:784` 与 `:884`（同款注释）。
  ⇒ 它现在是**实现惯例，不是冻结规约**。这正是"同一套协议被两侧各实现一次"最危险的形态：
  将来任一侧按文档重写，这条豁免就会消失，而**文档不会提醒他**。
- 我的处置：向量 `link/bootstrap_frames_bypass_seq_gate` 已写好（可观察结论 = 引导期
  `HELLO(seq=0)` 之后主机 `HELLO_ACK(seq=0)` 仍必须被接受并收敛到 `link_up`），
  但标了 `requires_spec`，在规约补上之前 runner 会 **SKIP 它**（不假装验证过）。
- **建议 Lead 在 §6.5 补一条 ⑥**（可直接采用实现里的措辞），我随后把 `requires_spec` 去掉即转为硬断言。

---

## 6. 我独立复核的两条"别人给的结论"

### 6.1 probe-report §5 **U8** 的更正 —— 我判定成立，但原证据行是过期固件

命令（逐槽位，我自己跑的）：
```
C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe tools\desc_lint.py --mode b --elf archive\probe-batch0\builds\build-<slot>-b\MeidaP4.elf
```

| 归档槽位 | `s_msos20_desc_set` 实际字节 | 子描述符序列 | desc_lint 汇总 |
|---|---|---|---|
| `build-diag-b` | **38** | `0x0000→0x0002→0x0003`（**无 RegProp**） | 118/122 |
| `build-diagfix-b` | **38** | 同上 | 119/122 |
| `build-main-b` | **38** | 同上 | 118/122 |
| `build-usbcore-b` / `build-final-b` / `build-verify-b` | **166** | `0x0000→0x0002→0x0003→0x0004` ✅ | **135/135 PASS** |

- 166 B 逐字节（`build-final-b`，desc_lint 原样输出，我留档）：
  `0A 00 00 00|00 00|03 06 A6 00` + `08 00|02 00|01 00|9C 00` + `14 00|03 00|"WINUSB\0\0"|8×00`
  + `80 00|04 00|01 00|28 00|"DeviceInterfaceGUID"(UTF-16,40B)|4E 00|"{A5DCBF10-…}"+NUL(UTF-16,78B)`；
  **无 0x0001 Configuration Subset**。
- ⇒ **§5.1/§5.2 的"166 B 形态已真机验证"成立**（final-b/verify-b 支撑），`CONFIG_USB_COMPOSITE_MSOS_CONFIG_SUBSET` 默认 n 与已验证形态一致。
- ⚠️ **但 probe-report §2.2 引用的串口日志来自 diag-b**（`MSOS20=38 B` + `C0 02 00 00 07 00 26 00`），
  那个固件**根本没有 Registry Property** ⇒ §2.2 的"证据"与 §2.5 的"RegProp 生效"出自两个不同固件。
  这不是代码缺陷，是**报告的证据时效性缺陷**（已报 Lead）。

### 6.2 "IDF 只构建从 main 可达的组件" —— 我推翻了一半，并发现更严重的问题

- **推翻的部分**：Lead 08:48 的归因是"main 没有 require → 组件没被编译"。实测
  `build-leadfix-prod`(08:51) / `build-webusb-prod`(08:55) 里 `usb_composite` **有 2 条**、
  `sd_storage`/`mtp_backend`/`video_pipe` 各 **1 条** —— **修法当时已生效**。
  那 3 个 0 条目（`webusb_proto`/`usb_vendor`/`usb_net`）是因为**它们的 CMakeLists 在 08:58~09:04 才出现**，
  旧 build 目录里根本没有它们的构建图。
  ⇒ **真问题是"写入者拿过期 build 目录当证据"**，而不是"main 不 require"。
- **更严重的部分（F7）**：我用**新鲜槽位**（`build-verify-prod`，我自己的槽位）重新 configure+build，
  8 个组件**全部进入构建图**（`net_hosted` 除外，见下），然后**编译失败** —— 见 F7。

---

## 7. F7【阻塞】整树 `-Mode prod` 构建失败（3 组件 / 3 写入者）

复现：`pwsh -File tools/build.ps1 -Mode prod -Slot verify` → `[build] FAILED (exit 2)`；
`. .\tools\idf-env.ps1 -Quiet ; ninja -C build-verify-prod -k 0` 收集全部错误。
原始日志：`C:\Users\30407\AppData\Local\Temp\meidap4-build-verify-prod.log`（快照时刻的副本
另存于 `test/host/out/`）。

```
FAILED esp-idf/webusb_proto/.../webusb_proto.c.obj
  webusb_proto.c:614:12: error: 'webusb_proto_stats_t' has no member named 'helloSent'
FAILED esp-idf/usb_vendor/.../usb_vendor_video.c.obj
  usb_vendor_video.c:47/48/138/239: error: unknown type name 'wu_frame_t'
  usb_vendor_video.c:203/261: error: address of 'video_pipe_get_frame'/'video_pipe_release_frame'
                                    will never be NULL [-Werror=address]
  usb_vendor_video.c:243/262: error: passing argument 1 of 'video_pipe_get_frame' from incompatible pointer type
FAILED esp-idf/usb_net/.../usb_net.c.obj
  usb_net.c:171/176/505/700: error: 'CONFIG_APP_USBNET_TX_LOCK_MS' / '..._TX_TIMEOUT_MS' /
                                    '..._DHCP_POOL_SIZE' / '..._DHCP_LEASE_S' undeclared
  usb_net.c:332/704: error: 'DHCPS_OFFER_DNS' undeclared
  usb_net.c:687: error: unknown type name 'dhcps_lease_t'
```

覆盖统计（`count_build_coverage.py build-verify-prod`，快照）：
`usb_composite 2/2`、`webusb_proto 2/2`、`usb_vendor 2/3`、`video_pipe 1/1`、`usb_net 1/1`、`sd_storage 1/1`、
`mtp_backend 1/1`、**`net_hosted 0/1 ★源文件存在但未被编译`**（该目录**没有 CMakeLists.txt**，不是组件）。

### 7.1 判读（含 Lead 要求的两个正面/负面留档）

1. **"此前给出的 PASS 是过期证据"**：这些 build 目录里根本没有相应组件的构建图，
   0 条目 ⇒ 写入者以为编译过了。**这是本轮最重要的一条系统性风险**：证据有时效性。
2. **正面案例（`-Werror=address`）**：`usb_vendor_video.c:203/261` 的"弱符号恒非 NULL"写法被
   `-Werror` 在**编译期**拦住。IDF 对工程组件默认 `-Wall -Werror -Wextra`，
   而本工程已经踩过一次弱符号陷阱（`probe-report §3.2` 的 esp_tinyusb MTP 强符号未链接）。
   **编译期拦住"会掩盖真实缺陷的写法"是好事**，值得作为正面案例留档。
3. **I2 契约两侧曾经对不上**（新发现）：`video_pipe.h:107` 是
   `esp_err_t video_pipe_get_frame(video_frame_t *out, uint32_t timeout_ms)`（out 参数 + 错误码），
   而 `usb_vendor_video.c:243` 用 `wu_frame_t f; video_pipe_get_frame(&f, 20)` 并访问
   `f.data/f.len/f.fmt/...`。Lead 已裁决：**类型名 `video_frame_t`、形态以 `video_pipe.h` 为准（out 参数 + 返回错误码）、
   成员名以 `video_pipe.h` 为准**；`wu_frame_t` 只属于 I0。
4. **构建纪律（Lead 已采纳为硬门槛）**：交付前必须用**新槽位**（或删旧 build 目录）做一次新鲜 configure，
   并用 `count_build_coverage.py` 证明自己的源文件出现在 `compile_commands.json`；
   **0 条目 = 没编译 = PASS 无效**。

> 说明：这三处错误属于**并发进行中的中间态**（写入者仍在改），我**不**把它们记为写入者的最终失败；
> 我记录的是"此前那些 PASS 是过期证据"这一事实。

### 7.1 更新快照（09:11:23）：F7 降到 2 处，并新增 F8

```
AT 2026-10-17 09:11:23
webusb_proto 编译=2 源文件=2 OK     （09:09 已修好）
usb_vendor   编译=3 源文件=3 OK     （09:09 已修好）
net_hosted/CMakeLists.txt  出现于 09:09:29  → 我上一条报的"不是组件"已闭环
仍失败：
  usb_net.c:710/711   error: incompatible types when assigning to 'ip4_addr_t' from 'esp_ip4_addr_t'
  net_hosted.c:158    error: comparison of unsigned expression in '>= 0' is always true [-Werror=type-limits]
  net_hosted.c:417    error: 'CONFIG_NET_HOSTED_WPA3_SAE' undeclared (did you mean 'CONFIG_NET_HOSTED_WIFI_SSID'?)
```
（`main/CMakeLists.txt` 08:48 的动态 require 在新鲜 configure 下**有效**：8 个完整版组件全部进入构建图。）

### 7.2 F8【新，严重】`usb_net_ncm.c` 不在 SRCS 里 —— **新鲜构建 PASS 也仍然是假绿**

```
components/usb_net/CMakeLists.txt:6-7    SRCS
                                             "usb_net.c"          ← 只有这一个
components/usb_net/usb_net_ncm.c         12,562 B  09:10:29      ← 存在，但没进构建
count_build_coverage.py build-verify-prod
    usb_net   编译=1  源文件=2  OK                                ← 2 个源文件只编了 1 个
```
而 `usb_net_ncm.c` 是**唯一**做 NCM 的 I1 注册的文件
（`:306 usb_composite_register(&fn)`、`:265 use_iad=true`、`:288 ep_out_same_num_as_in[0]=1` →
OUT 复用 `ep_in_req[1]` 的号 = EP6、`:263/:264` itf_string/extra_string）。

⇒ **后果**：NCM 永不注册 → 固件里没有 IF3/IF4 → 冻结 §1 的"5 个接口"在固件里只实现 3 个，
而 `-Mode prod` 构建**照样 exit 0**。

这条比 F7 更重要，因为它说明纪律要更严：**"我的组件在 compile_commands 里有条目"不够，
必须"我的组件下每个 .c 都在 SRCS 里、且都出现在 compile_commands 里"**。
`count_build_coverage.py` 的 `编译=N 源文件=M` 正好能暴露它（N<M 即问题）。

**因果链提示**：`usb_net_ncm.c:179-180` 发的正是**两套 alternate setting**（alt0 = 0 EP / alt1 = 2 EP，复用同一接口号）
⇒ F8 一旦修好，**F5 会立刻在运行期炸**（`uc_prepare() = 0x103 接口号不连续` → 复合层起不来、设备枚举失败），
而编译期完全看不到。**两条必须一起裁决/一起改**，否则会"修好一个、炸出另一个"。


---

### 7.3 关闭记录（09:14–09:15 快照）

| 项 | 处置 | 我的复验 |
|---|---|---|
| **F3** 字符串索引 | Lead **改文档**（不是改实现）：§4 更正为按注册顺序 `4=MeidaP4 SD Card`(IF0) / `5=MeidaP4 WebUSB`(IF1+IF2 共享) / `6=Network` / `7=MAC`，并写明"序号数值对 USB/Windows 无意义，有意义的是内容↔接口映射" | 我把断言改到**新表** → `test_composite_desc` 389/389 PASS |
| **F4** FIFO 852/172 | Lead 采纳我的源码级复算 → §2.2 改为 **884 / 余 140** | 断言恒为 884/140 → PASS |
| **F5** alt setting | Lead **批准允许 alt != 0**，并改了 `validate_fragment()`+`selfcheck()`（`alt==0` 才算新接口、`alt!=0` 复用前一号且不计入 `bNumInterfaces`）；文档新增 **§1.1**（NCM 数据接口**必须**两套 alt） | 我把 mock 改回**标准双 alt 形态**（alt0 = 0 EP / alt1 = 2 EP，复用 IF4）→ 389/389 PASS；并新增断言"alt != 0 必须复用前一个接口号""最大接口号 == 4" |
| **F6** IF4 protocol | Lead 采纳 → §1 改为 **`0A/00/01`**（Network Transfer Block），与 CDC-NCM 1.0 及 esp_tinyusb 默认模板一致 | 断言改为 protocol=0x01 → PASS |
| **F8** `usb_net_ncm.c` 不在 SRCS | WS-net 已改 `usb_net/CMakeLists.txt:6`（`set(usb_net_srcs "usb_net.c" "usb_net_ncm.c")`）；Lead 计划在 `main/CMakeLists.txt` 加 configure 期"SRCS 必须覆盖组件下所有 .c"检查 | `count_build_coverage.py` 现在显示 `usb_net 编译=2`（含 `usb_net_ncm.c`）→ **闭环** |
| **F7** 构建失败 | `webusb_proto`/`usb_vendor` 于 09:09 修好；`net_hosted`（2 处）与 `usb_net.c`（类型不兼容）在 09:13–09:15 迭代中修完 | ✅ **09:17:08 官方入口 `build.ps1 -Mode prod -Slot verify` exit 0**，`[build] OK -> MeidaP4.bin` 828,528 B；8 个组件全部进入构建图并编译 |

> 说明：F1–F6 的处置**我全部独立复验过**（不是采信汇报）：I0 侧 679+345 全过、I1 侧 389 全过。
> F8 由覆盖统计闭环（`编译=2` 而不是 `编译=1 源文件=2`）。

### 7.4 一条文档卫生问题（建议 Lead 顺手清理）

`docs/contracts/frozen-allocation.md` 的 **§2.2 代码块本身**（约 121-132 行）**仍然写着**
`合计 → 852 word ≤ 1024 余 172 word (672 B)`，而该节上方已加了"漏掉 EPInfo、正确值 884/140"的更正注记。
两处并存时，后来者极可能只抄代码块。**建议直接改代码块里的数字**（把 EPInfo 32 word 一行补进去），
不要只留更正注记。
> ✅ **09:2x 更新**：Lead 已回复确认并改掉了代码块里的数字（"我此前只加了注记、没改数字，是我的漏"）。

### 7.5 F9【新，高】集成路径抓到的**结构体生命周期 UB** —— mock 测试看不见的那一类

**来源**：ws-net 主动建议（"没有任何地方在执行'我真实的 `usb_net_ncm.c` 片段 × 真实装配器/校验器'"）。
我据此新增集成步骤 **4b**：把 mock NCM 换成 **ws-net 真实的 `usb_net_ncm_register()`**，
其余 3 个 mock 与**全部期望值不变**（不给真片段开后门）。实现见 `test/host/real_ncm_bridge.c` 与
`test_composite_desc.c` 的 `-DUSE_REAL_NCM=1` 分支。

**现象**（一条命令：`pwsh -File tools/verify-proto.ps1`）：
```
test_composite_desc(真实NCM)  FAIL   6 条断言，5 通过 1 失败
[FAIL] uc_prepare() = 0x106（NCM: IN 端点号耗尽（HS 上限 16，含 EP0 用户可用 7））
```
本次只注册 **4 个功能 / 6 个 IN 端点**（MTP 2 + video 1 + ctrl 1 + NCM 2）⇒ 字段完好时**不可能**报"端点耗尽"。

**根因（两处合起来才成立）**：
| # | 位置 | 事实 |
|---|---|---|
| 1 | `components/usb_composite/usb_composite_desc.c:260` | `s.fn[s.fn_count] = fn;` —— 注册表**只保存调用方指针，不复制结构体** |
| 2 | `components/usb_net/usb_net_ncm.c:255` | `usb_function_t fn;` 是**栈上局部**；`:306` 注册、`:311` 返回 ⇒ 指针立即失效 |

`uc_prepare()` 在**另一层调用栈**上解引用它 ⇒ 读被栈复用覆写的内存（UB）。
输出里仍能认出 `NCM`，因为 `name` 在结构体偏移 0 未被覆写、而 `ep_in_count` 在更后面被覆写 —— **悬垂栈内存的典型指纹**。
⚠️ 更危险的一面：prepare 阶段还要读 `fn->build_desc`（**函数指针**）；本次侥幸先在端点分配处报错，否则是跳转到随机地址。

**为什么之前所有测试都漏掉它**：
| 测试 | 为什么看不见 |
|---|---|
| IDF `-Mode prod` 构建 | 编译期完全合法（类型正确、无 warning） |
| ws-net 的字节级 host 测试 | 直接调片段生成函数，**从不经过注册表** |
| 我的 mock 路径（§5.1 的 389 条） | mock 是 `static const` ⇒ 指针常驻，**恰好绕过**生命周期问题 |
| **集成路径（新增 4b）** | ✅ **唯一**会走到"真注册 → prepare 解引用"的路径 |
| 对照：`usb_vendor_fn_video/ctrl` 是 `const usb_function_t` 全局、`mtp_backend` 是 `static` | 它们**恰好**安全 ⇒ 说明这是"没被文档约束的隐式约定"，不是"大家都写对了" |

**建议（已报 Lead，等他裁决，因为 `usb_composite.h` 是冻结件）**：
- **(A) 让 I1 复制结构体**（我推荐）：`uc_register()` 拷进 `static usb_function_t s_fn_copy[USB_COMPOSITE_MAX_FUNCTIONS]`，
  `uc_fn(i)` 返回副本地址。签名不变（不破 §8-6），代价 ~1 KB 静态内存，**永久消除该坑**；
  并在 `usb_composite.h` 补一句"复合层会复制整份结构；回调与字符串指针仍需常驻"。
- (B) 只写文档：`usb_composite.h` 明确"`usb_function_t` 必须活到 prepare 之后（static/全局）"，
  由 ws-net 把 `:255` 改成 `static usb_function_t fn;`。

**方法论收获**：这是本会话第 4 类"离线能抓到、但只有特定路径能抓到"的问题 ——
前 3 类是**假绿**（证据过期 / SRCS 半落地 / 跨配置），这一类是 **mock 替身掩盖了真实交互**。
共同教训：**"我测过了"必须同时说明"在哪个路径上测的"**。

**闭环记录（09:39）**：
- Lead 采纳 **(A)**：`usb_composite_desc.c:269-271` 在 **`uc_register()` 内部**深拷
  （`static usb_function_t s_fn_copy[USB_COMPOSITE_MAX_FUNCTIONS]; s_fn_copy[n] = *fn; s.fn[n] = &s_fn_copy[n];`）。
- ws-net 同时把 `usb_net_ncm.c:266` 改为 `static usb_function_t fn;`（在 (A)/(B) 两种裁决下都正确，属"不返工"改法）。
- **复验**：集成步骤 `test_composite_desc(真实NCM)` → **PASS 389/389**；`verify-proto.ps1` → **exit 0**；
  真实 NCM 片段现在**实执行**通过了 §1.1 的 alt=1 复用接口号路径、IAD(3/2/02-0D-00)、
  字符串恰好 8 条、FIFO 884、5 接口 / NCM 段 85 B。
- ⚠️ **留给后人的等价性条件**：我的集成桥是"公开入口 → `uc_register()`"的直接转发，
  因此 **F9 的拷贝必须留在 `uc_register()` 里**。若将来把拷贝搬到公开层（`usb_composite.c`），
  桥会绕过它、集成测试的等价性**悄悄失效** —— 这条已写进 `test/host/real_ncm_bridge.c` 的注释。
- 另：ws-net 指出字符串指针（`name`/`itf_string`/`extra_string`）在深拷结构体后**仍需调用方保证长期有效**，
  所以 (A) 只解决一半；更彻底是 I1 内部 `strdup` 深拷字符串（代价几行 heap）。**这一点我已转达 Lead 作为可选后续。**



> ⚠️ **归因规则（本轮踩过的坑）**：我最初按"任务映射"把 `webusb_proto.c` / `usb_vendor/**` 记在 ws-webusb2 名下，
> ws-webusb2 举证反驳（他的 8:54/8:56 两个文件已被 09:03–09:09 的新版本**覆盖**，且他从未创建 `usb_vendor/`），
> 文件 mtime 支持他。**因此下表按"文件 + mtime"归因，不按团队映射归因。**
> 在并发写入环境里，**归属也要靠证据**。

|  文件（证据） | 发现 | 严重度 | 证据 |
|---|---|---|---|
| **ws-storage**（`components/sd_storage/**`、`components/mtp_backend/**`） | **`CONFIG_SD_STORAGE_FORMAT_IF_MOUNT_FAILED` 默认 `y`，且只以 `ESP_FAIL` 为门控 → 有数据丢失风险**：IDF 在 `FR_NO_FILESYSTEM` **和 `FR_INT_ERR`** 两种情况下都返回 `ESP_FAIL`（`sd_storage.c:250` 自己的注释），而 `FR_INT_ERR` = FAT 内部错误（可修复的坏卡），此时会**静默 `mkfs` 用户的 SD 卡**，且每次开机重试。建议：prod 默认改 `n`，或先做"读引导扇区判断无 FS"再决定；并把"插一张留有用数据的卡"列入真机必测。 | 中（数据丢失） | `components/sd_storage/Kconfig:110-116`、`sd_storage.c:296-334` |
| **ws-net**（`components/usb_net/usb_net_ncm.c`、`usb_net.c`、`net_hosted/*`） | ①**F9（最高）**：`usb_net_ncm.c:255` 用**栈上局部** `usb_function_t fn` 注册，而 I1 的注册表（`usb_composite_desc.c:260`）**只存指针不复制** ⇒ `uc_prepare()` 读到悬垂栈内存（UB）；实测集成路径直接失败（`0x106 NCM: IN 端点号耗尽`，而当时只有 4 功能/6 IN）。**这是本会话唯一一条"会让真机崩或行为随机"的发现。** ②**F8**：`usb_net_ncm.c` **不在 SRCS** → 永不编译 → 固件没有 IF3/IF4 而构建 PASS；③F5 因果链：该文件发两套 alternate setting，修好 ② 后会被 I1 校验器拒绝（`uc_prepare()=0x103`）；④`usb_net.c:710/711` `ip4_addr_t` vs `esp_ip4_addr_t`；⑤`net_hosted.c:158` `-Werror=type-limits`、`:417` `CONFIG_NET_HOSTED_WPA3_SAE` 未定义。**已完成**：补 `net_hosted/CMakeLists.txt`（09:09:29）、`CONFIG_APP_USBNET_*` Kconfig、`usb_net_ncm.c` 进 SRCS、4 处编译错误。**未闭环：F9**。 | 高（UB/崩溃 + 静默功能缺失 + 构建阻塞） | `test/host/real_ncm_bridge.c` 集成路径输出；`usb_composite_desc.c:260`；`usb_net_ncm.c:255/306/311` |
| **ws-webusb**（`components/webusb_proto/webusb_proto.c` 09:09:10、`components/usb_vendor/*` 09:03–09:09） | 09:09 快照：`webusb_proto.c:614` 用了结构体不存在的字段 `helloSent`；`usb_vendor_video.c` 用不存在的类型名 `wu_frame_t`（改名前）/ 与 `video_pipe.h` 的 I2 形态不一致；弱符号判空触发 `-Werror=address`。**09:11 快照：`webusb_proto`(2/2) 与 `usb_vendor`(3/3) 均已编译通过 ⇒ 前两项已被修复，F7 不构成最终失败。** | 中（曾阻塞，已自愈） | §7、§7.1；`test_codec` PASS 输出 |
| **ws-webui**（`web/proto.js` 09:00:01） | **未发现功能缺陷**：`MAGIC/crc16/encodeFrame/FrameDecoder/ReliableChannel/LinkSession` 全部存在，29 个向量 **345/345 通过**（含 resync、坏 CRC、length=1025、ZLP、粒度不变量）。**一条覆盖缺口**：`ReliableChannel` 的窗口 W=8 / 200 ms 重传 / 5 次失败判死、`LinkSession` 的 HELLO-HELLO_ACK 状态机与 3 s `link_down` **离线没有任何向量覆盖**（`protocol.md §6` 只规定语义、没给可判定向量）；我目前只断言"类存在且可构造"。这条要么补黄金向量（我出）、要么列入真机/集成测试。另外他的 stats 未暴露 `badHeader`（`protocol.md §7` 未强制）⇒ 14 条 SKIP。 | 低—中（覆盖缺口） | `run_proto_js.mjs` 输出；`protocol.md §7` |

补充（**verifier 自曝**）：F2 —— 我自己的测试骨架曾用全局失败计数当守卫，漏算 58 条断言；已修。
补充（**verifier 归因错误**）：我曾在汇报里把两个文件错记到 ws-webusb2 名下；已按文件 + mtime 更正。

### 8.1 本轮总结出的两类"假绿"（都带实例，建议写进纪律）

| 类型 | 机制 | 实例 | 检测手段 |
|---|---|---|---|
| **A · 证据过期** | 写入者拿**旧 build 目录**当证据，而该目录的构建图里根本没有他的组件（CMakeLists 晚于 configure 出现） | `build-leadfix-prod`(08:51) / `build-webusb-prod`(08:55) 里 `webusb_proto`/`usb_vendor`/`usb_net` 条目数 **0** | 交付前用**新槽位**重新 configure；`count_build_coverage.py` |
| **B · SRCS 半落地** | 组件进了构建图、旧文件也编过，但**新加的 .c 没写进 SRCS** → 关键功能（这里是 NCM 注册）永不编译，而构建 exit 0 | `usb_net_ncm.c`（F8，已闭环） | `count_build_coverage.py` 的 `编译=N 源文件=M`（N<M 即问题） |
| **C · 跨配置/跨模式假绿** | 代码在**某个 Kconfig 组合下**才编译，只在 prod 跑构建就发现不了；换 mode/关掉某功能立刻编不过 | WS-net 自查（09:1x）：`CONFIG_NET_HOSTED_WPA3_SAE=n` 时宏被当 C 表达式用；`(int)CONFIG_LWIP_IPV6`；NCM=n 时 `usb_net_usb_rx_cb`/`tx_free_cb` 无引用点触发 `-Werror=unused-function`、`buffer` 未用触发 `-Wunused-parameter` | **每个交付的组件至少跑两种配置**（prod + 至少一个"功能关掉"的模式）；本工程 `-Wall -Werror -Wextra` 会把它们全变成硬错误 |
| **D · mock 替身掩盖真实交互** | 单元测试用**自己的替身**（mock 功能 / 静态结构体）验证框架，而真实调用方的写法不同（栈局部变量、生命周期、注册顺序） | **F9**：我的 mock 是 `static const` ⇒ 绕过生命周期问题；ws-net 的真实注册用栈局部 ⇒ 集成路径立刻 UB | 对**别人的真实产物**再跑一遍同一份期望值（`-DUSE_REAL_NCM=1` 集成步骤）；"我测过了"必须说明**在哪个路径上测的** |


---

## 9. 边界声明：离线**能**证明什么 / **不能**证明什么

### 能证明（本报告的适用范围）

1. C 与 JS 两套 I0 编解码器与 `docs/protocol.md` **逐字节一致**（29 个向量、含 11 类鲁棒性场景）。
2. I1 装配器输出的描述符字节与 `docs/contracts/frozen-allocation.md` §1/§2/§3/§4/§5/§6 一致
   （除字符串编号 F3）；端点号/方向/类型/`wMaxPacketSize`/`bInterval` 在 **FS 与 HS 分别**成立；IN ≤ 7。
3. MS OS 2.0 集是**与批 0 真机验证过的 166 B 形态逐字节相同**的形状
   （单数 `DeviceInterfaceGUID` + `REG_SZ`、CompatibleID、Function Subset 指向 IF2、无 Config Subset）。
4. §2.2 的 FIFO 预算在**源码级独立复算**下为 884 ≤ 1024（且 double buffering 仍不可开）。
5. "超预算/非法计数必须被拒绝"这条防御性行为成立（负例 2/2）。
6. 8 个完整版组件中，7 个能进入构建图；其中 4 个可编译，3 个当前编译失败（F7）。

### **不能**证明（必须上真机 / 集成测试）

| # | 项 | 为什么离线不能证 |
|---|---|---|
| D1 | USB 枚举成立（HS、`bcdUSB`、BOS 被取） | 需要主机真实读描述符 |
| D2 | Windows 驱动绑定：`usbccgp`/`wpdmtp.sys`/`winusb.sys`/`usbncm.sys` | MS OS 2.0 属性是否被应用取决于 Windows 实例与注册表 |
| D3 | Chrome `requestDevice` + `claimInterface` | 依赖 Windows device interface 注册（批 0 §3.1 的教训） |
| D4 | DWC2 的实际 FIFO 分配与 `dfifo_alloc()` 是否成功 | 需要读寄存器/看串口日志；884 是**静态推导** |
| D5 | ZLP 的真实发生频率与 `zeroLenReads` 计数 | 需要真实 IN 传输 |
| D6 | NCM 上网（DHCP/NAPT/DNS 透传/Windows NCM 驱动） | 需要 PC 与上游 AP；且见 F5/F6 两条未定口径 |
| D7 | MTP 浏览 SD 卡（且 P4 侧 `fopen` 同一文件） | 需要真机 + 卡；另见 ws-storage 的格式化数据丢失风险 |
| D8 | 供电与热稳定（总线 500 mA 是否够 Wi-Fi 峰值） | probe-report §5 U6，本阶段无法测 |
| D9 | `bm_double_buffered` 相关推论（1396 > 1024） | 静态推导；真机只需确认不开 |
| D10 | `ReliableChannel`/`LinkSession` 的窗口/重传/重连语义 | 目前没有可判定向量（见 §8 ws-webui 行） |
| D11 | F5/F6 两条 NCM 口径（alt setting、IF4 protocol） | 无真机证据，需 Lead 裁决 + 上电复测 |

---

## 10. 遗留问题（按优先级）

> ✅ **已闭环**（09:14/09:15/09:2x 快照，我全部独立复验过）：F1、F3、F4、F5、F6、F8。
> ⏳ **仍在推进**：**F9**（结构体生命周期 UB，等 Lead 裁决修法 A/B）、F7（构建已转绿，见 §7.3）、F2（我自己的骨架 bug，已修留档）。

| # | 项 | 需要谁 | 备注 |
|---|---|---|---|
| **1** | **F9 修法裁决**：I1 复制结构体（A，推荐）or 文档化生命周期 + `static usb_function_t fn`（B） | **Lead** | 修完后集成步骤 4b 会自动转为 PASS；在此之前 `verify-proto.ps1` 保持 **exit 1** |
| 2 | **§6.5⑥（引导帧绕开 seq 门）补进冻结文档** | Lead | 两侧实现都已做、文档没写；补上后我的 `link/bootstrap_frames_bypass_seq_gate` 向量从 SKIP 转硬断言（见 §5A.4） |
| 3 | **`web/proto.js` 补 `reliableStep`/`linkStep`** | ws-webui | 向量与断言器已就绪（自检 23/23），接口一到位即自动断言（见 §5A） |
| 4 | **ws-storage 的格式化数据丢失风险**（`SD_STORAGE_FORMAT_IF_MOUNT_FAILED` 默认 y、`ESP_FAIL` 同时覆盖 `FR_INT_ERR`） | WS-storage | 建议 prod 默认 n + 真机"插带数据的卡"必测 |
| 5 | **`-NoComponentManager` 已废弃**，但 `frozen-allocation §7` 仍要求写入者加 | Lead | `tools/build.ps1:80` 明确 `throw`；冻结表那一段要更新 |
| 6 | **probe-report §2.2 的证据时效性**（引用的是 pre-fix `diag-b` 固件） | Lead | 建议标注 pre-fix 或换成 `final-b` 证据 |
| 7 | **跨配置验证**（假绿 C 类） | 所有写入者 | 每个组件至少跑 prod + 一个"功能关掉"的模式 |
| 8 | **`CONFIG_PROD_STRICT_COMPONENTS=y`** + configure 期"SRCS 必须覆盖组件下所有 .c"检查 | Lead | Lead 已承诺下一轮落盘（F8 的结构性防线） |

---

## 11. 复验清单（任何人可照做）

```powershell
# 1) 契约面（本报告核心）
pwsh -File tools/verify-proto.ps1                       # 期望：I0 全 PASS；I1 只剩 F3 的 11 条
# 2) 向量没有被手改
C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe test\host\gen_vectors.py   # 输出 sha 与仓库文件比对
# 3) 构建覆盖（交付硬门槛）
pwsh -File tools/build.ps1 -Mode prod -Slot verify
C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe test\host\out\count_build_coverage.py build-verify-prod
# 4) 批 0 归档描述的独立复核（U8）
C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe tools\desc_lint.py --mode b `
    --elf archive\probe-batch0\builds\build-final-b\MeidaP4.elf
```

**最后重申**：以上全部是离线结论。**设备未连接，离线 PASS ≠ 真机通过。**
真机项见 `docs/device-test-requests.md`（若该文件尚未列出本报告 §9 的 D1~D11，请以本报告为准合并）。
