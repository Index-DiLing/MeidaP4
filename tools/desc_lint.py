#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
desc_lint.py - MeidaP4 探测固件 USB 描述符静态校验器（独立验证工具）

用途
----
从**已编译的 ELF** 里把描述符数组抠出来，逐字节解析，并与
`docs/probe-contract.md` §1/§2 的冻结契约对账。不依赖硬件、不依赖 idf.py。

工具链
------
用 `riscv32-esp-elf-nm -S` 拿符号地址/大小，用 `riscv32-esp-elf-objcopy -O binary
--only-section=<sec>` 拿节区原始字节，再按 VMA 做地址 -> 字节映射。
工具目录按以下顺序解析：
  1. --toolchain <dir>
  2. 环境变量 DESC_LINT_TOOLCHAIN
  3. PATH 里的 riscv32-esp-elf-nm
  4. C:\\Espressif\\tools\\riscv32-esp-elf\\*\\riscv32-esp-elf[\\riscv32-esp-elf]\\bin

退出码
------
  0 = 全部断言通过
  1 = 至少一条断言失败（描述符与契约不符）
  2 = 工具/输入错误（ELF 缺失、工具链找不到、符号找不到、解析失败）

只用 Python 3 标准库。用法：
  python tools/desc_lint.py --mode a --slot verify
  python tools/desc_lint.py --mode b --slot verify
  python tools/desc_lint.py --elf build-verify-a/MeidaP4.elf --mode a
  python tools/desc_lint.py --mode b --slot verify --json

生产者：verifier（独立校验，不修改被测代码）
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import re
import shutil
import struct
import subprocess
import sys
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Sequence, Tuple

EXIT_PASS = 0
EXIT_FAIL = 1
EXIT_ERROR = 2

# ---------------------------------------------------------------------------
# 契约常量（docs/probe-contract.md §1/§2 + Lead 已裁决的修订）
# ---------------------------------------------------------------------------

EXPECT_VID = 0x303A
EXPECT_PID = {"a": 0x4001, "b": 0x4002}

# ---- bcdDevice（契约 §2 已改为"每次改动描述符内容就递增"）----
# 契约不再钉死具体值（递增本身是"强制 Windows 造新实例"的手段），所以：
#   * 硬断言只要求 **非 0**；
#   * 与冻结基线值不同时记 WARN（不是 FAIL）——因为按契约那**可能是有意递增**；
#   * 加 --strict-baseline 可把该 WARN 提升为 FAIL（想要"钉死基线"强度的场景）。
EXPECT_BCD_DEVICE_BASELINE = 0x0102     # 冻结版实测值（docs/probe-contract.md §2）
MIN_BCD_USB = 0x0210              # 契约 §2：必须 >= 0x0210 主机才取 BOS

# 契约 §2（已更新）：device 描述符的类字段**按模式分叉**
#   模式 A = 0xFF/0x00/0x00（Vendor Specific，非复合 → winusb.sys 绑整个设备节点）
#   模式 B = 0xEF/0x02/0x01（Misc/IAD，复合 → usbccgp 再按接口分发）
EXPECT_DEV_CLASS = {
    "a": (0xFF, 0x00, 0x00),
    "b": (0xEF, 0x02, 0x01),
}
DEV_CLASS_DESC = {
    "a": "0xFF/0x00/0x00 Vendor(非复合)",
    "b": "0xEF/0x02/0x01 Misc/IAD(复合)",
}

# 契约 §1 表里的 "512" 是 HS 值；FS bulk 的合法上限就是 64（Lead 裁决修订 1）
BULK_MPS_FS = 64
BULK_MPS_HS = 512
MTP_EVT_MPS = 64                  # interrupt，两种速度都是 64

# 契约 §2 接口/端点分配
ITF_CLASS_IMAGE = 0x06
ITF_SUBCLASS_STILL_IMAGE = 0x01
ITF_PROTOCOL_PIMA_15470 = 0x01
ITF_CLASS_VENDOR = 0xFF

WEBUSB_UUID = bytes([0x38, 0xB6, 0x08, 0x34, 0xA9, 0x09, 0xA0, 0x47,
                     0x8B, 0xFD, 0xA0, 0x76, 0x88, 0x15, 0xB6, 0x65])
MSOS20_UUID = bytes([0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C,
                     0x9C, 0xD2, 0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F])
WEBUSB_VENDOR_CODE = 0x01
MSOS20_VENDOR_CODE = 0x02
MSOS20_WINDOWS_VERSION = 0x06030000
MSOS20_DESC_INDEX_ACCEPTED = (0x0000, 0x0007)   # 裁决修订 2：0 和 7 都合法

# ---- 契约 §2（已更新）：WebUSB 平台能力 iLandingPage 必须是 0 ----
EXPECT_WEBUSB_LANDING_PAGE = 0

# ---- 契约 §2（最终定稿，真机反转）：MS OS 2.0 Registry Property ----
# ⚠️ 必须用**单数** `DeviceInterfaceGUID` + **REG_SZ(0x0001)**。
#    真机证据（Lead/probe-contract §2）：复数 `DeviceInterfaceGUIDs` + REG_MULTI_SZ(0x0007)
#    会让 winusb.sys 启动失败（Code 31 / 0xC0000182），切单数后立即 problem=0。
#    （上一版契约/本工具曾按复数形式断言 —— 那是被真机推翻的旧结论。）
MSOS20_SET_HEADER = 0x0000
MSOS20_CONFIG_SUBSET = 0x0001
MSOS20_FUNC_SUBSET = 0x0002
MSOS20_COMPAT_ID = 0x0003
MSOS20_REG_PROPERTY = 0x0004

REGPROP_DATA_TYPE_REG_SZ = 0x0001              # ✅ 契约 §2 要求
REGPROP_DATA_TYPE_REG_MULTI_SZ = 0x0007        # ❌ 已知会让 winusb.sys Code 31，必须不出现
REGPROP_NAME_ASCII = b"DeviceInterfaceGUID"    # 单数（19 字符）
REGPROP_NAME_LEN = (len(REGPROP_NAME_ASCII) + 1) * 2    # (19+1)*2 = 40
REGPROP_NAME_FORBIDDEN_PLURAL = b"DeviceInterfaceGUIDs"  # ❌ 必须不出现
GUID_USB_DEVICE = "{A5DCBF10-6530-11D2-901F-00C04FB951ED}".encode("ascii")
GUID_WINUSB = "{DEE824EF-729B-4A0E-9C14-B7117D33A817}".encode("ascii")
# PropertyData = 1 个 GUID 的 UTF-16LE 文本 + 单个 UTF-16 NUL → (38+1)*2 = 78
REGPROP_DATA_LEN = (len(GUID_USB_DEVICE) + 1) * 2

# Registry Property 的 wLength 不变式。
# MS OS 2.0 布局（规范 §3.4.3 / TinyUSB webusb_serial usb_descriptors.c:214-223）：
#   wLength(2) wDescriptorType(2) wPropertyDataType(2) wPropertyNameLength(2)
#   PropertyName(nameLen) wPropertyDataLength(2) PropertyData(dataLen)
# → wLength = 2+2+2+2 + nameLen + 2 + dataLen = 10 + nameLen + dataLen
# 契约 §2 现在写的正是这个正确式子（早期"4+name+data+2"少算 4 B 的笔误已由 Lead 修正）。
REGPROP_LEN_FIXED = 10

# ---- 契约 §2（最终定稿）：MS OS 2.0 集的子描述符序列 ----
# 模式 A：**两种形态都合法**（subset header 已由"硬性要求"降为"规范推荐"）
#   A1 设备级（当前实现，实测 158 B）：SET → COMPAT → REGPROP
#   A2 规范形态（完整版建议）：        SET → CONFIG_SUBSET → FUNC_SUBSET → COMPAT → REGPROP
#   依据：真机证明 A1 也能正确应用 USB\MS_COMP_WINUSB / WINUSB / RegProperty；
#         模式 A 早期 problem=31 的真因是**复数 RegProp 值**，不是缺 subset header。
# 模式 B（复合）：仍按契约 §2 表格要求含 Function Subset
#   B1（实测 166 B）：SET → FUNC_SUBSET(if=vendor itf) → COMPAT → REGPROP
MSOS20_ALLOWED_SEQ = {
    "a": [
        (MSOS20_SET_HEADER, MSOS20_COMPAT_ID, MSOS20_REG_PROPERTY),
        (MSOS20_SET_HEADER, MSOS20_CONFIG_SUBSET, MSOS20_FUNC_SUBSET,
         MSOS20_COMPAT_ID, MSOS20_REG_PROPERTY),
    ],
    "b": [
        (MSOS20_SET_HEADER, MSOS20_FUNC_SUBSET, MSOS20_COMPAT_ID, MSOS20_REG_PROPERTY),
    ],
}
MSOS20_SEQ_DESC = {
    "a": {0: "设备级（无 subset header，真机实测形态）",
          1: "规范形态（含 Configuration+Function Subset，完整版建议）"},
    "b": {0: "复合形态（含 Function Subset，契约 §2 要求）"},
}
# 模式 A 下 subset 是"推荐"而非"必需"，所以只做信息输出；这两项仅在**出现**时校验其内部自洽
SUBSET_REQUIRED = {"a": False, "b": True}

# 契约 §2 字符串表（idx -> 期望的 C 字符串原始字节；idx0 是 LANGID 两字节）
# ⚠️ idx 3（序列号）**不再钉死具体值**：契约 §2 现在要求"模式间必须不同（例：PROBE-*）"，
#    所以这里只校验形态（PROBE- 前缀 + 非空），"两模式不同"由 §[9] 的 peer 比对负责。
EXPECT_STRINGS_BASE = {
    0: b"\x09\x04",
    1: b"MeidaP4",
    2: b"MeidaP4 Probe",
    4: b"MeidaP4 WebUSB",
}
EXPECT_STRINGS_MODE_B_EXTRA = {5: b"MeidaP4 Storage"}
# 条目**总数**（含不钉死内容的 idx 3）：模式 A=0..4 共 5 条，模式 B=0..5 共 6 条
EXPECT_STR_COUNT = {"a": 5, "b": 6}
SERIAL_PREFIX = b"PROBE-"
STR_TABLE_MAX = 8                 # esp_tinyusb USB_STRING_DESCRIPTOR_ARRAY_SIZE

# USB 描述符类型码
DT_DEVICE = 0x01
DT_CONFIG = 0x02
DT_STRING = 0x03
DT_INTERFACE = 0x04
DT_ENDPOINT = 0x05
DT_DEVICE_QUALIFIER = 0x06
DT_BOS = 0x0F
DT_DEVICE_CAPABILITY = 0x10

XFER_CONTROL, XFER_ISO, XFER_BULK, XFER_INTERRUPT = 0, 1, 2, 3
XFER_NAME = {0: "control", 1: "isochronous", 2: "bulk", 3: "interrupt"}

# 契约明确"不做"的类，必须缺席（probe-plan §1）
FORBIDDEN_ITF_CLASSES = {
    0x01: "audio", 0x02: "CDC-comm", 0x03: "HID", 0x07: "printer",
    0x08: "MSC", 0x0A: "CDC-data", 0x0B: "smart-card", 0x0E: "video(UVC)",
}

