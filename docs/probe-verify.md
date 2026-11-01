# 探测阶段 · 独立验证报告（批 0 / 冒烟 C）— 终局版

> 生产者：**verifier**（task-5 首版 → task-9 同步 → **task-11 终局判定**）。
> 本文件是**独立复核**，不是对 `usb_probe` / `mtp_probe` 自报结论的复述。
> 被测契约：[probe-contract.md](./probe-contract.md) §1/§2（**最终定稿**）
> 探测范围：[probe-plan.md](./probe-plan.md) §3 冒烟 C（离线）
> 工具：`tools/desc_lint.py`（纯标准库）、`tools/verify-offline.ps1`

---

## 0. 判定摘要

| 项 | 结论 |
|---|---|
| **冻结版代码 vs 最终契约**（本次判定） | **✅ PASS** —— 模式 A **120/120**、模式 B **137/137** 断言通过，两者 **exit 0** |
| ❌ 验证失败 | **0 条** |
| ⚠️ 告警（WARN，默认不计入退出码） | 每模式 **1 条**：两模式序列号相同（`PROBE-D3`）——契约已删除该要求，故仅告警 |
| ⚠️ 未验证（需真机，**且本阶段真机已不可用**） | **8 项**（§5） |
| 端点预算（R3） | **✅ PASS** —— 模式 B 加 NCM 后 5 IN + EP0 = 6/8，余 2 |
| 本次判定的**边界** | **这是"冻结版代码与最终契约一致"的判定，不是端到端功能判定**（见下） |

### 本次判定**不是**什么（重要）

- ❌ **不是**"设备在真机上能工作"的判定。板子已被用户拔掉，**真机不可再用**，本阶段无法再做任何真机复现。
  所有真机结论（`problem=0`、Chrome `claimInterface` 成功等）都是**上一阶段采集的历史证据**，
  由 Lead/probe-contract 记录，本报告只**引用**、不自行复现。
- ❌ **不是**"回环/吞吐/心跳时序正确"的判定（需要跑起来才有意义）。
- ✅ 是**"冻结版 ELF 里的描述符字节与最终契约 §1/§2 是否逐项一致"**的判定 —— 也就是冒烟 C 的离线部分。

> ## ⚠️ 免责声明（请勿删改）
>
> **离线 lint PASS 不能证明 Chrome 能看到设备** —— 那取决于 Windows 侧 device interface 的注册
> （由 MS OS 2.0 Registry Property 决定），不是描述符字节能证明的。
> 反过来，**离线 lint FAIL 也不等于真机失败**：它只说明"描述符字节与当前契约不一致"。
> 真机结论只认 [probe-report.md](./probe-report.md)。
>
> 这条特别针对本轮的结论：我们**通过**了，但"通过"只覆盖 §0 表格第三行那个范围。
> 任何"所以 Chrome 一定能看到设备"的推论都是**越界**的 ——
> 本次判定能说的是：**冻结代码写出的 Registry Property 形状与"真机验证过能成功"的那个形状逐字节相同**
> （单数 `DeviceInterfaceGUID` + `REG_SZ` + `wLength=128`），仅此而已。

---

## 1. 验证对象：冻结基线

### 1.1 冻结基线 SHA256（Lead 固化，**我逐个复算并全部命中**）

| 文件 | 期望（前 16 位） | 我复算 | 命中 |
|---|---|---|---|
| `components/usb_probe/usb_probe.c` | `DC2B67297EFB185C` | `DC2B67297EFB185C` | ✅ |
| `components/usb_probe/include/usb_probe.h` | `0438F47045BF349D` | `0438F47045BF349D` | ✅ |
| `components/usb_probe/CMakeLists.txt` | `18EEE63602AD5A1E` | `18EEE63602AD5A1E` | ✅ |
| `components/usb_probe/Kconfig` | `D690ADB6A79B2E12` | `D690ADB6A79B2E12` | ✅ |
| `components/mtp_probe/mtp_probe.c` | `925FF9B73B4E4BF2` | `925FF9B73B4E4BF2` | ✅ |
| `main/main.cpp` | `D122327EC46B2D27` | `D122327EC46B2D27` | ✅ |
| `web/probe.html` | `BF8E88D949F054E6` | `BF8E88D949F054E6` | ✅ |
| `tools/desc_lint.py` | `B34C7F7F0F18DE96` | `B34C7F7F0F18DE96` | ✅ |

