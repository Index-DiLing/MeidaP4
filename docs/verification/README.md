# docs/verification/ —— 独立验证入口与结论索引

本目录是 **verifier（task-4）** 的产出一半（另一半是 `test/host/**` 与 `tools/verify-proto.ps1`）。
**不写产品代码**：这里只放"怎么复现"与"能/不能证明什么"。

---

## 一条命令

```powershell
pwsh -File tools/verify-proto.ps1
```

| 步骤 | 断言 | 判据 |
|---|---|---|
| 1 向量完整性 | — | `test/host/gen_vectors.py` 重算结果与 `tools/proto_vectors.json` **逐字节一致** |
| 2 `test_codec (C)` | 679 | 真实 `components/webusb_proto/wu_codec.c`（原生 gcc） |
| 3 `proto.js (JS)` | 345（+14 SKIP） | 真实 `web/proto.js`（node），与 C 侧**同一批** 29 个向量 |
| 4 `test_composite_desc` | 389 | 真实 `components/usb_composite/usb_composite_desc.c`，逐行核对冻结表 |
| 5 `test_composite_desc(负例)` | 2 | 超 IN 预算必须被拒绝 |
| 6 `proto SM (JS)` | 23（当前 **SKIP**） | I0 状态机（`ReliableChannel`/`LinkSession`）纯转移函数；**需 `web/proto.js` 导出 `reliableStep`/`linkStep`**，否则记 SKIP 并明确提示"状态机语义此时未被验证" |
| 7 `test_composite_desc(真实NCM)` | 6（当前 **FAIL**） | 集成：WS-net **真实** `usb_net_ncm.c` 片段 → 真实装配器/校验器（期望值与 mock 路径**共用**，不给真片段开后门）。当前红 = **F9 结构体生命周期 UB** |

退出码：`0` 全部 PASS（含 SKIP）；`1` 有断言 FAIL；`2` 工具/输入缺失（缺 `wu_codec.c` / `proto.js` / `desc.c` / gcc / node）。

其它模式：
```powershell
pwsh -File tools/verify-proto.ps1 -SkipComposite        # 只验 I0 线协议
pwsh -File tools/verify-proto.ps1 -HarnessSelftest      # 只验"测试骨架本身"（临时参考实现，证明不了产品）
```

---

## 最新一次运行的实测结果

**2026-10-17 09:39 快照 · `verify-proto.ps1` exit 0 · `build.ps1 -Mode prod -Slot verify` exit 0**

| Step | Status | Assert | Pass | Fail |
|---|---|---|---|---|
| 向量完整性 | PASS | — | — | — |
| test_codec (C) | PASS | 679 | 679 | 0 |
| proto.js (JS) | PASS | 345 | 345 | 0 |
| test_composite_desc | PASS | 389 | 389 | 0 |
| test_composite_desc(负例) | PASS | 2 | 2 | 0 |
| test_composite_desc(真实NCM) | **PASS** | 389 | 389 | 0 |
| proto SM (JS) | **SKIP** | — | — | — |

> ⚠️ `proto SM` 的 SKIP 是**实情披露**：`web/proto.js` 还没提供 `reliableStep`/`linkStep`
> （Lead 2026-10 裁决要求的纯转移函数形状），所以 `ReliableChannel` 的窗口/ACK/重传/判死与
> `LinkSession` 的 HELLO 状态机、3 s 掉线、ZLP 计数**目前仍未被验证**。
> 向量与断言器已就绪（我用自己的参考实现自检 23/23 通过），接口一落地即自动转为硬断言。
> 另发现 1 处**规约缺口**：Lead 指令引用的 `§6.5⑥`（引导帧绕开 seq 门）在冻结文档里不存在 ——
> 两侧实现都做了、文档没写，详见主报告 §5A.4。
>
> ✅ **F9 已闭环**（09:39）：集成路径（真 `usb_net_ncm.c` 片段 × 真装配器）曾抓到一条**结构体生命周期 UB**
> （`uc_prepare()` 读到悬垂栈内存）。Lead 采纳"`uc_register()` 内部深拷"，ws-net 同时把注册结构体改 `static`；
> 现在该步骤 **389/389 PASS**。详见主报告 §7.5。

固件构建同一快照：`[build] OK -> build-verify-prod\MeidaP4.bin`（**828,528 B**，app 分区余 80%）；
8 个完整版组件**全部**进入构建图并编译（含 `usb_net` 的 `usb_net_ncm.c`）。
唯一按设计未编译的是 `usb_net_bridge.c`（只在 `CONFIG_APP_USBNET_MODE_BRIDGE=y` 时入 SRCS）。

> ⚠️ **这个 PASS 只覆盖"离线一致性"**：C/JS 编解码器与 `docs/protocol.md` 逐字节一致；
> 描述符与 `docs/contracts/frozen-allocation.md` 逐行一致。
> **不覆盖**真机 USB 链路（枚举、Windows 驱动绑定、device interface 注册、Chrome claim、NCM 上网、
> MTP 浏览、DWC2 实际 FIFO 分配、供电与热稳定）。设备未连接 ⇒ **离线 PASS ≠ 真机通过**。

固件构建（另一个维度，与上表独立）：
```powershell
pwsh -File tools/build.ps1 -Mode prod -Slot <你的新槽位>
C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe test\host\out\count_build_coverage.py build-<槽位>-prod
```
`count_build_coverage.py` 会打出每个组件的 `编译=N 源文件=M`。**N<M 就是问题**（有 .c 没进 SRCS）。

---

## 文件索引

| 文件 | 内容 |
|---|---|
| [`offline-verification-report.md`](offline-verification-report.md) | **主报告**：逐条结论、缺陷记录（F1–F8）、对每个队友的对抗性发现、能/不能证明的边界、真机必测清单 |
| `../test/host/gen_vectors.py` | 黄金向量生成器（**不引用任何产品代码**；CRC 两种独立算法对撞 + catalogue check 值） |
| `../tools/proto_vectors.json` | 黄金向量（29 个：`frame` 10 / `crc` 5 / `stream` 14） |
| `../test/host/test_codec.c` | C 侧断言（含 11 类鲁棒性 + **喂入粒度不变量**） |
| `../test/host/test_composite_desc.c` | I1 描述符逐行断言（4 个 mock 功能，raw-bytes 解析，负例模式） |
| `../test/host/run_proto_js.mjs` | JS 侧同批向量断言 + `protocol.md §7` 契约面检查 |
| `../test/host/shim/esp_err.h` | 让冻结头在原生 gcc 下可编译的最小垫片（取值与 IDF v6.1 一致） |
| `../test/host/harness_selftest/` | **仅**用于证明"测试骨架本身没 bug"的临时参考实现（不是产品代码，默认不参与验证） |
| `../test/host/out/count_build_coverage.py` | 构建覆盖统计（判断"我的组件到底有没有被编译"） |
| `../test/host/out/*.log` | 最近一次验证/构建的原始输出（留档） |

---

## 三条验证纪律（本会话踩坑后固化）

1. **独立复算，不引用他人汇报的数字**。每条结论都要能指向可复现命令或原始输出。
   （实例：`§2.2` 的 FIFO 预算，靠读 `dcd_dwc2.c` 源码复算出 884，才发现冻结表漏了 EPInfo 32 word。）
2. **结论必须带时间戳 + 文件 sha256**。并发写入环境里，同一个文件在两个时刻可能是两个不同真相。
3. **归因靠证据，不靠团队映射**：按"文件 + mtime"归因，不按"谁负责哪个组件"归因。
