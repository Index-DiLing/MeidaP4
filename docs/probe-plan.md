# 探测阶段方案（批 0）· 可行性验证

> 上位计划：[usb-composite-plan.md](./usb-composite-plan.md)。本文件只描述**探测阶段**要做什么、怎么验收。
> 探测固件是**一次性的**：只验证风险点，不做完整架构。探测结论回填后，按 plan 的批 1~4 推倒重做。

---

## 0. 为什么先探测

plan 的架构里有 3 处"一旦错了要整体返工"的假设，必须在写业务代码前用真机证伪/证实：

| # | 假设 | 若为假的后果 | 探测手段 |
|---|---|---|---|
| **R1** | Windows 能**自动**给 vendor 接口挂 `winusb.sys`（靠 MS OS 2.0 描述符），Chrome 在 `file://` 下能 `claimInterface` | 每次插拔都要 Zadig 手动装驱动；`file://` 方案不成立 | 冒烟 A（真机 + 网页） |
| **R2** | MTP + Vendor 复合后，**MTP 仍被 Windows 识别**，且网页**同时**能 claim vendor 接口 | plan 的核心"一个口三件事"不成立，要退回单 Vendor | 冒烟 B（真机） |
| **R3** | `esp_tinyusb` 能撑起 plan 的端点表（MTP 2 IN + 1 OUT、Vendor 1 IN + 1 OUT、NCM 2 IN + 1 OUT） | 端点预算算错，NCM 加不进来 | 离线静态校验 + 冒烟 C |

---

## 1. 探测固件范围（明确"不做什么"）

**做**：
- 一个 ESP-IDF 工程（本目录），`esp_tinyusb 2.3.0`，P4 HS 口。
- **模式 A**：单 Vendor 接口 + WebUSB + MS OS 2.0（WCID）+ bulk 回环。
- **模式 B**：MTP + Vendor 复合 + WebUSB/WCID + bulk 回环。
- 一键 `build / flash / monitor` 脚本，显式加载 EIM 环境。
- 浏览器端单文件验证页（`web/probe.html`）。
- 离线描述符静态校验工具（不依赖硬件也能查描述符表）。

**不做**（留给完整版）：
- SD 卡 / PSRAM 视频 / LVGL / PPA / JPEG / H.264。
- esp_hosted / Wi-Fi / NCM / NAPT（冒烟 C 只做**离线端点预算**校验，不上电）。
- 完整 I0 线协议（探测期只用"裸回环 + 心跳"，见 `probe-contract.md`）。
- 完整 I1 功能注册框架（探测期由 `usb_probe` 直接写死描述符）。

---

## 2. 工程结构（探测期）

```
MeidaP4/
├─ CMakeLists.txt                  # 工程入口（未改）
├─ partitions.csv                  # [Lead] 32MB flash：nvs/phy/factory(4M)/storage fat(4M)
├─ sdkconfig.defaults              # [Lead] 公共基线（PSRAM/Flash/FATFS/TinyUSB 类目）
├─ sdkconfig.a.defaults            # [Lead] 模式 A：MTP 关
├─ sdkconfig.b.defaults            # [Lead] 模式 B：MTP 开
├─ main/
│  ├─ CMakeLists.txt               # [Lead]
│  ├─ idf_component.yml            # [Lead] esp_tinyusb 锁 2.3.0
│  ├─ Kconfig.projbuild            # [Lead] 探测开关
│  └─ main.cpp                     # [Lead] 只做接线 + 遥测打印
├─ components/
│  ├─ usb_probe/                   # [usb-core] 描述符/BOS/WCID/回环
│  └─ mtp_probe/                   # [mtp-core] FATFS 卷 + MTP 注册
├─ web/probe.html                  # [web-host] 浏览器验证页
├─ tools/
│  ├─ idf-env.ps1                  # [Lead] 显式加载 EIM 环境
│  ├─ build.ps1  flash.ps1         # [Lead] 一键脚本
│  └─ desc_lint.py                 # [verifier] ELF 描述符静态校验
└─ docs/probe-*.md                 # 计划/契约/验证报告
```

---

## 3. 冒烟项与验收标准

| 冒烟 | 固件模式 | 验收（必须可复现） | 依赖硬件 |
|---|---|---|---|
| **A** | `mode a` | ① `idf.py build` 干净；② Win11 设备管理器出现 `MeidaP4 Probe` 且 vendor 接口自动绑 `WinUSB`；③ `web/probe.html` 在 `file://` 下 `requestDevice` 能选中、`claimInterface` 成功；④ 回环 1 MB 数据校验一致；⑤ 页面能读出 BOS/WebUSB 平台能力 | ✅ |
| **B** | `mode b` | ① 资源管理器出现 MTP 便携设备、能浏览 `storage` 卷里的文件；② **同时**网页仍能 `claimInterface` vendor 接口并跑通回环；③ 设备管理器里 MTP 走 `wpdmtp.sys`、vendor 走 `WinUSB`，互不干扰 | ✅ |
| **C** | 离线 | 描述符静态校验：模式 A/B 的 IN 端点 ≤ 7（+EP0 = 8）、EP 号无重复、`wTotalLength` 与逐段实测一致、BOS 内含 WebUSB UUID，**并推演**加 NCM 后仍 ≤ 8 | ❌ 不需要 |