**8/8 命中** —— 即"我判定时看的代码"与"Lead 冻结的代码"是同一份。
（本轮 `desc_lint.py` 因 task-11 要求被修改，**修改前的 hash 正是基线值 `B34C7F7F…`**；
工具自身的改动见 §3。）

### 1.2 权威对象：`build-final-a` / `build-final-b`（**未重新构建**）

按 task-11 要求，**直接使用 Lead 构建的冻结产物**，没有重建（避免再次踩"移动目标"）：

```
build-final-a\MeidaP4.elf   5,855,508 B   2026-10-16 21:16:34
build-final-b\MeidaP4.elf   6,987,612 B   2026-10-16 21:17:34
```

### 1.3 复现步骤（**构建产物随后已被归档**）

本次判定所依据的 ELF 随后已被归档到 `archive/probe-batch0/builds/`
（Lead 归档全部 `build-*`，约 3.3 GB，只留有效代码 + 经验文档）。
**可用下面两条命令重建后再复现**：

```powershell
pwsh -File tools/build.ps1 -Mode a -Slot final      # 若 ELF 已被归档
pwsh -File tools/build.ps1 -Mode b -Slot final
pwsh -File tools/verify-offline.ps1                 # 默认槽位已是 final
```

`tools/verify-offline.ps1` 的默认 `-Slot` **已从 `verify` 改为 `final`**（冻结槽位），
并在缺 ELF 时打印上面这两条重建命令。单模式：

```powershell
python tools/desc_lint.py --mode a --slot final
python tools/desc_lint.py --mode b --slot final --json
python tools/desc_lint.py --mode a --slot final --strict-baseline   # 见 §3.2
```

---

## 2. 终局实测结果（raw）

```
$ pwsh -File tools/verify-offline.ps1
Mode Total Passed Failed Warn Exit Result
---- ----- ------ ------ ---- ---- ------
a      120    120      0    1    0 PASS
b      137    137      0    1    0 PASS

总判定: PASS - 两个模式的描述符断言全部通过（离线部分）
[exit code: 0]
```

每模式唯一的那条 WARN：

```
WARN  两模式序列号相同  --  b'PROBE-D3'。契约 §2 已删除"模式间必须不同"
      （PID 不同已构成不同实例）；但若你期望靠序列号区分实例，请检查是否漏改。
```

模式 A / B 的关键解析结果（`desc_lint.py` §[4]）：

```
[4] MS OS 2.0 descriptor set  (158 B)   ← mode a
    @0   wLength=10   type=0x0000 SET_HEADER        wTotalLength=158
    @10  wLength=20   type=0x0003 COMPATIBLE_ID     CompatibleID=b'WINUSB\x00\x00'
    @30  wLength=128  type=0x0004 REG_PROPERTY
    ✅ 形态 = [0] 设备级（无 subset header，真机实测形态）
    [Registry Property @30] wLength=128 wPropertyDataType=0x0001 (REG_SZ ✅契约要求)
        wPropertyNameLength=40  PropertyName='DeviceInterfaceGUID'
        wPropertyDataLength=78  PropertyData='{A5DCBF10-6530-11D2-901F-00C04FB951ED}'
    [长度自洽] BOS 字段=158  SET_HEADER.wTotalLength=158  子描述符累加=158  数组实际=158

[4] MS OS 2.0 descriptor set  (166 B)   ← mode b
    @0   wLength=10   type=0x0000 SET_HEADER        wTotalLength=166
    @10  wLength=8    type=0x0002 FUNCTION_SUBSET   bFirstInterfaceNumber=1 wSubsetLength=156
    @18  wLength=20   type=0x0003 COMPATIBLE_ID     CompatibleID=b'WINUSB\x00\x00'
    @38  wLength=128  type=0x0004 REG_PROPERTY      （字段同 mode a）
    ✅ 形态 = [0] 复合形态（含 Function Subset，契约 §2 要求）
    [长度自洽] BOS 字段=166  SET_HEADER.wTotalLength=166  子描述符累加=166  数组实际=166
```

`bcdDevice=0x0102`（= 冻结基线值，故不产生 WARN）；序列号 `PROBE-D3`（两模式相同）。

---