# P4 HS 口硬件上限，出处 managed_components/espressif__tinyusb/src/portable/
# synopsys/dwc2/dwc2_esp32.h:80 -> ep_count=16, ep_in_count=8（IN 含 EP0）
DWC2_EP_COUNT = 16
DWC2_EP_IN_COUNT = 8

# 端点预算推演：加 NCM（契约 §1 推演段）
NCM_PROJECTION_IN = 2   # EP4 interrupt IN + EP5 bulk IN
NCM_PROJECTION_OUT = 1  # EP5 bulk OUT
NCM_PROJECTION_EP_TOTAL = 3

SYM_DEVICE = "s_desc_device"
SYM_QUALIFIER = "s_desc_qualifier"
SYM_CFG_FS = "s_desc_cfg_fs"
SYM_CFG_HS = "s_desc_cfg_hs"
SYM_BOS = "s_desc_bos"
SYM_MSOS = "s_msos20_desc_set"
SYM_STRTAB = "s_string_desc"
SYM_LANGID = "s_langid_en_us"

SYM_BOS_CB = "tud_descriptor_bos_cb"
SYM_VENDOR_CTRL = "tud_vendor_control_xfer_cb"
SYM_VENDOR_RX = "tud_vendor_rx_cb"
SYM_MOUNT = "tud_mount_cb"
SYM_UMOUNT = "tud_umount_cb"

REQUIRED_SYMS = (SYM_DEVICE, SYM_CFG_FS, SYM_CFG_HS, SYM_BOS, SYM_MSOS, SYM_STRTAB)


def _force_utf8_stdio() -> None:
    """报告里有中文；在 cp936 控制台上必须显式切 UTF-8，否则输出乱码。"""
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")  # type: ignore[attr-defined]
        except (AttributeError, ValueError):  # pragma: no cover - 老 Python / 已重定向
            pass


# ---------------------------------------------------------------------------
# 工具/输入错误
# ---------------------------------------------------------------------------


class ToolError(Exception):
    """工具或输入问题 -> 退出码 2。"""


# ---------------------------------------------------------------------------
# ELF 读取
# ---------------------------------------------------------------------------


@dataclass
class Section:
    name: str
    vma: int
    size: int


@dataclass
class Symbol:
    name: str
    addr: int
    size: int
    kind: str  # 'T'/'t' 文本，'r' 只读数据，'d' 可写数据 ...