> ⚠️ 冒烟 A/B 的真机部分**当前被阻塞**：本机未插 ESP32-P4 开发板（无 USB JTAG/串口设备）。离线部分见冒烟 C。

---

## 4. 关键结论（探测期已确认，来自源码）

1. **`esp_tinyusb 2.3.0` 完全没有 BOS / WebUSB / MS OS 描述符支持**（全树 grep `MS_OS|WINUSB|webusb|desc_bos` 零命中）。
   → plan §5.1 的 WCID **必须自己写**。上游 `tinyusb 0.21.0~2` 提供了全部宏：`tud_descriptor_bos_cb()`、`TUD_BOS_WEBUSB_DESCRIPTOR()`、`TUD_BOS_MS_OS_20_DESCRIPTOR()`、MS OS 2.0 描述符类型枚举。
2. **`esp_tinyusb` 的默认配置描述符只覆盖 CDC / MSC / MTP / NCM，不覆盖 VENDOR**（`descriptors_control.c:206,223`）。
   → **纯 Vendor 配置（模式 A）必须自带 FS+HS 配置描述符**，否则 `tinyusb_driver_install()` 返回 `ESP_ERR_INVALID_ARG`。这是探测期发现的第一处 plan 未覆盖的坑。
3. **P4 HS 口端点上限经源码确认**：`tinyusb/src/portable/synopsys/dwc2/dwc2_esp32.h:80` → `ep_count=16, ep_in_count=8`（IN 含 EP0 共 8），`otg_dfifo_depth=1024`（= 4 KB TX FIFO）。plan 的 `6 IN + EP0 = 7/8` 算术成立。
4. **MS OS 1.0（字符串描述符 0xEE）路线不可行**：`esp_tinyusb` 的字符串数组上限是 8（`descriptors_control.h:15`），索引 0xEE 放不进去。**只能走 MS OS 2.0（BOS + 供应商请求）**。
5. **`tud_vendor_control_xfer_cb()` / `tud_vendor_rx_cb()` 必须由应用实现**（esp_tinyusb 未提供）→ WCID 请求处理器就该放这里。
6. 组件仓库缓存里已有 `esp_tinyusb 2.3.0` / `tinyusb 0.21.0~2`（`C:\Espressif\tools\components`），可离线安装；注册表最新是 `esp_tinyusb 2.4.0`。

### 4.7 真机（2026-10）追加的两条硬结论 —— 详见 [probe-report.md](./probe-report.md) §3

7. ⛔ **「WCID 自动绑 winusb.sys」不足以让 Chrome 看到设备**。实测 `MI_01` 服务 = WINUSB、compatible ID = `USB\MS_COMP_WINUSB`，但它在 `DeviceClasses` 里**注册了 0 个 device interface**，`Device Parameters` 下也没有 `DeviceInterfaceGUIDs`（`winusb.inf` 的 `[WINUSB.NT.HW]` 是空占位段）。结果 Chrome 155 在 `file://` 和 `http://127.0.0.1` 下都是**空 chooser**。
   → 必须补 MS OS 2.0 **Registry Property**（`DeviceInterfaceGUIDs`，`REG_MULTI_SZ=0x07`），或让该功能做**非复合**设备。**plan §5.1 与 §6 的"Zadig 降级"都覆盖不到这一条**（驱动已经绑上了，缺的是接口注册）。

8. ⛔ **`esp_tinyusb 2.3.0` 的 MTP 默认不可用**（链接期缺陷）：上游 tinyusb 的 `tud_mtp_*` 弱默认实现赢了链接，esp_tinyusb 的强实现（`mtp/tinyusb_mtp_transport.c`）没被拉进来 → 每条 MTP 命令都被 stall（真机：OpenSession → `Stall EP 02/EP 82`）。修法：`-Wl,--undefined=mtp_dispatch`。2.4.0 的 CHANGELOG 未提，大概率同样存在。


---

## 5. 环境事实（本机实测）

| 项 | 值 |
|---|---|
| ESP-IDF | `C:\esp\v6.1\esp-idf`（Junction → `H:\.Import_CDiskLinker\esp\v6.1\esp-idf`），版本 **v6.1**，git HEAD `fff9895c` |
| 工具链 | `C:\Espressif\tools`（cmake 4.0.3 / ninja 1.12.1 / riscv32-esp-elf esp-15.2.0 / python v6.1 venv） |
| EIM 激活脚本 | `C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1` |
| ⚠️ 会话继承的陈旧环境变量 | `IDF_PATH=E:\esp32\.espressif\v6.0.2\esp-idf`（**不存在**）、`IDF_TOOLS_PATH=E:\esp32\.espressif`（**不存在**）→ 所有脚本必须显式设环境 |
| ⚠️ `ESP_IDF_VERSION` 必须显式设置 | 否则 `idf.py` 直接 Python `TypeError`（`idf_component_manager/idf_extensions.py:388`） |
| 目标板 | **未连接**。无 CP210x/CH34x/USB-JTAG 设备；`COM1/6/7/8/9/10` 全是蓝牙或主板串口 |

---

## 6. 探测结论的回填位置

- 离线结论 → `docs/probe-verify.md`
- 真机结论 → `docs/probe-report.md`（Lead 汇总，含实测数字）
- 需要改动 plan 的条目 → 回填 `docs/usb-composite-plan.md` 并在 `probe-report.md` 里列为"决策变更"