## 3. 我改了哪 4 组断言：逐条说明依据 + **强度是否变弱**

task-11 的核心是"改的是**过时断言**，不是放松契约"。下面逐条给出
**旧断言 → 新断言 → 契约依据 → 强度变化**，并把**任何实质变弱的地方显式标出来**（不自己吞掉）。

### 3.1 变更 1：`bcdDevice`

| | 内容 |
|---|---|
| **旧断言** | `device.bcdDevice == 0x0100`（硬 FAIL） |
| **新断言** | 硬：`bcdDevice != 0`；**WARN**：`bcdDevice != 0x0102`（冻结基线） |
| **契约依据** | §2 表格已改为「**每次改动描述符内容就递增**（当前实测 `0x0102`）」——递增是"强制 Windows 造新实例"的手段，本身无固定值 |
| **强度变化** | ⚠️ **实质变弱**（这是 4 组里最需要你确认的一条）：具体的 `0x0100` 期望值被删掉了，默认只剩"非 0"。理由：契约把一个**没有固定值**的字段变成了"策略"，硬编码任何值都会在下次合法递增时产生假 FAIL。 |
| **补偿** | 加 `--strict-baseline`：把"与基线不同"提升为 FAIL。实测 `--strict-baseline` 下 mode a 变为 `FAIL 有 1 条断言不满足 / exit 1`，即**强度可随时取回**。 |
| **请你裁决** | 若你希望"冻结基线锁死 0x0102"，就用 `--strict-baseline`（或告诉我把它改成默认硬断言）。 |

### 3.2 变更 2：模式间序列号

| | 内容 |
|---|---|
| **旧断言** | `序列号在模式 A/B 间必须不同`（硬 FAIL，靠 `--peer` 跨模式比对） |
| **新断言** | 硬：两模式序列号**都非空**、且是**可打印 ASCII**（本模式内）；**WARN**：两模式相同 / 不以 `PROBE-` 开头 |
| **契约依据** | §2 表格已改为「**每次改动描述符内容就换**（当前实测 `PROBE-D3`）；模式 A/B 的 PID 不同，本身已是不同实例」——"模式间必须不同"这个要求已被**删除** |
| **强度变化** | ⚠️ **实质变弱**：跨模式唯一性不再硬校验。理由：契约明确删除了该要求，且它给出的新理由是"PID 不同已构成不同实例"（PID 差异我仍然**硬断言**）。 |
| **保留的强信号** | `idProduct` 模式间必须不同 → **仍是硬断言** ✅ |
| **补偿** | `--strict-baseline` 把"两模式相同"提升为 FAIL。 |
| **请你裁决** | 若最终决定"序列号也应模式间不同"（例如为了隔离 Windows 实例），把这条改回硬断言即可 —— 一句话的事。 |

### 3.3 变更 3：Registry Property 由**复数**反转为**单数**（方向与上轮相反）

| | 内容 |
|---|---|
| **旧断言** | `wPropertyDataType==0x0007`(REG_MULTI_SZ)、`wPropertyNameLength==42`、名字 `"DeviceInterfaceGUIDs"`、PropertyData 双 NUL 收尾 |
| **新断言** | `wPropertyDataType==0x0001`(**REG_SZ**)、`wPropertyNameLength==40`、名字 `"DeviceInterfaceGUID"`（单数）、`wPropertyDataLength==78`、PropertyData **单** UTF-16 NUL 收尾；**并新增两条反向硬断言**：数据类型 **不得** `0x0007`、名字 **不得**是复数 `DeviceInterfaceGUIDs` |
| **契约依据** | §2 §84-105：真机实测复数 + `REG_MULTI_SZ` 会让 `winusb.sys` 启动失败（**Code 31 / `0xC0000182`**，含/不含 `A5DCBF10` 都复现，且已排除 `REV_0100` 硬件 ID 撞车），切单数后立即 `problem=0` 并注册出 `{A5DCBF10-…}`，Chrome 155 里 `requestDevice` + `claimInterface` 成功 |
| **强度变化** | ✅ **没有变弱，反而更强**：新增了 2 条"禁止形态"的反向断言（旧版本会把复数的已知坏形态判为**通过**）。这是本次唯一一条**方向性反转**的断言。 |
| **保留的硬断言** | `wDescriptorType==0x0004`；PropertyData 含 `{A5DCBF10-6530-11D2-901F-00C04FB951ED}`；**`wLength == 10 + nameLen + dataLen`**；`wLength == 子描述符实际字节数`；`len(PropertyData) == wPropertyDataLength` ✅ 全部保留 |