def _run(cmd: Sequence[str]) -> str:
    try:
        proc = subprocess.run(
            list(cmd),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
    except OSError as exc:  # pragma: no cover - 环境问题
        raise ToolError("无法执行 %s: %s" % (cmd[0], exc)) from exc
    out = proc.stdout.decode("utf-8", errors="replace")
    if proc.returncode != 0:
        raise ToolError("命令失败 (rc=%d): %s\n%s" % (proc.returncode, " ".join(cmd), out.strip()))
    return out


def find_toolchain(explicit: Optional[str]) -> str:
    candidates: List[str] = []
    if explicit:
        candidates.append(explicit)
    env = os.environ.get("DESC_LINT_TOOLCHAIN")
    if env:
        candidates.append(env)

    which = shutil.which("riscv32-esp-elf-nm")
    if which:
        candidates.append(os.path.dirname(which))

    for pattern in (
        r"C:\Espressif\tools\riscv32-esp-elf\*\riscv32-esp-elf\bin",
        r"C:\Espressif\tools\riscv32-esp-elf\*\riscv32-esp-elf\riscv32-esp-elf\bin",
        "/opt/esp/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin",
        os.path.expanduser("~/.espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin"),
    ):
        candidates.extend(sorted(glob.glob(pattern), reverse=True))

    for cand in candidates:
        nm = os.path.join(cand, "riscv32-esp-elf-nm.exe")
        oc = os.path.join(cand, "riscv32-esp-elf-objcopy.exe")
        if not os.path.isfile(nm):
            nm = os.path.join(cand, "riscv32-esp-elf-nm")
            oc = os.path.join(cand, "riscv32-esp-elf-objcopy")
        if os.path.isfile(nm) and os.path.isfile(oc):
            return cand
    raise ToolError(
        "找不到 riscv32-esp-elf 工具链（需要 nm + objcopy）。"
        "用 --toolchain <dir> 或 DESC_LINT_TOOLCHAIN 指定。"
    )


class ElfImage:
    """按 VMA 索引的 ELF 只读视图（nm 符号表 + objcopy 节区字节）。"""

    def __init__(self, elf: str, toolchain: str) -> None:
        if not os.path.isfile(elf):
            raise ToolError("ELF 不存在: %s" % elf)
        self.elf = os.path.abspath(elf)
        self.nm = os.path.join(toolchain, "riscv32-esp-elf-nm.exe")
        self.objcopy = os.path.join(toolchain, "riscv32-esp-elf-objcopy.exe")
        if not os.path.isfile(self.nm):
            self.nm = os.path.join(toolchain, "riscv32-esp-elf-nm")
        if not os.path.isfile(self.objcopy):
            self.objcopy = os.path.join(toolchain, "riscv32-esp-elf-objcopy")
        self.symbols: Dict[str, Symbol] = {}
        self._sections: List[Section] = []
        self._blob: Dict[str, bytes] = {}
        self._load_symbols()
        self._load_sections()

    # -- 符号 ---------------------------------------------------------------
    def _load_symbols(self) -> None:
        out = _run([self.nm, "-S", "--defined-only", self.elf])
        # `4003b318 00000012 r s_desc_device`
        pat_sized = re.compile(r"^([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+(\S)\s+(\S+)$")
        pat_plain = re.compile(r"^([0-9a-fA-F]+)\s+(\S)\s+(\S+)$")
        for line in out.splitlines():
            m = pat_sized.match(line.strip())
            if m:
                addr, size, kind, name = m.groups()
                self.symbols[name] = Symbol(name, int(addr, 16), int(size, 16), kind)
                continue
            m = pat_plain.match(line.strip())
            if m:
                addr, kind, name = m.groups()
                self.symbols[name] = Symbol(name, int(addr, 16), 0, kind)

    def require_symbol(self, name: str) -> Symbol:
        if name not in self.symbols:
            raise ToolError(
                "ELF 里找不到符号 %r —— 描述符数组可能被改名/内联/优化掉了，"
                "无法做静态校验（这本身就是要报给 Lead 的问题）。" % name
            )
        sym = self.symbols[name]
        if sym.size == 0:
            raise ToolError("符号 %r 的大小是 0，无法确定数组长度。" % name)
        return sym

    # -- 节区 ---------------------------------------------------------------
    def _load_sections(self) -> None:
        out = _run([self.objcopy.replace("objcopy", "objdump"), "-h", self.elf])
        for line in out.splitlines():
            parts = line.split()
            if len(parts) < 6 or not parts[0].isdigit():
                continue
            name, size, vma = parts[1], parts[2], parts[3]
            try:
                self._sections.append(Section(name, int(vma, 16), int(size, 16)))
            except ValueError:
                continue
        if not self._sections:
            raise ToolError("objdump -h 没解析出任何节区，ELF 格式可能不对。")

    def section_of(self, addr: int) -> Optional[Section]:
        for sec in self._sections:
            if sec.vma <= addr < sec.vma + sec.size:
                return sec
        return None

    def _section_bytes(self, sec: Section) -> bytes:
        if sec.name in self._blob:
            return self._blob[sec.name]
        tmp = os.path.join(
            os.environ.get("TEMP", os.environ.get("TMPDIR", ".")),
            "desclint_%s_%d.bin" % (re.sub(r"\W+", "_", sec.name), abs(hash(sec.name)) % 10**6),
        )
        try:
            _run([self.objcopy, "-O", "binary", "--only-section=%s" % sec.name, self.elf, tmp])
            with open(tmp, "rb") as fh:
                data = fh.read()
        except ToolError as exc:
            raise ToolError("取节区 %s 字节失败: %s" % (sec.name, exc)) from exc
        finally:
            try:
                os.remove(tmp)
            except OSError:
                pass
        self._blob[sec.name] = data
        return data

    def read(self, addr: int, size: int) -> bytes:
        """按虚拟地址读连续字节（允许跨节区，但每次都重新定位）。"""
        out = bytearray()
        cur = addr
        remaining = size
        while remaining > 0:
            sec = self.section_of(cur)
            if sec is None:
                raise ToolError("地址 0x%08X 不落在任何已加载节区里，无法读描述符。" % cur)
            data = self._section_bytes(sec)
            off = cur - sec.vma
            if off >= len(data):
                raise ToolError("节区 %s 的 objcopy 输出(%d B)短于符号偏移 %d。" % (sec.name, len(data), off))
            take = min(remaining, len(data) - off)
            out += data[off:off + take]
            cur += take
            remaining -= take
        return bytes(out)

    def read_u32(self, addr: int) -> int:
        return struct.unpack("<I", self.read(addr, 4))[0]

    def read_cstr(self, addr: int, limit: int = 128) -> bytes:
        raw = self.read(addr, limit)
        nul = raw.find(b"\x00")
        return raw if nul < 0 else raw[:nul]

    def disassemble(self, sym: Symbol) -> str:
        objdump = self.objcopy.replace("objcopy", "objdump")
        if not os.path.isfile(objdump):
            return ""
        try:
            return _run([
                objdump, "-d",
                "--start-address=0x%X" % sym.addr,
                "--stop-address=0x%X" % (sym.addr + max(sym.size, 4)),
                self.elf,
            ])
        except ToolError:
            return ""

    def map_owner(self, symbol: str) -> Optional[str]:
        """从 <elf>.map 里找出符号由哪个归档成员定义（回调归属证据）。"""
        mapfile = _map_path(self.elf)
        if not os.path.isfile(mapfile):
            return None
        pat = re.compile(r"^\s*%s\s+(\S.*)$" % re.escape(symbol))
        try:
            with open(mapfile, "r", encoding="utf-8", errors="replace") as fh:
                for line in fh:
                    m = pat.match(line.rstrip("\n"))
                    if m:
                        return m.group(1).strip()
        except OSError:
            return None
        return None


# ---------------------------------------------------------------------------
# 断言收集
# ---------------------------------------------------------------------------


class Report:
    def __init__(self, title: str, strict_baseline: bool = False) -> None:
        self.title = title
        self.lines: List[str] = []
        self.checks: List[Tuple[str, bool, str]] = []
        self.skipped: List[Tuple[str, str]] = []
        self.warnings: List[Tuple[str, str]] = []
        self.strict_baseline = strict_baseline

    def out(self, text: str = "") -> None:
        self.lines.append(text)

    def check(self, name: str, ok: bool, detail: str = "") -> bool:
        self.checks.append((name, bool(ok), detail))
        return bool(ok)

    def skip(self, name: str, why: str) -> None:
        """本条断言没有执行（缺输入/缺 peer 等）。**不算通过也不算失败**，必须显式列出。"""
        self.skipped.append((name, why))
        self.out("    ⊘ 跳过断言: %s  --  %s" % (name, why))

    def warn(self, name: str, detail: str) -> None:
        """契约明确允许的"漂移"（例如 bcdDevice 按契约可以递增）→ 记 WARN 并打印。

        默认**不影响退出码**（否则就是又一条假 FAIL）；`--strict-baseline` 会把它提升为 FAIL。
        """
        self.warnings.append((name, detail))
        self.out("    ⚠ WARN: %s  --  %s%s"
                 % (name, detail, "  [--strict-baseline 下会算 FAIL]" if self.strict_baseline else ""))

    @property
    def failures(self) -> List[Tuple[str, bool, str]]:
        if self.strict_baseline:
            return [c for c in self.checks if not c[1]] + [
                ("[strict-baseline] " + w[0], False, w[1]) for w in self.warnings
            ]
        return [c for c in self.checks if not c[1]]

    def render(self) -> str:
        return "\n".join(self.lines)

    def json_summary(self) -> dict:
        return {
            "title": self.title,
            "total": len(self.checks),
            "passed": len(self.checks) - len([c for c in self.checks if not c[1]]),
            "failed": [
                {"assertion": f[0], "detail": f[2]} for f in self.failures
            ],
            "warnings": [{"assertion": w[0], "detail": w[1]} for w in self.warnings],
            "skipped": [{"assertion": s[0], "why": s[1]} for s in self.skipped],
            "strict_baseline": self.strict_baseline,
        }


def hexs(data: bytes) -> str:
    return " ".join("%02X" % b for b in data)


# ---------------------------------------------------------------------------
# 描述符解析
# ---------------------------------------------------------------------------


def le16(data: bytes, off: int) -> int:
    return data[off] | (data[off + 1] << 8)


def le32(data: bytes, off: int) -> int:
    return struct.unpack_from("<I", data, off)[0]


@dataclass
class Endpoint:
    addr: int
    xfer: int
    mps: int
    interval: int
    offset: int = -1

    @property
    def direction(self) -> str:
        return "IN" if self.addr & 0x80 else "OUT"

    @property
    def number(self) -> int:
        return self.addr & 0x0F


@dataclass
class Interface:
    number: int
    alt: int
    num_ep: int
    itf_class: int
    subclass: int
    protocol: int
    stridx: int
    offset: int = -1
    endpoints: List[Endpoint] = field(default_factory=list)


@dataclass
class Config:
    raw: bytes
    total_len: int
    num_interfaces: int
    config_value: int
    attributes: int
    max_power: int
    interfaces: List[Interface]
    descriptors: List[Tuple[int, int, int]]  # (offset, bLength, bDescriptorType)
    trailing: int  # 链结束位置与 wTotalLength 的差值


def parse_config(raw: bytes) -> Config:
    if len(raw) < 9:
        raise ToolError("配置描述符太短 (%d B)" % len(raw))
    total_len = le16(raw, 2)
    cfg = Config(
        raw=raw,
        total_len=total_len,
        num_interfaces=raw[4],
        config_value=raw[5],
        attributes=raw[7],
        max_power=raw[8],
        interfaces=[],
        descriptors=[(0, raw[0], raw[1])],
        trailing=len(raw) - total_len,
    )
    if total_len > len(raw):
        raise ToolError("wTotalLength=%d 超过数组实际长度 %d" % (total_len, len(raw)))

    pos = raw[0] if raw[0] >= 2 else 9
    cur: Optional[Interface] = None
    while pos < total_len:
        blen = raw[pos]
        if blen < 2:
            raise ToolError("偏移 %d 的 bLength=%d 非法" % (pos, blen))
        if pos + blen > total_len:
            raise ToolError("偏移 %d 的描述符 (bLength=%d) 越过 wTotalLength=%d" % (pos, blen, total_len))
        btype = raw[pos + 1]
        cfg.descriptors.append((pos, blen, btype))
        if btype == DT_INTERFACE:
            cur = Interface(
                number=raw[pos + 2], alt=raw[pos + 3], num_ep=raw[pos + 4],
                itf_class=raw[pos + 5], subclass=raw[pos + 6], protocol=raw[pos + 7],
                stridx=raw[pos + 8], offset=pos,
            )
            cfg.interfaces.append(cur)
        elif btype == DT_ENDPOINT:
            if cur is None:
                raise ToolError("偏移 %d 的端点描述符出现在任何接口之前" % pos)
            cur.endpoints.append(Endpoint(
                addr=raw[pos + 2], xfer=raw[pos + 3] & 0x03,
                mps=le16(raw, pos + 4), interval=raw[pos + 6], offset=pos,
            ))
        pos += blen
    return cfg


def parse_bos(raw: bytes) -> List[Tuple[int, int, dict]]:
    """返回 [(offset, bLength, {字段...}), ...]，第一项是 BOS 头。"""
    caps: List[Tuple[int, int, dict]] = [(0, raw[0], {
        "bDescriptorType": raw[1],
        "wTotalLength": le16(raw, 2),
        "bNumDeviceCaps": raw[4],
    })]
    pos = raw[0]
    while pos < len(raw):
        blen = raw[pos]
        if blen < 3:
            raise ToolError("BOS 偏移 %d 的 bLength=%d 非法" % (pos, blen))
        info = {
            "bDescriptorType": raw[pos + 1],
            "bDevCapabilityType": raw[pos + 2] if blen > 2 else None,
        }
        if blen >= 20:
            info["uuid"] = raw[pos + 4:pos + 20]
        if blen >= 28:
            info["dwWindowsVersion"] = le32(raw, pos + 20)
            info["wMSOSDescriptorSetTotalLength"] = le16(raw, pos + 24)
            info["bVendorCode"] = raw[pos + 26]
            info["bAltEnumCode"] = raw[pos + 27]
        elif blen >= 24:
            info["bcdVersion"] = le16(raw, pos + 20)
            info["bVendorCode"] = raw[pos + 22]
            info["iLandingPage"] = raw[pos + 23]
        caps.append((pos, blen, info))
        pos += blen
        if pos > len(raw):
            raise ToolError("BOS 描述符链越过数组末尾")
    return caps


def parse_msos20(raw: bytes) -> List[Tuple[int, int, int]]:
    """返回 [(offset, wLength, wDescriptorType), ...]（含 10 B Set Header）。"""
    items: List[Tuple[int, int, int]] = []
    pos = 0
    while pos < len(raw):
        if pos + 4 > len(raw):
            raise ToolError("MS OS 2.0 描述符集在偏移 %d 处被截断" % pos)
        wlen = le16(raw, pos)
        wtype = le16(raw, pos + 2)
        if wlen < 4:
            raise ToolError("MS OS 2.0 偏移 %d 的 wLength=%d 非法" % (pos, wlen))
        items.append((pos, wlen, wtype))
        pos += wlen
    return items


# ---------------------------------------------------------------------------
# 端点期望
# ---------------------------------------------------------------------------


def expected_endpoints(mode: str, high_speed: bool) -> List[Tuple[int, int, int]]:
    """契约 §1 + 裁决修订 1：(addr, xfer, mps)"""
    bulk = BULK_MPS_HS if high_speed else BULK_MPS_FS
    if mode == "a":
        return [(0x01, XFER_BULK, bulk), (0x81, XFER_BULK, bulk)]
    return [
        (0x81, XFER_INTERRUPT, MTP_EVT_MPS),
        (0x02, XFER_BULK, bulk),
        (0x82, XFER_BULK, bulk),
        (0x03, XFER_BULK, bulk),
        (0x83, XFER_BULK, bulk),
    ]


def expected_interfaces(mode: str) -> List[dict]:
    if mode == "a":
        return [{"number": 0, "num_ep": 2, "itf_class": ITF_CLASS_VENDOR, "stridx": 4,
                 "name": "Vendor(WebUSB 回环)"}]
    return [
        {"number": 0, "num_ep": 3, "itf_class": ITF_CLASS_IMAGE, "stridx": 5, "name": "MTP"},
        {"number": 1, "num_ep": 2, "itf_class": ITF_CLASS_VENDOR, "stridx": 4,
         "name": "Vendor(WebUSB 回环)"},
    ]


def vendor_itf_number(mode: str) -> int:
    return 0 if mode == "a" else 1


# ---------------------------------------------------------------------------
# 校验主体
# ---------------------------------------------------------------------------


def lint(elf: ElfImage, mode: str, rep: Report) -> None:
    # ---- [0] 符号表 -------------------------------------------------------
    rep.out("[0] 符号表（riscv32-esp-elf-nm -S）")
    syms = {name: elf.require_symbol(name) for name in REQUIRED_SYMS}
    syms[SYM_QUALIFIER] = elf.symbols.get(SYM_QUALIFIER)
    for name in REQUIRED_SYMS:
        s = syms[name]
        sec = elf.section_of(s.addr)
        rep.out("    %-22s addr=0x%08X size=%-4d type=%s sec=%s"
                % (name, s.addr, s.size, s.kind, sec.name if sec else "?"))
    extra = [n for n in (SYM_LANGID,) if n in elf.symbols]
    for name in extra:
        s = elf.symbols[name]
        rep.out("    %-22s addr=0x%08X size=%-4d type=%s" % (name, s.addr, s.size, s.kind))
    rep.out("")

    # ---- 读出原始字节 -----------------------------------------------------
    dev_raw = elf.read(syms[SYM_DEVICE].addr, syms[SYM_DEVICE].size)
    fs_raw = elf.read(syms[SYM_CFG_FS].addr, syms[SYM_CFG_FS].size)
    hs_raw = elf.read(syms[SYM_CFG_HS].addr, syms[SYM_CFG_HS].size)
    bos_raw = elf.read(syms[SYM_BOS].addr, syms[SYM_BOS].size)
    msos_raw = elf.read(syms[SYM_MSOS].addr, syms[SYM_MSOS].size)

    # ---- [1] Device descriptor -------------------------------------------
    rep.out("[1] Device Descriptor  (%d B)" % len(dev_raw))
    rep.out("    %s" % hexs(dev_raw))
    d_bcd_usb = le16(dev_raw, 2)
    d_vid = le16(dev_raw, 8)
    d_pid = le16(dev_raw, 10)
    d_bcd_dev = le16(dev_raw, 12)
    rep.out("    bLength=%d  bDescriptorType=0x%02X  bcdUSB=0x%04X  class=0x%02X/0x%02X/0x%02X"
            % (dev_raw[0], dev_raw[1], d_bcd_usb, dev_raw[4], dev_raw[5], dev_raw[6]))
    rep.out("    bMaxPacketSize0=%d  idVendor=0x%04X  idProduct=0x%04X  bcdDevice=0x%04X"
            % (dev_raw[7], d_vid, d_pid, d_bcd_dev))
    rep.out("    iManufacturer=%d  iProduct=%d  iSerialNumber=%d  bNumConfigurations=%d"
            % (dev_raw[14], dev_raw[15], dev_raw[16], dev_raw[17]))
    rep.check("device.bLength==18", dev_raw[0] == 18, "bLength=%d" % dev_raw[0])
    rep.check("device.bDescriptorType==DEVICE", dev_raw[1] == DT_DEVICE, "type=0x%02X" % dev_raw[1])
    rep.check("device.bcdUSB>=0x0210", d_bcd_usb >= MIN_BCD_USB, "bcdUSB=0x%04X" % d_bcd_usb)
    want_class = EXPECT_DEV_CLASS[mode]
    rep.check("device.bDeviceClass/SubClass/Protocol==%02X/%02X/%02X (mode %s: %s)"
              % (want_class[0], want_class[1], want_class[2], mode, DEV_CLASS_DESC[mode]),
              (dev_raw[4], dev_raw[5], dev_raw[6]) == want_class,
              "实际 0x%02X/0x%02X/0x%02X" % (dev_raw[4], dev_raw[5], dev_raw[6]))
    rep.check("device.bMaxPacketSize0 in {8,16,32,64}", dev_raw[7] in (8, 16, 32, 64),
              "bMaxPacketSize0=%d" % dev_raw[7])
    rep.check("device.idVendor==0x303A", d_vid == EXPECT_VID, "idVendor=0x%04X" % d_vid)
    rep.check("device.idProduct==0x%04X (mode %s)" % (EXPECT_PID[mode], mode),
              d_pid == EXPECT_PID[mode], "idProduct=0x%04X" % d_pid)
    rep.check("device.bcdDevice != 0（契约 §2：必须是有效递增值）",
              d_bcd_dev != 0, "bcdDevice=0x%04X" % d_bcd_dev)
    if d_bcd_dev != EXPECT_BCD_DEVICE_BASELINE:
        rep.warn("device.bcdDevice 与冻结基线 0x%04X 不同" % EXPECT_BCD_DEVICE_BASELINE,
                 "实测 0x%04X。契约 §2 说\"每次改动描述符内容就递增\"，"
                 "所以这**可能是有意递增**（用于强制 Windows 造新实例），"
                 "因此默认不算 FAIL；若本次冻结基线不允许变动，请用 --strict-baseline。"
                 % d_bcd_dev)
    rep.check("device string idx 1/2/3", (dev_raw[14], dev_raw[15], dev_raw[16]) == (1, 2, 3),
              "iMan=%d iProd=%d iSer=%d" % (dev_raw[14], dev_raw[15], dev_raw[16]))
    rep.check("device.bNumConfigurations==1", dev_raw[17] == 1, "bNumConfigurations=%d" % dev_raw[17])
    rep.out("")

    # qualifier 的类字段必须与 device 描述符逐字节一致（USB 2.0 §9.6.2）
    if syms.get(SYM_QUALIFIER) is not None:
        q = syms[SYM_QUALIFIER]
        q_raw = elf.read(q.addr, q.size)
        rep.out("[1b] Device Qualifier  (%d B)  %s" % (len(q_raw), hexs(q_raw)))
        if len(q_raw) >= 10:
            rep.check("qualifier 类字段==device 类字段",
                      (q_raw[4], q_raw[5], q_raw[6]) == (dev_raw[4], dev_raw[5], dev_raw[6]),
                      "qualifier 0x%02X/0x%02X/0x%02X vs device 0x%02X/0x%02X/0x%02X"
                      % (q_raw[4], q_raw[5], q_raw[6], dev_raw[4], dev_raw[5], dev_raw[6]))
        rep.out("")

    # ---- [2] 配置描述符 --------------------------------------------------
    for label, raw, hs in (("FS", fs_raw, False), ("HS", hs_raw, True)):
        speed = "HS" if hs else "FS"
        rep.out("[2.%s] Configuration %s  (%d B)" % (speed, speed, len(raw)))
        cfg = parse_config(raw)
        rep.out("    %s" % hexs(raw))
        rep.out("    wTotalLength=%d  bNumInterfaces=%d  bConfigurationValue=%d  bmAttributes=0x%02X  bMaxPower=%d(%d mA)"
                % (cfg.total_len, cfg.num_interfaces, cfg.config_value, cfg.attributes,
                   cfg.max_power, cfg.max_power * 2))
        for off, blen, btype in cfg.descriptors:
            if btype == DT_INTERFACE:
                itf = next(i for i in cfg.interfaces if i.offset == off)
                rep.out("      @%-3d len=%-2d type=0x04 INTERFACE  num=%d alt=%d eps=%d class=0x%02X/0x%02X/0x%02X str=%d"
                        % (off, blen, itf.number, itf.alt, itf.num_ep, itf.itf_class,
                           itf.subclass, itf.protocol, itf.stridx))
            elif btype == DT_ENDPOINT:
                ep = next(e for i in cfg.interfaces for e in i.endpoints if e.offset == off)
                rep.out("      @%-3d len=%-2d type=0x05 ENDPOINT   0x%02X %-9s %s mps=%-4d interval=%d"
                        % (off, blen, ep.addr, XFER_NAME[ep.xfer], ep.direction, ep.mps, ep.interval))
            else:
                rep.out("      @%-3d len=%-2d type=0x%02X" % (off, blen, btype))

        rep.check("%s config.bLength==9" % speed, raw[0] == 9, "bLength=%d" % raw[0])
        rep.check("%s config.bDescriptorType==CONFIG" % speed, raw[1] == DT_CONFIG, "0x%02X" % raw[1])
        rep.check("%s 描述符链精确落在 wTotalLength 上" % speed, cfg.trailing == 0,
                  "数组 %d B vs wTotalLength %d B（差 %d）" % (len(raw), cfg.total_len, cfg.trailing))
        rep.check("%s bNumInterfaces==实际接口数" % speed,
                  cfg.num_interfaces == len(cfg.interfaces),
                  "声明 %d，实际 %d" % (cfg.num_interfaces, len(cfg.interfaces)))

        exp_eps = expected_endpoints(mode, hs)
        exp_itfs = expected_interfaces(mode)
        flat: List[Endpoint] = [ep for itf in cfg.interfaces for ep in itf.endpoints]

        rep.check("%s 接口数==%d" % (speed, len(exp_itfs)),
                  len(cfg.interfaces) == len(exp_itfs),
                  "实际 %d" % len(cfg.interfaces))
        for idx, exp in enumerate(exp_itfs):
            if idx >= len(cfg.interfaces):
                rep.check("%s 存在接口 #%d" % (speed, idx), False, "接口缺失")
                break
            itf = cfg.interfaces[idx]
            rep.check("%s IF%d 接口号==%d" % (speed, idx, exp["number"]), itf.number == exp["number"],
                      "实际 %d" % itf.number)
            rep.check("%s IF%d bNumEndpoints==%d" % (speed, idx, exp["num_ep"]),
                      itf.num_ep == exp["num_ep"] and len(itf.endpoints) == exp["num_ep"],
                      "声明 %d，实际描述符 %d" % (itf.num_ep, len(itf.endpoints)))
            rep.check("%s IF%d 接口类==0x%02X (%s)" % (speed, idx, exp["itf_class"], exp["name"]),
                      itf.itf_class == exp["itf_class"], "实际 0x%02X" % itf.itf_class)
            rep.check("%s IF%d iInterface==%d" % (speed, idx, exp["stridx"]),
                      itf.stridx == exp["stridx"], "实际 %d" % itf.stridx)

        if mode == "b" and cfg.interfaces:
            mtp = cfg.interfaces[0]
            rep.check("%s MTP 接口 class/subclass/protocol==06/01/01 (IMAGE/PTP/PIMA)"
                      % speed,
                      (mtp.itf_class, mtp.subclass, mtp.protocol)
                      == (ITF_CLASS_IMAGE, ITF_SUBCLASS_STILL_IMAGE, ITF_PROTOCOL_PIMA_15470),
                      "实际 0x%02X/0x%02X/0x%02X" % (mtp.itf_class, mtp.subclass, mtp.protocol))

        rep.check("%s 端点清单与契约 §1 逐项一致" % speed,
                  [(e.addr, e.xfer, e.mps) for e in flat] == exp_eps,
                  "实际 %s / 期望 %s"
                  % ([(hex(e.addr), XFER_NAME[e.xfer], e.mps) for e in flat],
                     [(hex(a), XFER_NAME[x], m) for a, x, m in exp_eps]))

        # 分方向 EP 号唯一
        in_addrs = [e.addr for e in flat if e.addr & 0x80]
        out_addrs = [e.addr for e in flat if not (e.addr & 0x80)]
        rep.check("%s IN 端点地址无重复" % speed, len(in_addrs) == len(set(in_addrs)), str(in_addrs))
        rep.check("%s OUT 端点地址无重复" % speed, len(out_addrs) == len(set(out_addrs)), str(out_addrs))
        rep.check("%s 没有端点号 0（EP0 归控制端点）" % speed,
                  all(e.number != 0 for e in flat), str([hex(e.addr) for e in flat]))

        # 类白名单：不得引入 UVC/CDC/MSC/HID...
        bad = [i for i in cfg.interfaces if i.itf_class in FORBIDDEN_ITF_CLASSES]
        rep.check("%s 未引入 UVC/CDC/HID/MSC 等被砍掉的类" % speed, not bad,
                  "; ".join("IF%d=0x%02X(%s)" % (i.number, i.itf_class,
                                                 FORBIDDEN_ITF_CLASSES[i.itf_class]) for i in bad))

        # 端点预算
        n_in = len(in_addrs)
        rep.out("    [端点预算 %s] 用户 IN=%d -> 含 EP0 共 %d/%d ；总端点=%d -> 含 EP0 共 %d/%d"
                % (speed, n_in, n_in + 1, DWC2_EP_IN_COUNT, len(flat), len(flat) + 1, DWC2_EP_COUNT))
        rep.check("%s IN 端点(含 EP0)<=%d" % (speed, DWC2_EP_IN_COUNT),
                  n_in + 1 <= DWC2_EP_IN_COUNT, "IN=%d +EP0=%d" % (n_in, n_in + 1))
        rep.check("%s 总端点(含 EP0)<=%d" % (speed, DWC2_EP_COUNT),
                  len(flat) + 1 <= DWC2_EP_COUNT, "total=%d +EP0=%d" % (len(flat), len(flat) + 1))
        rep.out("")

    # ---- [3] BOS ----------------------------------------------------------
    rep.out("[3] BOS Descriptor  (%d B)" % len(bos_raw))
    rep.out("    %s" % hexs(bos_raw))
    caps = parse_bos(bos_raw)
    head = caps[0][2]
    rep.out("    @0   len=%d type=0x%02X BOS          wTotalLength=%d bNumDeviceCaps=%d"
            % (bos_raw[0], bos_raw[1], head["wTotalLength"], head["bNumDeviceCaps"]))
    cap_sum = 0
    for off, blen, info in caps[1:]:
        cap_sum += blen
        rep.out("    @%-3d len=%-2d type=0x%02X DEVCAP       bDevCapabilityType=0x%02X"
                % (off, blen, info["bDescriptorType"], info["bDevCapabilityType"]))
        if "uuid" in info:
            rep.out("         uuid=%s" % hexs(info["uuid"]))
        if "bcdVersion" in info:
            rep.out("         bcdVersion=0x%04X bVendorCode=0x%02X iLandingPage=%d"
                    % (info["bcdVersion"], info["bVendorCode"], info["iLandingPage"]))
        if "dwWindowsVersion" in info:
            rep.out("         dwWindowsVersion=0x%08X wMSOSDescriptorSetTotalLength=%d bVendorCode=0x%02X bAltEnumCode=%d"
                    % (info["dwWindowsVersion"], info["wMSOSDescriptorSetTotalLength"],
                       info["bVendorCode"], info["bAltEnumCode"]))

    rep.check("BOS.bLength==5", bos_raw[0] == 5, "bLength=%d" % bos_raw[0])
    rep.check("BOS.bDescriptorType==0x0F", bos_raw[1] == DT_BOS, "0x%02X" % bos_raw[1])
    rep.check("BOS.wTotalLength==数组实际长度", head["wTotalLength"] == len(bos_raw),
              "字段 %d vs 数组 %d" % (head["wTotalLength"], len(bos_raw)))
    rep.check("BOS.wTotalLength==各能力长度累加+头", head["wTotalLength"] == 5 + cap_sum,
              "5 + %d = %d vs 字段 %d" % (cap_sum, 5 + cap_sum, head["wTotalLength"]))
    rep.check("BOS.bNumDeviceCaps==2", head["bNumDeviceCaps"] == 2, "=%d" % head["bNumDeviceCaps"])
    rep.check("BOS 能力实际个数==bNumDeviceCaps", len(caps) - 1 == head["bNumDeviceCaps"],
              "实际 %d" % (len(caps) - 1))

    webusb_cap = next((c for c in caps[1:] if c[2].get("uuid") == WEBUSB_UUID), None)
    msos_cap = next((c for c in caps[1:] if c[2].get("uuid") == MSOS20_UUID), None)
    rep.check("BOS 含 WebUSB 平台能力 UUID {3408b638-...}", webusb_cap is not None, "未找到")
    rep.check("BOS 含 MS OS 2.0 平台能力 UUID {d8dd60df-...}", msos_cap is not None, "未找到")
    if webusb_cap:
        off, blen, info = webusb_cap
        rep.check("WebUSB 能力 bLength==24", blen == 24, "=%d" % blen)
        rep.check("WebUSB 能力 bDescriptorType==0x10", info["bDescriptorType"] == DT_DEVICE_CAPABILITY,
                  "0x%02X" % info["bDescriptorType"])
        rep.check("WebUSB 能力 bDevCapabilityType==0x05(PLATFORM)", info["bDevCapabilityType"] == 0x05,
                  "0x%02X" % info["bDevCapabilityType"])
        rep.check("WebUSB bcdVersion==0x0100", info.get("bcdVersion") == 0x0100,
                  "0x%04X" % info.get("bcdVersion", -1))
        rep.check("WebUSB bVendorCode==0x01", info.get("bVendorCode") == WEBUSB_VENDOR_CODE,
                  "0x%02X" % info.get("bVendorCode", -1))
        rep.check("WebUSB iLandingPage==%d（契约 §2：不做着陆页，必须 0）"
                  % EXPECT_WEBUSB_LANDING_PAGE,
                  info.get("iLandingPage") == EXPECT_WEBUSB_LANDING_PAGE,
                  "实际 %s" % info.get("iLandingPage"))
    if msos_cap:
        off, blen, info = msos_cap
        rep.check("MS OS 2.0 能力 bLength==28", blen == 28, "=%d" % blen)
        rep.check("MS OS 2.0 能力 bDescriptorType==0x10",
                  info["bDescriptorType"] == DT_DEVICE_CAPABILITY, "0x%02X" % info["bDescriptorType"])
        rep.check("MS OS 2.0 能力 bDevCapabilityType==0x05(PLATFORM)",
                  info["bDevCapabilityType"] == 0x05, "0x%02X" % info["bDevCapabilityType"])
        rep.check("MS OS 2.0 dwWindowsVersion==0x06030000",
                  info.get("dwWindowsVersion") == MSOS20_WINDOWS_VERSION,
                  "0x%08X" % info.get("dwWindowsVersion", -1))
        rep.check("MS OS 2.0 bVendorCode==0x02", info.get("bVendorCode") == MSOS20_VENDOR_CODE,
                  "0x%02X" % info.get("bVendorCode", -1))
        rep.check("MS OS 2.0 wMSOSDescriptorSetTotalLength==msos 数组实际长度",
                  info.get("wMSOSDescriptorSetTotalLength") == len(msos_raw),
                  "字段 %s vs 数组 %d" % (info.get("wMSOSDescriptorSetTotalLength"), len(msos_raw)))
    rep.check("两个平台能力 bVendorCode 不同（WebUSB 0x01 / MS OS 0x02）",
              WEBUSB_VENDOR_CODE != MSOS20_VENDOR_CODE, "0x01 vs 0x02")
    rep.out("")

    # ---- [4] MS OS 2.0 描述符集 ------------------------------------------
    rep.out("[4] MS OS 2.0 descriptor set  (%d B)" % len(msos_raw))
    rep.out("    %s" % hexs(msos_raw))
    items = parse_msos20(msos_raw)
    set_total = le16(msos_raw, 8)
    subset_total = 0
    compat_payload = b""
    for off, wlen, wtype in items:
        if wtype == 0x0000:
            rep.out("    @%-3d wLength=%-4d type=0x%04X SET_HEADER        dwWindowsVersion=0x%08X wTotalLength=%d"
                    % (off, wlen, wtype, le32(msos_raw, off + 4), le16(msos_raw, off + 8)))
        elif wtype == 0x0001:
            rep.out("    @%-3d wLength=%-4d type=0x%04X CONFIG_SUBSET     bConfigurationValue=%d"
                    % (off, wlen, wtype, msos_raw[off + 4]))
        elif wtype == 0x0002:
            rep.out("    @%-3d wLength=%-4d type=0x%04X FUNCTION_SUBSET   bFirstInterfaceNumber=%d bReserved=0x%02X wSubsetLength=%d"
                    % (off, wlen, wtype, msos_raw[off + 4], msos_raw[off + 5], le16(msos_raw, off + 6)))
            subset_total = le16(msos_raw, off + 6)
        elif wtype == 0x0003:
            rep.out("    @%-3d wLength=%-4d type=0x%04X COMPATIBLE_ID     CompatibleID=%r SubCompatibleID=%s"
                    % (off, wlen, wtype, msos_raw[off + 4:off + 12], hexs(msos_raw[off + 12:off + 20])))
            compat_payload = msos_raw[off + 4:off + 20]
        elif wtype == 0x0004:
            rep.out("    @%-3d wLength=%-4d type=0x%04X REG_PROPERTY" % (off, wlen, wtype))
        else:
            rep.out("    @%-3d wLength=%-4d type=0x%04X (unknown)" % (off, wlen, wtype))

    # ---- [4a] 结构：按模式允许的子描述符序列（契约 §2 **最终定稿**）------------
    # 模式 A 的 subset header 已由"硬性要求"降为"规范推荐" → **两种形态都算通过**；
    # 当前实现属于哪一种作为**信息**输出，不再是 FAIL。
    # 模式 B 仍按契约 §2 表格要求含 Function Subset。
    got_seq = [wtype for _, _, wtype in items]
    allowed = MSOS20_ALLOWED_SEQ[mode]
    matched = next((i for i, seq in enumerate(allowed) if tuple(got_seq) == seq), None)
    rep.out("    允许的序列（契约 §2 模式 %s）：" % mode)
    for i, seq in enumerate(allowed):
        rep.out("      [%d] %s  →  %s" % (i, " → ".join("0x%04X" % t for t in seq),
                                          MSOS20_SEQ_DESC[mode].get(i, "")))
    rep.out("    实测序列：%s" % " → ".join("0x%04X" % t for t in got_seq))
    if matched is not None:
        rep.out("    ✅ 形态 = [%d] %s" % (matched, MSOS20_SEQ_DESC[mode].get(matched, "")))
    rep.check("MSOS20 子描述符序列是契约 §2 模式 %s 允许的形态之一" % mode,
              matched is not None,
              "实际 %s；允许 %s"
              % ([hex(t) for t in got_seq], [[hex(t) for t in s] for s in allowed]))

    rep.check("MSOS20 恰好 1 个 Set Header (0x0000)",
              got_seq.count(MSOS20_SET_HEADER) == 1, "%d 个" % got_seq.count(MSOS20_SET_HEADER))
    rep.check("MSOS20 恰好 1 个 CompatibleID Feature (0x0003)",
              got_seq.count(MSOS20_COMPAT_ID) == 1, "%d 个" % got_seq.count(MSOS20_COMPAT_ID))
    rep.check("MSOS20 恰好 1 个 Registry Property (0x0004)",
              got_seq.count(MSOS20_REG_PROPERTY) == 1, "%d 个" % got_seq.count(MSOS20_REG_PROPERTY))
    rep.check("MSOS20 至多 1 个 Function Subset (0x0002)",
              got_seq.count(MSOS20_FUNC_SUBSET) <= 1, "%d 个" % got_seq.count(MSOS20_FUNC_SUBSET))
    rep.check("MSOS20 至多 1 个 Configuration Subset (0x0001)",
              got_seq.count(MSOS20_CONFIG_SUBSET) <= 1, "%d 个" % got_seq.count(MSOS20_CONFIG_SUBSET))
    if SUBSET_REQUIRED[mode]:
        rep.check("MSOS20 含 Function Subset (0x0002) —— 契约 §2 模式 %s 要求" % mode,
                  got_seq.count(MSOS20_FUNC_SUBSET) == 1, "%d 个" % got_seq.count(MSOS20_FUNC_SUBSET))
    else:
        if got_seq.count(MSOS20_FUNC_SUBSET) == 0:
            rep.out("    ℹ 模式 A 未使用 subset header（= 设备级形态）。契约 §2：subset 是"
                    "\"规范推荐\"而非必需，真机已证明该形态可正常应用兼容 ID 与注册表属性。")
        else:
            rep.check("MSOS20 模式 A 使用 subset 时 Configuration/Function 必须成对出现",
                      got_seq.count(MSOS20_CONFIG_SUBSET) == 1, "%d 个" % got_seq.count(MSOS20_CONFIG_SUBSET))
            rep.check("MSOS20 模式 A 使用 Function Subset 时 bFirstInterfaceNumber==0(vendor IF)",
                      not items or all(
                          msos_raw[o + 4] == vendor_itf_number(mode)
                          for o, _, t in items if t == MSOS20_FUNC_SUBSET),
                      "见上方逐段输出")

    if items:
        hdr_off, hdr_len, hdr_type = items[0]
        rep.check("MSOS20 Set Header wLength==0x000A", hdr_len == 0x000A, "=%d" % hdr_len)
        rep.check("MSOS20 Set Header wDescriptorType==0x0000", hdr_type == 0x0000, "0x%04X" % hdr_type)
        rep.check("MSOS20 Set Header dwWindowsVersion==0x06030000",
                  le32(msos_raw, hdr_off + 4) == MSOS20_WINDOWS_VERSION,
                  "0x%08X" % le32(msos_raw, hdr_off + 4))
    config_subset = next((i for i in items if i[2] == MSOS20_CONFIG_SUBSET), None)
    if config_subset:
        off, wlen, _ = config_subset
        rep.check("MSOS20 Configuration Subset wLength==8", wlen == 8, "=%d" % wlen)
        rep.check("MSOS20 Configuration Subset bConfigurationValue==0",
                  msos_raw[off + 4] == 0, "=0x%02X" % msos_raw[off + 4])
        rep.check("MSOS20 Configuration Subset bReserved==0", msos_raw[off + 5] == 0,
                  "=0x%02X" % msos_raw[off + 5])
        following = sum(w for o, w, _ in items if o > off)
        rep.check("MSOS20 wConfigSubsetLength==8+后续子描述符长度",
                  le16(msos_raw, off + 6) == 8 + following,
                  "字段 %d vs 8+%d=%d" % (le16(msos_raw, off + 6), following, 8 + following))
    subset = next((i for i in items if i[2] == MSOS20_FUNC_SUBSET), None)
    if subset:
        off, wlen, _ = subset
        rep.check("MSOS20 Function Subset wLength==8", wlen == 8, "=%d" % wlen)
        rep.check("MSOS20 bFirstInterfaceNumber==%d (mode %s 的 vendor 接口)"
                  % (vendor_itf_number(mode), mode),
                  msos_raw[off + 4] == vendor_itf_number(mode),
                  "实际 %d" % msos_raw[off + 4])
        rep.check("MSOS20 Function Subset bReserved==0", msos_raw[off + 5] == 0,
                  "=0x%02X" % msos_raw[off + 5])
        following = sum(w for o, w, _ in items if o > off)
        rep.check("MSOS20 wSubsetLength==8+后续子描述符长度",
                  le16(msos_raw, off + 6) == 8 + following,
                  "字段 %d vs 8+%d=%d" % (le16(msos_raw, off + 6), following, 8 + following))
    else:
        # 模式 A 无 subset 是合法形态（契约 §2：规范推荐而非必需）→ 不再是 FAIL
        if SUBSET_REQUIRED[mode]:
            rep.check("MSOS20 存在 Function Subset Header (0x0002)", False, "未找到")
        else:
            rep.out("    ℹ 模式 A 无 Function Subset Header（设备级形态，契约允许）")
    compat = next((i for i in items if i[2] == MSOS20_COMPAT_ID), None)
    if compat:
        off, wlen, _ = compat
        rep.check("MSOS20 CompatibleID wLength==0x14", wlen == 0x14, "=%d" % wlen)
        rep.check('MSOS20 CompatibleID=="WINUSB\\0\\0"', msos_raw[off + 4:off + 12] == b"WINUSB\x00\x00",
                  repr(msos_raw[off + 4:off + 12]))
        rep.check("MSOS20 SubCompatibleID 全 0", msos_raw[off + 12:off + 20] == b"\x00" * 8,
                  hexs(msos_raw[off + 12:off + 20]))

    # ---- [4b] Registry Property：硬断言（契约 §2 **最终定稿 = 单数 REG_SZ**）----
    # 契约已按真机反转：**单数 `DeviceInterfaceGUID` + REG_SZ(0x0001) + nameLen 40 + dataLen 78**。
    # 复数 `DeviceInterfaceGUIDs` + REG_MULTI_SZ(0x0007) 会让 winusb.sys Code 31 → 现在是**禁止**形态，
    # 所以下面同时硬断言"必须是单数 REG_SZ 形态"和"不得是复数 MULTI_SZ 形态"。
    regprop = next((i for i in items if i[2] == MSOS20_REG_PROPERTY), None)
    if regprop is None:
        rep.check("MSOS20 存在 Registry Property Feature (0x0004)", False, "未找到")
    elif len(msos_raw) < regprop[0] + 10:
        rep.check("MSOS20 Registry Property 至少 10 B 头部", False,
                  "wLength=%d，剩余字节不足" % regprop[1])
    else:
        rp_off, rp_len, _ = regprop
        rp = msos_raw[rp_off:rp_off + rp_len]
        rp_type = le16(rp, 2)
        rp_dtype = le16(rp, 4)
        rp_namelen = le16(rp, 6)
        rp_name = rp[8:8 + rp_namelen]
        data_len_off = 8 + rp_namelen
        rp_datalen = le16(rp, data_len_off) if len(rp) >= data_len_off + 2 else -1
        rp_data = rp[data_len_off + 2:data_len_off + 2 + max(rp_datalen, 0)] if rp_datalen >= 0 else b""

        name_utf16_raw = rp_name.decode("utf-16-le", errors="replace")
        # wPropertyNameLength 是**含结尾 NUL** 的长度（19 字符 + NUL = 40 B），解码后尾随 '\x00'，
        # 比较时必须剥掉，否则会把正确的实现判成 FAIL。
        name_utf16 = name_utf16_raw.rstrip("\x00")
        data_utf16 = rp_data.decode("utf-16-le", errors="replace").rstrip("\x00")
        rep.out("    [Registry Property @%d] wLength=%d wDescriptorType=0x%04X wPropertyDataType=0x%04X (%s)"
                % (rp_off, le16(rp, 0), rp_type, rp_dtype,
                   "REG_SZ ✅契约要求" if rp_dtype == REGPROP_DATA_TYPE_REG_SZ
                   else ("REG_MULTI_SZ ❌已知会让 winusb.sys Code 31"
                         if rp_dtype == REGPROP_DATA_TYPE_REG_MULTI_SZ else "未分类")))
        rep.out("        wPropertyNameLength=%d  PropertyName(UTF-16LE, 去尾 NUL)=%r"
                % (rp_namelen, name_utf16))
        rep.out("        wPropertyDataLength=%d  PropertyData(UTF-16LE, 去尾 NUL)=%r"
                % (rp_datalen, data_utf16))

        rep.check("RegProp wDescriptorType==0x0004", rp_type == MSOS20_REG_PROPERTY, "0x%04X" % rp_type)

        # --- 契约 §2 要求的**单数 + REG_SZ**形态 ---
        rep.check("RegProp wPropertyDataType==0x0001 (REG_SZ) —— 契约 §2 最终定稿",
                  rp_dtype == REGPROP_DATA_TYPE_REG_SZ, "实际 0x%04X" % rp_dtype)
        rep.check("RegProp wPropertyDataType != 0x0007（复数 REG_MULTI_SZ 会让 winusb.sys Code 31）",
                  rp_dtype != REGPROP_DATA_TYPE_REG_MULTI_SZ, "实际 0x%04X" % rp_dtype)
        rep.check("RegProp wPropertyNameLength==%d" % REGPROP_NAME_LEN,
                  rp_namelen == REGPROP_NAME_LEN, "实际 %d" % rp_namelen)
        rep.check('RegProp PropertyName(UTF-16LE)=="%s"（单数）' % REGPROP_NAME_ASCII.decode(),
                  name_utf16 == REGPROP_NAME_ASCII.decode(), "实际 %r" % name_utf16)
        rep.check('RegProp PropertyName 不是复数 "%s"（已知会让 winusb.sys 起不来）'
                  % REGPROP_NAME_FORBIDDEN_PLURAL.decode(),
                  name_utf16 != REGPROP_NAME_FORBIDDEN_PLURAL.decode(), "实际 %r" % name_utf16)
        rep.check("RegProp PropertyName 以 NUL 结尾（UTF-16LE 双字节 0）",
                  rp_name.endswith(b"\x00\x00"), hexs(rp_name[-2:]) if len(rp_name) >= 2 else "?")
        rep.check("RegProp wPropertyDataLength==%d（单 GUID + 单 NUL）" % REGPROP_DATA_LEN,
                  rp_datalen == REGPROP_DATA_LEN, "实际 %d" % rp_datalen)

        # --- 保留的硬断言：wLength 与布局自洽（契约 §2 现在写的正是这个式子）---
        expect_len = REGPROP_LEN_FIXED + rp_namelen + max(rp_datalen, 0)
        rep.check("RegProp wLength == 10 + wPropertyNameLength + wPropertyDataLength",
                  le16(rp, 0) == expect_len,
                  "实际 wLength=%d vs 10+%d+%d=%d" % (le16(rp, 0), rp_namelen, rp_datalen, expect_len))
        rep.check("RegProp wLength == 子描述符实际字节数（无残尾）",
                  le16(rp, 0) == len(rp), "%d vs %d" % (le16(rp, 0), len(rp)))
        rep.check("RegProp PropertyData 长度==wPropertyDataLength",
                  len(rp_data) == rp_datalen, "%d vs %d" % (len(rp_data), rp_datalen))

        # --- 保留的硬断言：必须含 {A5DCBF10-…}（Chrome/Edge 唯一认的 device interface GUID）---
        rep.check("RegProp PropertyData 含 GUID {A5DCBF10-6530-11D2-901F-00C04FB951ED}",
                  GUID_USB_DEVICE.decode("ascii") in data_utf16,
                  "PropertyData=%r" % data_utf16)
        if GUID_WINUSB.decode("ascii") in data_utf16:
            rep.out("        ℹ PropertyData 还含 {DEE824EF-…}（本形态下非必需）")

        # --- 终止符：REG_SZ 要求"单 NUL 收尾"，而双 NUL 是 REG_MULTI_SZ 的特征 ---
        rep.check("RegProp PropertyData 是 REG_SZ：单 UTF-16 NUL 收尾",
                  rp_data.endswith(b"\x00\x00"), hexs(rp_data[-4:]) if len(rp_data) >= 4 else "?")
        rep.check("RegProp PropertyData 不是 REG_MULTI_SZ（不得双 NUL 收尾）",
                  not rp_data.endswith(b"\x00\x00\x00\x00"),
                  hexs(rp_data[-4:]) if len(rp_data) >= 4 else "?")

    # ---- [4c] 长度自洽（契约 §2 第 5 条）----------------------------------
    bos_msos_cap = None
    if msos_cap:
        bos_msos_cap = msos_cap[2].get("wMSOSDescriptorSetTotalLength")
    sum_sub = sum(w for _, w, _ in items)
    rep.out("    [长度自洽] BOS 字段=%s  SET_HEADER.wTotalLength=%d  子描述符累加=%d  数组实际=%d"
            % (bos_msos_cap, set_total, sum_sub, len(msos_raw)))
    rep.check("长度自洽: BOS.wMSOSDescriptorSetTotalLength == s_msos20_desc_set 真实字节数",
              bos_msos_cap == len(msos_raw),
              "BOS 字段 %s vs 数组 %d" % (bos_msos_cap, len(msos_raw)))
    rep.check("长度自洽: SET_HEADER.wTotalLength == s_msos20_desc_set 真实字节数",
              set_total == len(msos_raw), "字段 %d vs 数组 %d" % (set_total, len(msos_raw)))
    rep.check("长度自洽: 各子描述符 wLength 累加 == s_msos20_desc_set 真实字节数",
              sum_sub == len(msos_raw), "累加 %d vs 数组 %d" % (sum_sub, len(msos_raw)))
    rep.out("")

    # ---- [5] 字符串描述符表 ----------------------------------------------
    rep.out("[5] 字符串描述符表 (s_string_desc)")
    strtab = syms[SYM_STRTAB]
    n_str = strtab.size // 4
    expected_strings = dict(EXPECT_STRINGS_BASE)
    if mode == "b":
        expected_strings.update(EXPECT_STRINGS_MODE_B_EXTRA)
    got_strings: Dict[int, bytes] = {}
    for idx in range(n_str):
        ptr = elf.read_u32(strtab.addr + 4 * idx)
        raw = elf.read_cstr(ptr)
        got_strings[idx] = raw
        rep.out("    idx %d -> ptr=0x%08X  %r" % (idx, ptr, raw))
    rep.check("字符串表条目数<=%d (esp_tinyusb 上限)" % STR_TABLE_MAX, n_str <= STR_TABLE_MAX,
              "%d 条" % n_str)
    rep.check("字符串表条目数==%d (契约 §2)" % EXPECT_STR_COUNT[mode],
              n_str == EXPECT_STR_COUNT[mode], "%d vs %d" % (n_str, EXPECT_STR_COUNT[mode]))
    for idx, want in sorted(expected_strings.items()):
        rep.check("str[%d]==%r" % (idx, want), got_strings.get(idx) == want,
                  "实际 %r" % got_strings.get(idx))
    # idx 3 = 序列号：契约 §2 已改为"每次改动描述符内容就换"，**不再要求模式间不同**，
    # 也不钉死具体值。只硬断言"非空且是可见 ASCII"。
    serial = got_strings.get(3, b"")
    rep.check("str[3] 序列号非空",
              len(serial) > 0, "实际 %r" % serial)
    rep.check("str[3] 序列号是可打印 ASCII",
              len(serial) > 0 and all(0x20 <= c < 0x7F for c in serial), "实际 %r" % serial)
    if serial and not serial.startswith(SERIAL_PREFIX):
        rep.warn("str[3] 序列号不以 %r 开头" % SERIAL_PREFIX.decode(),
                 "实测 %r。契约 §2 只写\"每次改动就换（当前实测 PROBE-D3）\"，"
                 "未强制前缀，故仅告警。" % serial)
    rep.check("没有使用 0xEE (MS OS 1.0 路线已废弃)", 0xEE not in got_strings,
              "索引 0..%d" % (n_str - 1))
    rep.out("")

    # ---- [6] 回调强符号 / 归属 -------------------------------------------
    rep.out("[6] USB 回调符号（强符号 + 归属）")
    have_map = os.path.isfile(_map_path(elf_path := elf.elf))
    if not have_map:
        rep.out("    ⚠ 未找到 %s ，回调归属无法从 map 判定（相关断言记为未验证）" % _map_path(elf.elf))
    for name in (SYM_BOS_CB, SYM_VENDOR_CTRL, SYM_VENDOR_RX):
        sym = elf.symbols.get(name)
        if sym is None:
            rep.check("%s 已定义" % name, False, "ELF 里找不到")
            continue
        owner = elf.map_owner(name)
        rep.out("    %-28s addr=0x%08X size=%-4d type=%s owner=%s"
                % (name, sym.addr, sym.size, sym.kind, owner or "?"))
        rep.check("%s 是强符号 (nm type T/t)" % name, sym.kind in ("T", "t"),
                  "type=%s" % sym.kind)
        if have_map:
            rep.check("%s 由 usb_probe.c 提供（非栈内默认实现）" % name,
                      owner is not None and "usb_probe" in owner,
                      "owner=%s" % owner)

    bos_sym = elf.symbols.get(SYM_BOS_CB)
    if bos_sym is not None:
        dis = elf.disassemble(bos_sym)
        rep.out("    tud_descriptor_bos_cb 反汇编：")
        for line in dis.splitlines():
            if SYM_BOS in line or "lui" in line or "addi" in line or "ret" in line:
                rep.out("      %s" % line.rstrip())
        # 弱默认实现 (usbd.c:56) 返回 NULL；这里必须能看到 s_desc_bos 的地址
        rep.check("tud_descriptor_bos_cb 返回 s_desc_bos（不是返回 NULL 的弱默认实现）",
                  SYM_BOS in dis, "反汇编里未出现 %s" % SYM_BOS)
        for name in (SYM_MOUNT, SYM_UMOUNT):
            owner = elf.map_owner(name)
            rep.out("    %-28s owner=%s" % (name, owner or "?"))
            if have_map:
                rep.check("%s 由 esp_tinyusb 提供（应用侧未重复定义）" % name,
                          owner is not None and "esp_tinyusb" in owner,
                          "owner=%s" % owner)
    if have_map:
        rep.check("usb_probe.c 未定义 tud_mount_cb/tud_umount_cb（避免 multiple definition）",
                  "usb_probe" not in (elf.map_owner(SYM_MOUNT) or "")
                  and "usb_probe" not in (elf.map_owner(SYM_UMOUNT) or ""),
                  "mount owner=%s / umount owner=%s"
                  % (elf.map_owner(SYM_MOUNT), elf.map_owner(SYM_UMOUNT)))
    rep.out("")

    # ---- [7] 端点预算推演（含 NCM） --------------------------------------
    rep.out("[7] 端点预算推演（契约 §1 推演段：加 NCM）")
    hs_cfg = parse_config(hs_raw)
    hs_eps = [ep for itf in hs_cfg.interfaces for ep in itf.endpoints]
    base_in = sum(1 for e in hs_eps if e.addr & 0x80)
    base_out = len(hs_eps) - base_in
    ncm_in = base_in + NCM_PROJECTION_IN
    ncm_out = base_out + NCM_PROJECTION_OUT
    ncm_total = len(hs_eps) + NCM_PROJECTION_EP_TOTAL
    rep.out("    模式 %s (HS cfg): IN=%d OUT=%d total=%d" % (mode, base_in, base_out, len(hs_eps)))
    rep.out("    加 NCM(EP4 int IN + EP5 bulk IN/OUT) 后: IN=%d OUT=%d total=%d"
            % (ncm_in, ncm_out, ncm_total))
    rep.out("    P4 HS 口上限: ep_in_count=%d(含 EP0), ep_count=%d(含 EP0)  [dwc2_esp32.h:80]"
            % (DWC2_EP_IN_COUNT, DWC2_EP_COUNT))
    rep.out("    算式: IN %d + 2(NCM) = %d ; 含 EP0 = %d %s %d -> 余量 %d"
            % (base_in, ncm_in, ncm_in + 1, "<=" if ncm_in + 1 <= DWC2_EP_IN_COUNT else ">",
               DWC2_EP_IN_COUNT, DWC2_EP_IN_COUNT - (ncm_in + 1)))
    rep.check("加 NCM 后 IN 端点(含 EP0)<=%d" % DWC2_EP_IN_COUNT,
              ncm_in + 1 <= DWC2_EP_IN_COUNT, "IN=%d +EP0=%d" % (ncm_in, ncm_in + 1))
    rep.check("加 NCM 后总端点(含 EP0)<=%d" % DWC2_EP_COUNT,
              ncm_total + 1 <= DWC2_EP_COUNT, "total=%d +EP0=%d" % (ncm_total, ncm_total + 1))
    if mode == "b":
        # 契约 §1 的 "5 IN + EP0 = 6/8" 结论是从**模式 B**（MTP+Vendor）推出来的
        rep.check("契约 §1 推演：模式 B 加 NCM 后 5 IN + EP0 == 6/8",
                  ncm_in + 1 == 6, "实际 %d IN + EP0 = %d" % (ncm_in, ncm_in + 1))
    else:
        rep.out("    注: 契约 §1 的 '6 IN/8' 结论基于模式 B；模式 A 加 NCM 得 %d IN + EP0 = %d/8"
                % (ncm_in, ncm_in + 1))
    rep.out("")


def lint_source(rep: Report, src_path: str, mode: str) -> None:
    """§[8] 源码级断言：`wIndex` 接受 0 与 0x07、bRequest 与 BOS 的 bVendorCode 严格对应。

    这些是**控制路径**属性，不在描述符字节里，只能读源码。故意做成有针对性的正则，
    并在失败时打印命中的原始行；读不到源码时记为"跳过"（不算通过也不算失败）。
    """
    rep.out("[8] 源码级：wIndex / bRequest 分发（%s）" % src_path)
    if not os.path.isfile(src_path):
        rep.skip("wIndex 同时接受 0 与 0x07", "源码不存在: %s" % src_path)
        rep.skip("bRequest == MS OS 的 bVendorCode", "源码不存在")
        rep.out("")
        return
    try:
        with open(src_path, "r", encoding="utf-8", errors="replace") as fh:
            src = fh.read()
    except OSError as exc:
        rep.skip("源码级 wIndex/bRequest 检查", "读取失败: %s" % exc)
        rep.out("")
        return
    lines = src.splitlines()

    def find(pattern: str) -> List[Tuple[int, str]]:
        rx = re.compile(pattern)
        return [(i + 1, lines[i].strip()) for i in range(len(lines)) if rx.search(lines[i])]

    # -- wIndex 接受 0 与 0x07（且用的是一个 0x07 常量）--
    windex_hits = find(r"wIndex\s*==\s*0x0000[\s\S]{0,120}?wIndex\s*==|wIndex\s*==\s*0\b[\s\S]{0,120}?wIndex\s*==")
    idx_const = find(r"#define\s+PROBE_MSOS20_DESCRIPTOR_INDEX\s+0x0*7\b")
    ok_windex = bool(windex_hits) and bool(idx_const)
    for ln, text in windex_hits:
        rep.out("    L%-5d %s" % (ln, text))
    for ln, text in idx_const:
        rep.out("    L%-5d %s" % (ln, text))
    rep.check("源码: MS OS 请求同时接受 wIndex=0 与 wIndex=0x07", ok_windex,
              "比较行 %s / 0x07 常量 %s"
              % (len(windex_hits), "找到" if idx_const else "未找到"))

    # -- bVendorCode 定义与 bRequest 分发严格对应 --
    vc_we = find(r"#define\s+PROBE_BOS_VENDOR_CODE_WEBUSB\s+0x0*1\b")
    vc_ms = find(r"#define\s+PROBE_BOS_VENDOR_CODE_MSOS20\s+0x0*2\b")
    req_ms = find(r"bRequest\s*==\s*PROBE_BOS_VENDOR_CODE_MSOS20")
    req_we = find(r"bRequest\s*==\s*PROBE_BOS_VENDOR_CODE_WEBUSB")
    rep.check("源码: MS OS bVendorCode==0x02 且 GET_MS_DESCRIPTOR 按它分发",
              bool(vc_ms) and bool(req_ms),
              "宏 %s / 分发 %s" % ("找到" if vc_ms else "缺失", "找到" if req_ms else "缺失"))
    rep.check("源码: WebUSB bVendorCode==0x01 且按它分发 GET_URL",
              bool(vc_we) and bool(req_we),
              "宏 %s / 分发 %s" % ("找到" if vc_we else "缺失", "找到" if req_we else "缺失"))

    # -- iLandingPage==0 时不该有 GET_URL 实现（只 stall）；列出证据行 --
    landing = find(r"#define\s+PROBE_WEBUSB_LANDING_PAGE\s+0\b")
    rep.check("源码: PROBE_WEBUSB_LANDING_PAGE==0（不声明着陆页）", bool(landing),
              "未找到 `#define PROBE_WEBUSB_LANDING_PAGE 0`")
    rep.out("")


def lint_peer(rep: Report, elf: ElfImage, mode: str, peer_path: str) -> None:
    """§[9] 跨模式比对：序列号必须不同、PID 必须不同（契约 §2）。"""
    rep.out("[9] 跨模式比对（peer: %s）" % peer_path)
    if not os.path.isfile(peer_path):
        rep.skip("序列号在模式 A/B 间必须不同（契约 §2）", "peer ELF 不存在: %s" % peer_path)
        rep.out("")
        return
    if os.path.abspath(peer_path) == os.path.abspath(elf.elf):
        rep.skip("序列号在模式 A/B 间必须不同（契约 §2）", "peer 与自身是同一个 ELF")
        rep.out("")
        return
    try:
        peer = ElfImage(peer_path, os.path.dirname(elf.nm))
    except ToolError as exc:
        rep.skip("序列号在模式 A/B 间必须不同（契约 §2）", "peer 解析失败: %s" % exc)
        rep.out("")
        return

    def serial_of(img: ElfImage) -> Optional[bytes]:
        dev = img.symbols.get(SYM_DEVICE)
        st = img.symbols.get(SYM_STRTAB)
        if dev is None or st is None:
            return None
        raw = img.read(dev.addr, dev.size)
        idx = raw[16]
        if idx >= st.size // 4:
            return None
        return img.read_cstr(img.read_u32(st.addr + 4 * idx))

    def pid_of(img: ElfImage) -> Optional[int]:
        dev = img.symbols.get(SYM_DEVICE)
        return None if dev is None else le16(img.read(dev.addr, dev.size), 10)

    mine, theirs = serial_of(elf), serial_of(peer)
    my_pid, peer_pid = pid_of(elf), pid_of(peer)
    rep.out("    mode %s : PID=0x%04X serial=%r" % (mode, my_pid or 0, mine))
    rep.out("    peer    : PID=0x%04X serial=%r" % (peer_pid or 0, theirs))
    # 契约 §2 **已删除**"序列号模式间必须不同"：现在只要求"每次改动描述符内容就换"，
    # 且"模式 A/B 的 PID 不同，本身已是不同实例"。所以这里只硬断言非空，
    # 两模式相同 → WARN（可能是漏改，也可能是刻意保留），不再是 FAIL。
    rep.check("两模式序列号都非空",
              bool(mine) and bool(theirs), "本模式 %r vs peer %r" % (mine, theirs))
    if mine and theirs and mine == theirs:
        rep.warn("两模式序列号相同",
                 "%r。契约 §2 已删除\"模式间必须不同\"（PID 不同已构成不同实例）；"
                 "但若你期望靠序列号区分实例，请检查是否漏改。" % mine)
    rep.check("idProduct 在模式 A/B 间必须不同（契约 §2）",
              my_pid is not None and peer_pid is not None and my_pid != peer_pid,
              "0x%04X vs 0x%04X" % (my_pid or 0, peer_pid or 0))
    rep.out("")


def _itf_offset(cfg: Config, itf: Interface) -> int:
    """接口对象在配置数组里的偏移。"""
    return itf.offset


def _map_path(elf: str) -> str:
    return os.path.splitext(elf)[0] + ".map"


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="desc_lint.py",
        description="MeidaP4 探测固件 USB 描述符静态校验器（从 ELF 抠描述符，对照 docs/probe-contract.md）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="退出码: 0=全部通过  1=断言失败  2=工具/输入错误",
    )
    p.add_argument("--mode", choices=("a", "b"), help="探测模式（与 ELF 必须一致）")
    p.add_argument("--slot", default="verify", help="构建槽位名，用于推导 build-<slot>-<mode>/MeidaP4.elf")
    p.add_argument("--build-dir", help="直接指定构建目录（内含 MeidaP4.elf）")
    p.add_argument("--elf", help="直接指定 ELF 路径（优先级最高）")
    p.add_argument("--toolchain", help="riscv32-esp-elf 工具目录（含 nm/objcopy）")
    p.add_argument("--peer", help="另一个模式的 ELF，用于跨模式比对序列号/PID（默认按 --slot 自动找）")
    p.add_argument("--source", help="usb_probe.c 路径，用于源码级 wIndex/bRequest 断言"
                                    "（默认 <repo>/components/usb_probe/usb_probe.c）")
    p.add_argument("--json", action="store_true", help="额外输出一行 JSON 摘要")
    p.add_argument("--strict-baseline", action="store_true",
                   help="把\"与冻结基线漂移\"的 WARN 提升为 FAIL（例如 bcdDevice 与基线不同、"
                        "两模式序列号相同）。默认不提升，因为契约允许这些值随描述符改动而变。")
    return p