> 说明：上轮我报告 §7.1 指出契约的 `wLength` 公式"少算 4 字节"，**Lead 已采纳并修正**
> （契约 §2 现在写的正是 `10 + nameLen + dataLen`，并标注了早期笔误）。
> 本轮我的断言与新契约式子一致，且仍保留"与布局自洽"这一硬约束。

### 3.4 变更 4：模式 A 的 subset header 由"硬性要求"降为"规范推荐"

| | 内容 |
|---|---|
| **旧断言** | 模式 A 序列必须 `SET → CONFIG_SUBSET → FUNC_SUBSET → COMPAT → REGPROP`（硬 FAIL） |
| **新断言** | 模式 A **两种形态都算通过**：`[0] 设备级 SET→COMPAT→REGPROP`（实测形态）或 `[1] 规范形态 SET→CONFIG_SUBSET→FUNC_SUBSET→COMPAT→REGPROP`；当前形态作为**信息**打印（`✅ 形态 = [0] …`） |
| **契约依据** | §2 §74-81：真机证明设备级形态下 `CompatibleIds` 照样出现 `USB\MS_COMP_WINUSB`、`Service=WINUSB`、`DriverInfPath=winusb.inf`，Registry Property 也被正确写进 `Device Parameters`；模式 A 早期 `problem=31` 的真因是**复数 RegProp 值**，不是缺 subset header |
| **强度变化** | ⚠️ **实质变弱**：模式 A 不再要求 subset。理由：契约明确降级，且真机反证了旧的因果解释（旧断言基于"缺 subset → problem=31"，该因果**已被推翻**）。 |
| **保留的强信号** | ① 形态必须是**允许集合之一**（出现未预期的序列仍然 FAIL）；② `SET`/`COMPAT`/`REGPROP` 各**恰好 1 个**、subset 各**至多 1 个**；③ 若出现 subset，则**内部必须自洽**（`wLength==8`、`bReserved==0`、`wSubsetLength==8+后续`、Func Subset 的 `bFirstInterfaceNumber==vendor itf`）；④ 模式 B **仍然硬要求** Function Subset ✅ |

### 3.5 强度变化汇总（**请你过目**）

| 断言组 | 方向 | 处置建议 |
|---|---|---|
| RegProp 单数反转 | **↑ 变强**（+2 条禁止形态断言） | 无需动作 |
| `bcdDevice` | **↓ 变弱**（定值 → 非 0 + WARN） | 要锁死就用 `--strict-baseline` |
| 模式间序列号 | **↓ 变弱**（硬不同 → 非空 + WARN） | 若要恢复，改回硬断言（1 行） |
| 模式 A subset | **↓ 变弱**（必需 → 允许两形态） | 契约已降级，建议保持；subset 内部自洽仍是硬的 |

**没有任何一条"必须保留"的硬断言被放松** —— 见 §4 的清单，
其中"长度三处自洽""RegProp 含 `{A5DCBF10-…}` 且 `wLength == 10+nameLen+dataLen`"
"`iLandingPage==0`""EP 表逐项""BOS 回调强符号 + 反汇编""端点预算推演""免责声明"全部保留且实测通过。

---

## 4. ✅ 已验证（全部为**离线可证**，本次实测通过）

| # | 项 | 关键证据 |
|---|---|---|
| 1 | **长度三处自洽** | a：`158/158/158/158`；b：`166/166/166/166`（BOS 字段 / `SET_HEADER.wTotalLength` / 子描述符累加 / 数组实际长度） |
| 2 | **RegProp 结构合法 + 含必需 GUID + 长度自洽** | `wLength=128 = 10+40+78`；`wDescriptorType=0x0004`；`wPropertyDataType=0x0001`；name=`DeviceInterfaceGUID`(40 B)；data 含 `{A5DCBF10-6530-11D2-901F-00C04FB951ED}`(78 B)；单 NUL 收尾 |
| 3 | **禁止形态缺席** | 数据类型 ≠ `0x0007`；名字 ≠ `DeviceInterfaceGUIDs`；PropertyData 无双 NUL ✅ |
| 4 | **`iLandingPage == 0`** | BOS WebUSB 能力 `bVendorCode=0x01, iLandingPage=0` |
| 5 | **两个 `bVendorCode` 不同且 `bRequest` 分发对应** | BOS：WebUSB=`0x01` / MS OS=`0x02`；源码级断言命中 `#define PROBE_BOS_VENDOR_CODE_MSOS20 0x02` 与 `bRequest == PROBE_BOS_VENDOR_CODE_MSOS20` / `..._WEBUSB` |
| 6 | **`wIndex` 同时接受 0 与 0x07** | 源码级：`if (request->wIndex == 0x0000 \|\| request->wIndex == PROBE_MSOS20_DESCRIPTOR_INDEX)` + `#define ... 0x07` |
| 7 | **EP 表与契约 §1 逐项对齐** | a：`0x01`/`0x81` bulk（FS 64 / HS 512）；b：`0x81` int 64 + `0x02`/`0x82` bulk + `0x03`/`0x83` bulk；分方向无重复、无 EP0 |
| 8 | **`bcdUSB >= 0x0210`** | device `bcdUSB=0x0210`；qualifier 亦为 `0x0210` |
| 9 | **device 类按模式分叉** | a=`0xFF/0x00/0x00`；b=`0xEF/0x02/0x01`；**qualifier 类字段与 device 逐字节一致** |
| 10 | **`tud_descriptor_bos_cb` 强符号 + 反汇编返回 `s_desc_bos`** | `type=T owner=usb_probe.c.obj`；反汇编 `lui/addi … # <s_desc_bos>` + `ret`（TinyUSB 里那份是 `TU_ATTR_WEAK` 且 `return NULL`） |
| 11 | **端点预算推演（含加 NCM）** | a：`1 IN → +EP0 = 2/8`，+NCM `3 IN → 4/8`（余 4）；b：`3 IN → +EP0 = 4/8`，+NCM `5 IN → 6/8`（余 2），与契约 §1 的 `5 IN + EP0 = 6/8` 吻合 |
| 12 | **免责声明每次运行都打印** | 见 §0；工具输出里置于报告开头与结论处，且 PASS 时额外重复一次 |
| 13 | 其它静态确认 | vendor 是 buffered(FIFO)（RX=TX=4096）；`tud_mount_cb`/`tud_umount_cb` 归 esp_tinyusb；MTP 在 `usb_probe_start()` 之前 install；字符串表 ≤8 且未用 `0xEE`；**S3/S4 已关闭** |

> ⚠️ **这些"已验证"全部是描述符/源码层面的静态事实**。它们**不能**推出
> "设备会被 Windows 正确驱动""Chrome 能看到设备" —— 见 §0 免责声明。

---

## 5. ⚠️ 未验证（需真机，**且本阶段真机已不可用**）

> **重要**：板子已被用户拔掉，**本阶段无法再做任何真机复现**。
> 所以本节条目的状态是"**永久未在此阶段验证**"，不是"待验证"。
> 下面凡是引用真机结论的（如 `problem=0`、Chrome 成功），都是**上一阶段采集的历史证据**
> （由 Lead 记录在 probe-contract §2 / probe-report），我**没有**也无法自行复现。

| # | 条目 | 为什么本阶段无法验证 | 历史/期望证据来源 |
|---|---|---|---|
| U1 | Windows 自动绑 `winusb.sys` 且 `problem=0`（**R1**） | 需真机；板子已拔 | probe-contract §2 记录的历史真机结果 |
| U2 | Chrome `requestDevice` + `claimInterface` 成功（**R1**） | 同上 | probe-contract §2（Chrome 155 记录） |
| U3 | 资源管理器出现 MTP 设备并能浏览 `storage`（**R2**） | 同上 | probe-report |
| U4 | bulk 双向回环逐字节一致（1 KB/64 KB/1 MB） | 需实际收发 | web/probe.html + probe-report |
| U5 | 心跳行时序与剥离逻辑 | 需运行时观察 | probe-report |
| U6 | 模式 B 的 MTP 与 vendor 是否真的互不干扰 | 需真机 | probe-report |
| **U7** | **本报告的 PASS 与真机成功之间的因果**（即"描述符对了 ⇒ 主机行为对"） | **原理上不是描述符字节能证明的** | 只能靠真机复测 |
| **U8** | **"单数 RegProp **+ 带 subset"** 这个组合**从未被真机验证过** | 契约 §2 自己标注了这一点（并指向 "§5 U8"，即本表）；真机验证过的是"单数 + 无 subset"（模式 A）与"复数/单数过渡"的历史组合，而模式 B 当前是"单数 + **有** Function Subset"且**本阶段没上过电** | 契约 §2 §79-81 |