def resolve_peer(args, mode: str) -> Optional[str]:
    """peer ELF：显式 --peer 优先；否则按 --slot 找另一个模式的 build 目录。"""
    if args.peer:
        return os.path.abspath(args.peer)
    if args.elf or args.build_dir:
        return None
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    other = "b" if mode == "a" else "a"
    cand = os.path.join(repo, "build-%s-%s" % (args.slot, other), "MeidaP4.elf")
    return cand


def resolve_source(args) -> str:
    if args.source:
        return os.path.abspath(args.source)
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(repo, "components", "usb_probe", "usb_probe.c")


def resolve_elf(args) -> str:
    if args.elf:
        return os.path.abspath(args.elf)
    if not args.mode:
        raise ToolError("需要 --mode a|b ，或直接给 --elf / --build-dir。")
    if args.build_dir:
        base = args.build_dir
    else:
        repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        base = os.path.join(repo, "build-%s-%s" % (args.slot, args.mode))
    return os.path.abspath(os.path.join(base, "MeidaP4.elf"))


def infer_mode_from_config(elf: ElfImage) -> Optional[str]:
    """从 device 描述符的 idProduct 反推模式（--elf 未给 --mode 时用）。"""
    for sym_name in (SYM_DEVICE,):
        sym = elf.symbols.get(sym_name)
        if sym is None or sym.size < 12:
            return None
        pid = le16(elf.read(sym.addr, sym.size), 10)
        for mode, exp in EXPECT_PID.items():
            if pid == exp:
                return mode
    return None


def main(argv: Optional[Sequence[str]] = None) -> int:
    _force_utf8_stdio()
    args = build_arg_parser().parse_args(argv)

    try:
        toolchain = find_toolchain(args.toolchain)
        elf_path = resolve_elf(args)
        elf = ElfImage(elf_path, toolchain)
    except ToolError as exc:
        sys.stderr.write("[desc_lint] 输入/工具错误: %s\n" % exc)
        return EXIT_ERROR

    mode = args.mode
    if not mode:
        try:
            mode = infer_mode_from_config(elf)
        except ToolError as exc:
            sys.stderr.write("[desc_lint] 输入/工具错误: %s\n" % exc)
            return EXIT_ERROR
        if not mode:
            sys.stderr.write("[desc_lint] 无法从 idProduct 反推模式，请显式指定 --mode a|b。\n")
            return EXIT_ERROR

    rep = Report("mode %s · %s" % (mode, elf_path), strict_baseline=args.strict_baseline)
    rep.out("=" * 78)
    rep.out("MeidaP4 描述符静态校验（独立复核）  %s" % rep.title)
    rep.out("契约: docs/probe-contract.md §1/§2（**最终定稿**：device 类分叉 / iLandingPage=0 /")
    rep.out("      MS OS 2.0 单数 Registry Property REG_SZ / subset header 降为推荐 / 长度自洽）")
    rep.out("ELF : %s" % elf_path)
    rep.out("工具链: %s" % toolchain)
    rep.out("模式: strict_baseline=%s（--strict-baseline 会把基线漂移 WARN 算作 FAIL）"
            % ("on" if args.strict_baseline else "off"))
    rep.out("!" * 78)
    rep.out("免责: 离线 lint PASS **不能**证明 Chrome 能看到设备 —— 那取决于 Windows 侧")
    rep.out("      device interface 的注册（由 MS OS 2.0 Registry Property 决定），")
    rep.out("      不是描述符字节能证明的。本工具只判定\"冻结版代码与最终契约是否一致\"，")
    rep.out("      不是端到端功能判定。真机结论见 docs/probe-report.md。")
    rep.out("!" * 78)
    rep.out("")

    try:
        lint(elf, mode, rep)
        peer = resolve_peer(args, mode)
        if peer:
            lint_peer(rep, elf, mode, peer)
        else:
            rep.out("[9] 跨模式比对")
            rep.skip("序列号在模式 A/B 间必须不同（契约 §2）",
                     "未指定 --peer，且无法按 --slot 推导 peer ELF")
            rep.out("")
        lint_source(rep, resolve_source(args), mode)
    except ToolError as exc:
        sys.stdout.write(rep.render() + "\n")
        sys.stderr.write("[desc_lint] 解析/输入错误: %s\n" % exc)
        return EXIT_ERROR

    passed = len(rep.checks) - len([c for c in rep.checks if not c[1]])
    rep.out("=" * 78)
    rep.out("[断言汇总] %d 条，通过 %d，失败 %d，跳过 %d，WARN %d"
            % (len(rep.checks), passed,
               len([c for c in rep.checks if not c[1]]), len(rep.skipped), len(rep.warnings)))
    hard_fails = [c for c in rep.checks if not c[1]]
    if hard_fails:
        for name, _, detail in hard_fails:
            rep.out("  FAIL  %s%s" % (name, ("  -- " + detail) if detail else ""))
    elif rep.warnings and args.strict_baseline:
        rep.out("  （无硬断言失败；但 --strict-baseline 下基线漂移 WARN 已提升为 FAIL）")
    for name, detail in rep.warnings:
        rep.out("  WARN  %s  --  %s" % (name, detail))
    for name, why in rep.skipped:
        rep.out("  SKIP  %s  --  %s" % (name, why))
    fails = rep.failures
    rep.out("[结论] %s" % ("PASS 全部断言通过" if not fails
                           else "FAIL 有 %d 条断言不满足" % len(fails)))
    if not fails:
        rep.out("[免责] PASS 仅代表**描述符字节**与最终契约一致；Chrome 能否看到设备取决于 Windows")
        rep.out("       侧 device interface 注册（MS OS 2.0 Registry Property），须以真机为准；")
        rep.out("       本判定不是端到端功能判定。")
    rep.out("=" * 78)
    sys.stdout.write(rep.render() + "\n")
    if args.json:
        print(json.dumps(rep.json_summary(), ensure_ascii=False))
    return EXIT_FAIL if rep.failures else EXIT_PASS

if __name__ == "__main__":
    try:
        sys.exit(main())
    except ToolError as exc:
        sys.stderr.write("[desc_lint] 输入/工具错误: %s\n" % exc)
        sys.exit(EXIT_ERROR)