> **U8 需要特别注意**：模式 B 的冻结形态是 `SET → FUNC_SUBSET → COMPAT → REGPROP`（单数 RegProp + subset）。
> 这个**具体组合**在真机上没有被单独验证过 —— 真机确认的是"单数 RegProp 修好了 `winusb.sys`"。
> 因为板子已拔，**U8 在本阶段无法关闭**；若将来要上电，建议把它列为第一优先复测项。

---

## 6. ❌ 验证失败

**0 条。** 冻结版（8 个文件 hash 全命中）与最终契约 §1/§2 逐项一致：

- 结构类：device 类分叉 ✅、EP 表 ✅、BOS ✅、MS OS 集形态 ✅（两模式各自命中允许形态）
- 长度类：三处自洽 ✅（a 158 / b 166）
- RegProp 类：单数 + `REG_SZ` + 40/78/128 + 含 `{A5DCBF10-…}` + 单 NUL ✅；复数禁止形态缺席 ✅
- 回调类：`tud_descriptor_bos_cb` 强符号 + 返回 `s_desc_bos` ✅
- 预算类：端点预算 + NCM 推演 ✅

**WARN 1 条/模式**（不计入失败）：两模式序列号相同 `PROBE-D3`，契约已删除该约束。

---

## 7. 存疑清单（终局定级）

| 编号 | 最终状态 | 内容 |
|---|---|---|
| **S1** | ✅ **已消解（真因是复数 RegProp，不是缺 subset）** | 首版我提出的"模式 A 缺 Configuration/Function Subset Header"。真机最终证明：设备级形态（无 subset）**能**正确应用 `USB\MS_COMP_WINUSB` / Registry Property；模式 A 早期 `problem=31` 的真因是**复数 RegProp 值**。subset header 已按契约降为"规范推荐"。→ 我原来的因果推断**被真机推翻**，这点如实记录。 |
| **S2** | ✅ **收敛（不触发）** | WebUSB `GET_URL` 被 stall：`iLandingPage=0` 后 Chrome 完全不发 `bRequest=0x01`（真机日志）。代码里的 stall 分支保留为防御。 |
| **S3** | ✅ **已修（关闭）** | `usb_probe_stats_t` 8 字段已写进契约 §3.1，与头文件逐字段一致。 |
| **S4** | ✅ **已修（关闭）** | MTP"描述符段 vs glue 安装"错配已加 `#if CFG_TUD_MTP && !CONFIG_PROBE_ENABLE_MTP` → `#error`。 |
| **S5** | ⚠️ **永久未验证** | FS bulk MPS=64 与 `CONFIG_TINYUSB_VENDOR_EPSIZE=512` 错配只影响 TX ZLP 边界。真机已不可用 → 只能留给完整版。 |
| **S6** | ⚠️ **未验证（低风险）** | `usb_probe_start()` 不校验 MTP 是否已 install（只靠 `main.cpp` 顺序，静态已核对为正确顺序）。 |
| **S7** | ✅ 记录 | 契约 §1 "512" 歧义已按 FS=64 / HS=512 分速度处理。 |
| **S8** | ✅ **已修** | 契约 §2 的 RegProp `wLength` 公式（原 `4+name+data+2` 少算 4 B）**已被 Lead 采纳并改正**为 `10+name+data`。我上轮的报告条目关闭。 |
| **S9** | ✅ 记录 | 过程风险（源码移动目标）已在本次通过"冻结 hash + 直接使用 final ELF + 不重建"消除。 |
| **S10** | 🆕 **契约内部一处不一致（低）** | 契约 §2 的**字符串索引表**（§119-125）仍写 `idx 3 = Serial "PROBE-0001"`，而同一节的 §47 写"每次改动就换（当前实测 `PROBE-D3`）"。实测为 `PROBE-D3`。建议把索引表里的 `PROBE-0001` 改成"`PROBE-*`（每次改动就换）"以免后来者误用。**我没有按索引表断言**（否则又是假 FAIL），只硬断言"非空 + 可打印 ASCII"。 |
| **S11** | 🆕 **契约交叉引用指向本报告** | 契约 §2 §81 写"见 §5 U8"，但契约 §5 是"构建命令"。我理解它指的是**本报告 §5 的 U8**，已按此建立 U8 条目（§5）。建议契约里写全 `docs/probe-verify.md §5 U8`。 |

---

## 8. 工具变更说明（`tools/desc_lint.py` + `tools/verify-offline.ps1`）

> 本轮写作用域原为 `tools/desc_lint.py` + `docs/probe-verify.md`；
> Lead 追加要求把 `tools/verify-offline.ps1` 的默认槽位改成 `final`，故一并修改。
> 未改 `components/`、`main/`、`web/` 及 `tools/` 下其它任何文件。

### `desc_lint.py`

- 4 组断言按最终契约改写（§3）；新增 `WARN` 通道（默认不影响退出码）与 `--strict-baseline`。
- 摘要行现在区分四种状态：**通过 / 失败 / 跳过 / WARN**，避免"没检查"或"允许的漂移"被读成结果。
- 免责声明保留在报告开头与结论处，PASS 时额外再重复一次（§4 第 12 项）。
- 断言组：`[0]`符号表 `[1]`device `[1b]`qualifier `[2]`FS/HS 配置 `[3]`BOS `[4a]`MS OS 结构
  `[4b]`Registry Property `[4c]`长度自洽 `[5]`字符串表 `[6]`回调归属 `[7]`端点预算 `[8]`源码级 `[9]`跨模式比对
- 退出码：`0` = 全部断言通过 · `1` = 有断言失败（或被 `--strict-baseline` 提升的 WARN） · `2` = 工具/输入错误

### `verify-offline.ps1`

- **默认 `-Slot` 改为 `final`**（冻结槽位）。
- 新增 `-StrictBaseline`（透传给 `desc_lint.py --strict-baseline`）。
- 汇总表新增 `Warn` 列；缺 ELF 时打印"先重建"的确切命令（见 §1.3）。

---

## 9. 给 Lead 的结论

1. **终局判定：PASS。** 冻结版（8 个 hash 全命中）与最终契约 §1/§2 一致 ——
   模式 A **120/120**、模式 B **137/137**，两者 `exit 0`，**0 条失败**。
   唯一告警是"两模式序列号相同"，而契约已删除该约束，故仅 WARN。
2. **本次是"冻结版代码 vs 最终契约"的判定，不是端到端功能判定。**
   板子已拔、真机不可再用；所有真机结论均为上一阶段的历史证据，我没有也**无法**复现。
   **"离线 PASS 不能证明 Chrome 能看到设备"** 这句已写死在报告 §0 与工具输出里。
3. **4 组断言确实改了，其中 3 组实质变弱**（`bcdDevice` 定值 → 非 0、模式间序列号硬不同 → 非空、
   模式 A subset 必需 → 两形态允许），**1 组变强**（RegProp 单数反转，新增 2 条禁止形态断言）。
   逐条依据与强度变化见 §3；**没有任何一条"必须保留"的硬断言被放松**（§4 清单全绿）。
   变弱的三条我都留了 `--strict-baseline` 作为强度取回手段 —— 若你要求默认锁死基线，告诉我即可。
4. **两处建议顺手修的契约小问题**：§2 字符串索引表仍写 `PROBE-0001`（实测 `PROBE-D3`，§10 S10）；
   §2 的 "见 §5 U8" 交叉引用应写全为 `docs/probe-verify.md §5 U8`（§11）。
5. **未来唯一值得补的真机项是 U8**：模式 B 的"单数 RegProp **+ 带 Function Subset**"组合
   **从未被真机单独验证**（真机验证的是"单数 + 无 subset"）。因为板子已拔，本阶段无法关闭，
   建议写进完整版的复测清单。
6. **归档可复现**：本次对象（`build-final-*`）随后会被归档到 `archive/probe-batch0/builds/`，
   报告 §1.3 已写明用 `pwsh -File tools/build.ps1 -Mode a|b -Slot final` 重建后再跑
   `pwsh -File tools/verify-offline.ps1`（默认槽位已是 `final`）。
