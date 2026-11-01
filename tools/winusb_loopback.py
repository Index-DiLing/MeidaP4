#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""MeidaP4 probe firmware - host-side WinUSB claim + bulk loopback validator.

Pure Python 3 standard library. Talks to SetupAPI and WinUSB directly through
ctypes; no pyusb / libusb / Zadig involvement (those would require replacing the
driver binding, which would destroy the WCID-provided WinUSB binding).

What it does
    1. Enumerates WinUSB device interfaces (SetupDiGetClassDevs +
       SetupDiEnumDeviceInterfaces + SetupDiGetDeviceInterfaceDetailW), prints
       device path / hardware IDs / friendly name / BusReportedDeviceDesc, and
       filters by VID (and optionally PID / interface number "MI").
    2. CreateFileW + WinUsb_Initialize, WinUsb_QueryInterfaceSettings,
       WinUsb_QueryPipe  -> interface + pipe inventory.
    3. Claims the interface and runs a bulk loopback: chunked (<= 4096 B) OUT
       writes with a *concurrently running* IN reader thread, byte-exact
       verification, throughput, RTT min/avg/max, first mismatch offset.
    4. Strips the firmware heartbeat line "\\nHB <uptime_ms>\\n" out of the same
       IN stream used by the loopback data and reports device uptime.

Exit codes
    0  all requested checks passed
    1  loopback mismatch / timeout / incomplete transfer
    2  tool, enumeration, open or WinUSB error

Run with the ESP-IDF python, e.g.:
    C:\\Espressif\\tools\\python\\v6.1\\venv\\Scripts\\python.exe tools\\winusb_loopback.py --mi 01
"""

from __future__ import annotations

import argparse
import ctypes
import os
import re
import sys
import threading
import time
from collections import deque
from ctypes import wintypes as wt

EXIT_PASS = 0
EXIT_FAIL = 1
EXIT_TOOL = 2

# ---------------------------------------------------------------------------
# Device interface GUIDs  (all established by measurement on this host, see
# docs/host-usb-test.md for the evidence table)
# ---------------------------------------------------------------------------
# {DEE824EF-729B-4A0E-9C14-B7117D33A817} == GUID_DEVINTERFACE_WINUSB.
# winusb.sys hard-codes it: the 16-byte GUID pattern occurs exactly once in
# C:\Windows\System32\drivers\winusb.sys (offset 0x146F0) and in no other
# driver.  winusb.sys registers it for a device only when that device key
# carries a DeviceInterfaceGUID / DeviceInterfaceGUIDs value.
GUID_DEVINTERFACE_WINUSB = "{DEE824EF-729B-4A0E-9C14-B7117D33A817}"

# {A5DCBF10-6530-11D2-901F-00C04FB951ED} == GUID_DEVINTERFACE_USB_DEVICE.
# Registered by the USB *hub* drivers (USBHUB3.SYS / usbhub.sys / tsusbhub.sys)
# for the USB *device* node - never for a usbccgp child interface.  This is the
# GUID Chrome/Edge WebUSB actually enumerates (the pattern is present in
# chrome.dll and msedge.dll; DEE824EF and F72FE0D4 are not).
GUID_DEVINTERFACE_USB_DEVICE = "{A5DCBF10-6530-11D2-901F-00C04FB951ED}"

# {F72FE0D4-CBCB-407D-8814-9ED673D0DD6B} appears in exactly one place on this
# host - winusb.inf line 96, inside [ADB.HW.AddReg] - so it belongs to the ADB
# device class, NOT to generic WinUSB.  Kept only so the diagnosis can show it
# resolves to zero interfaces (it is a commonly mis-quoted "WinUSB GUID").
GUID_WINUSB_INF_ADB = "{F72FE0D4-CBCB-407D-8814-9ED673D0DD6B}"

# ---------------------------------------------------------------------------
# Win32 constants
# ---------------------------------------------------------------------------
DIGCF_PRESENT = 0x02
DIGCF_DEVICEINTERFACE = 0x10

ERROR_NO_MORE_ITEMS = 259
ERROR_INSUFFICIENT_BUFFER = 122
ERROR_ACCESS_DENIED = 5
ERROR_IO_PENDING = 997
ERROR_OPERATION_ABORTED = 995
ERROR_SEM_TIMEOUT = 121
ERROR_NOT_FOUND = 1168
ERROR_FILE_NOT_FOUND = 2
ERROR_INVALID_HANDLE = 6
ERROR_GEN_FAILURE = 31

WAIT_OBJECT_0 = 0
WAIT_TIMEOUT = 258

GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
FILE_SHARE_READ = 0x01
FILE_SHARE_WRITE = 0x02
OPEN_EXISTING = 3
FILE_FLAG_OVERLAPPED = 0x40000000

INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

# SetupDiGetDeviceRegistryProperty property ids
SPDRP_DEVICEDESC = 0x00
SPDRP_HARDWAREID = 0x01
SPDRP_SERVICE = 0x04
SPDRP_CLASS = 0x07
SPDRP_CLASSGUID = 0x08
SPDRP_COMPATIBLEIDS = 0x0D
SPDRP_FRIENDLYNAME = 0x0C

DIGCF_ALLCLASSES = 0x04

DICS_FLAG_GLOBAL = 0x00000001
DIREG_DEV = 0x00000001
DIREG_DRV = 0x00000002
KEY_READ = 0x20019

REG_SZ = 1
REG_MULTI_SZ = 7

DEVPROP_TYPE_STRING = 0x00000012
DEVPROP_TYPE_STRING_LIST = 0x00002012

# WinUSB
DEVICE_SPEED = 0x01
PIPE_TRANSFER_TIMEOUT = 0x03
ALLOW_PARTIAL_READS = 0x05

SPEED_NAMES = {0: "Unknown", 1: "LowSpeed", 2: "FullSpeed", 3: "HighSpeed", 4: "SuperSpeed"}
PIPE_TYPE_NAMES = {0: "Control", 1: "Isochronous", 2: "Bulk", 3: "Interrupt"}

MAX_CHUNK = 4096  # device-side buffers are ~12 KB; never exceed 4 KiB per block


# ---------------------------------------------------------------------------
# ctypes plumbing
# ---------------------------------------------------------------------------
if not sys.platform.startswith("win"):
    print("this tool only runs on Windows (SetupAPI + WinUSB)", file=sys.stderr)
    raise SystemExit(EXIT_TOOL)

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
setupapi = ctypes.WinDLL("setupapi", use_last_error=True)
winusb = ctypes.WinDLL("winusb", use_last_error=True)


class GUID(ctypes.Structure):
    _fields_ = [
        ("Data1", wt.DWORD),
        ("Data2", wt.WORD),
        ("Data3", wt.WORD),
        ("Data4", ctypes.c_ubyte * 8),
    ]

    def __init__(self, text=None):
        super().__init__()
        if text:
            p = text.strip().strip("{}").split("-")
            if len(p) != 5:
                raise ValueError("bad GUID: %r" % (text,))
            self.Data1 = int(p[0], 16)
            self.Data2 = int(p[1], 16)
            self.Data3 = int(p[2], 16)
            rest = p[3] + p[4]
            for i in range(8):
                self.Data4[i] = int(rest[2 * i:2 * i + 2], 16)

    def __str__(self):
        d = self.Data4
        return "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}" % (
            self.Data1, self.Data2, self.Data3,
            d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7],
        )


class DEVPROPKEY(ctypes.Structure):
    _fields_ = [("fmtid", GUID), ("pid", wt.ULONG)]


class SP_DEVINFO_DATA(ctypes.Structure):
    _fields_ = [
        ("cbSize", wt.DWORD),
        ("ClassGuid", GUID),
        ("DevInst", wt.DWORD),
        ("Reserved", ctypes.c_void_p),
    ]


class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
    _fields_ = [
        ("cbSize", wt.DWORD),
        ("InterfaceClassGuid", GUID),
        ("Flags", wt.DWORD),
        ("Reserved", ctypes.c_void_p),
    ]


class SP_DEVICE_INTERFACE_DETAIL_DATA_W(ctypes.Structure):
    _fields_ = [("cbSize", wt.DWORD), ("DevicePath", ctypes.c_wchar * 1)]


class USB_INTERFACE_DESCRIPTOR(ctypes.Structure):
    _fields_ = [
        ("bLength", ctypes.c_ubyte),
        ("bDescriptorType", ctypes.c_ubyte),
        ("bInterfaceNumber", ctypes.c_ubyte),
        ("bAlternateSetting", ctypes.c_ubyte),
        ("bNumEndpoints", ctypes.c_ubyte),
        ("bInterfaceClass", ctypes.c_ubyte),
        ("bInterfaceSubClass", ctypes.c_ubyte),
        ("bInterfaceProtocol", ctypes.c_ubyte),
        ("iInterface", ctypes.c_ubyte),
    ]


class WINUSB_PIPE_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("PipeType", ctypes.c_int),
        ("PipeId", ctypes.c_ubyte),
        ("MaximumPacketSize", wt.USHORT),
        ("Interval", ctypes.c_ubyte),
    ]


class OVERLAPPED(ctypes.Structure):
    _fields_ = [
        ("Internal", ctypes.c_void_p),
        ("InternalHigh", ctypes.c_void_p),
        ("Offset", wt.DWORD),
        ("OffsetHigh", wt.DWORD),
        ("hEvent", wt.HANDLE),
    ]


setupapi.SetupDiGetClassDevsW.restype = wt.HANDLE
setupapi.SetupDiGetClassDevsW.argtypes = [
    ctypes.POINTER(GUID), wt.LPCWSTR, wt.HWND, wt.DWORD]
setupapi.SetupDiEnumDeviceInfo.restype = wt.BOOL
setupapi.SetupDiEnumDeviceInfo.argtypes = [
    wt.HANDLE, wt.DWORD, ctypes.POINTER(SP_DEVINFO_DATA)]
setupapi.SetupDiEnumDeviceInterfaces.restype = wt.BOOL
setupapi.SetupDiEnumDeviceInterfaces.argtypes = [
    wt.HANDLE, ctypes.POINTER(SP_DEVINFO_DATA), ctypes.POINTER(GUID),
    wt.DWORD, ctypes.POINTER(SP_DEVICE_INTERFACE_DATA)]
setupapi.SetupDiGetDeviceInterfaceDetailW.restype = wt.BOOL
setupapi.SetupDiGetDeviceInterfaceDetailW.argtypes = [
    wt.HANDLE, ctypes.POINTER(SP_DEVICE_INTERFACE_DATA),
    ctypes.POINTER(SP_DEVICE_INTERFACE_DETAIL_DATA_W), wt.DWORD,
    ctypes.POINTER(wt.DWORD), ctypes.POINTER(SP_DEVINFO_DATA)]
setupapi.SetupDiGetDeviceRegistryPropertyW.restype = wt.BOOL
setupapi.SetupDiGetDeviceRegistryPropertyW.argtypes = [
    wt.HANDLE, ctypes.POINTER(SP_DEVINFO_DATA), wt.DWORD,
    ctypes.POINTER(wt.DWORD), ctypes.POINTER(ctypes.c_ubyte), wt.DWORD,
    ctypes.POINTER(wt.DWORD)]
setupapi.SetupDiGetDevicePropertyW.restype = wt.BOOL
setupapi.SetupDiGetDevicePropertyW.argtypes = [
    wt.HANDLE, ctypes.POINTER(SP_DEVINFO_DATA), ctypes.POINTER(DEVPROPKEY),
    ctypes.POINTER(wt.ULONG), ctypes.POINTER(ctypes.c_ubyte), wt.DWORD,
    ctypes.POINTER(wt.DWORD), wt.DWORD]
setupapi.SetupDiDestroyDeviceInfoList.restype = wt.BOOL
setupapi.SetupDiDestroyDeviceInfoList.argtypes = [wt.HANDLE]
setupapi.SetupDiGetDeviceInstanceIdW.restype = wt.BOOL
setupapi.SetupDiGetDeviceInstanceIdW.argtypes = [
    wt.HANDLE, ctypes.POINTER(SP_DEVINFO_DATA), wt.LPWSTR, wt.DWORD,
    ctypes.POINTER(wt.DWORD)]
setupapi.SetupDiOpenDevRegKey.restype = wt.HKEY
setupapi.SetupDiOpenDevRegKey.argtypes = [
    wt.HANDLE, ctypes.POINTER(SP_DEVINFO_DATA), wt.DWORD, wt.DWORD, wt.DWORD,
    wt.DWORD]

advapi32 = ctypes.WinDLL("advapi32", use_last_error=True)
advapi32.RegEnumKeyExW.restype = wt.LONG
advapi32.RegEnumKeyExW.argtypes = [
    wt.HKEY, wt.DWORD, wt.LPWSTR, ctypes.POINTER(wt.DWORD), ctypes.c_void_p,
    wt.LPWSTR, ctypes.POINTER(wt.DWORD), ctypes.c_void_p]
advapi32.RegQueryValueExW.restype = wt.LONG
advapi32.RegQueryValueExW.argtypes = [
    wt.HKEY, wt.LPCWSTR, ctypes.c_void_p, ctypes.POINTER(wt.DWORD),
    ctypes.POINTER(ctypes.c_ubyte), ctypes.POINTER(wt.DWORD)]
advapi32.RegCloseKey.restype = wt.LONG
advapi32.RegCloseKey.argtypes = [wt.HKEY]
advapi32.RegEnumValueW.restype = wt.LONG
advapi32.RegEnumValueW.argtypes = [
    wt.HKEY, wt.DWORD, wt.LPWSTR, ctypes.POINTER(wt.DWORD), ctypes.c_void_p,
    ctypes.POINTER(wt.DWORD), ctypes.POINTER(ctypes.c_ubyte),
    ctypes.POINTER(wt.DWORD)]

k32.CreateFileW.restype = wt.HANDLE
k32.CreateFileW.argtypes = [
    wt.LPCWSTR, wt.DWORD, wt.DWORD, ctypes.c_void_p, wt.DWORD, wt.DWORD,
    wt.HANDLE]
k32.CloseHandle.argtypes = [wt.HANDLE]
k32.CreateEventW.restype = wt.HANDLE
k32.CreateEventW.argtypes = [ctypes.c_void_p, wt.BOOL, wt.BOOL, wt.LPCWSTR]
k32.ResetEvent.argtypes = [wt.HANDLE]
k32.WaitForSingleObject.restype = wt.DWORD
k32.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
k32.GetOverlappedResult.restype = wt.BOOL
k32.GetOverlappedResult.argtypes = [
    wt.HANDLE, ctypes.POINTER(OVERLAPPED), ctypes.POINTER(wt.DWORD), wt.BOOL]
k32.CancelIoEx.restype = wt.BOOL
k32.CancelIoEx.argtypes = [wt.HANDLE, ctypes.POINTER(OVERLAPPED)]

winusb.WinUsb_Initialize.restype = wt.BOOL
winusb.WinUsb_Initialize.argtypes = [wt.HANDLE, ctypes.POINTER(wt.HANDLE)]
winusb.WinUsb_Free.restype = wt.BOOL
winusb.WinUsb_Free.argtypes = [wt.HANDLE]
winusb.WinUsb_QueryInterfaceSettings.restype = wt.BOOL
winusb.WinUsb_QueryInterfaceSettings.argtypes = [
    wt.HANDLE, ctypes.c_ubyte, ctypes.POINTER(USB_INTERFACE_DESCRIPTOR)]
winusb.WinUsb_QueryPipe.restype = wt.BOOL
winusb.WinUsb_QueryPipe.argtypes = [
    wt.HANDLE, ctypes.c_ubyte, ctypes.c_ubyte,
    ctypes.POINTER(WINUSB_PIPE_INFORMATION)]
winusb.WinUsb_SetCurrentAlternateSetting.restype = wt.BOOL
winusb.WinUsb_SetCurrentAlternateSetting.argtypes = [wt.HANDLE, ctypes.c_ubyte]
winusb.WinUsb_GetCurrentAlternateSetting.restype = wt.BOOL
winusb.WinUsb_GetCurrentAlternateSetting.argtypes = [
    wt.HANDLE, ctypes.POINTER(ctypes.c_ubyte)]
winusb.WinUsb_QueryDeviceInformation.restype = wt.BOOL
winusb.WinUsb_QueryDeviceInformation.argtypes = [
    wt.HANDLE, wt.ULONG, ctypes.POINTER(wt.ULONG), ctypes.c_void_p]
winusb.WinUsb_SetPipePolicy.restype = wt.BOOL
winusb.WinUsb_SetPipePolicy.argtypes = [
    wt.HANDLE, ctypes.c_ubyte, wt.ULONG, wt.ULONG, ctypes.c_void_p]
winusb.WinUsb_ReadPipe.restype = wt.BOOL
winusb.WinUsb_ReadPipe.argtypes = [
    wt.HANDLE, ctypes.c_ubyte, ctypes.c_void_p, wt.ULONG,
    ctypes.POINTER(wt.ULONG), ctypes.POINTER(OVERLAPPED)]
winusb.WinUsb_WritePipe.restype = wt.BOOL
winusb.WinUsb_WritePipe.argtypes = [
    wt.HANDLE, ctypes.c_ubyte, ctypes.c_void_p, wt.ULONG,
    ctypes.POINTER(wt.ULONG), ctypes.POINTER(OVERLAPPED)]
winusb.WinUsb_AbortPipe.restype = wt.BOOL
winusb.WinUsb_AbortPipe.argtypes = [wt.HANDLE, ctypes.c_ubyte]
winusb.WinUsb_FlushPipe.restype = wt.BOOL
winusb.WinUsb_FlushPipe.argtypes = [wt.HANDLE, ctypes.c_ubyte]
winusb.WinUsb_ResetPipe.restype = wt.BOOL
winusb.WinUsb_ResetPipe.argtypes = [wt.HANDLE, ctypes.c_ubyte]


def _err():
    return ctypes.get_last_error()


# Names for the codes this tool actually reports.  Keeping them here lets the
# report stay pure ASCII: ctypes.FormatError() returns a localized string that
# turns into mojibake when the report is piped through a CP936/CP932 console,
# which would make the "copy-pasteable report" unreadable.
ERR_NAMES = {
    2: "ERROR_FILE_NOT_FOUND",
    5: "ERROR_ACCESS_DENIED",
    6: "ERROR_INVALID_HANDLE",
    8: "ERROR_NOT_ENOUGH_MEMORY",
    31: "ERROR_GEN_FAILURE",
    87: "ERROR_INVALID_PARAMETER",
    1168: "ERROR_NOT_FOUND",
    121: "ERROR_SEM_TIMEOUT",
    122: "ERROR_INSUFFICIENT_BUFFER",
    259: "ERROR_NO_MORE_ITEMS",
    995: "ERROR_OPERATION_ABORTED",
    997: "ERROR_IO_PENDING",
    1058: "ERROR_SERVICE_DISABLED",
    1072: "ERROR_SERVICE_MARKED_FOR_DELETE",
}


def _winerr(code):
    name = ERR_NAMES.get(code)
    txt = ""
    try:
        raw = ctypes.FormatError(code) or ""
        txt = raw.encode("ascii", "ignore").decode("ascii").strip()
    except Exception:
        txt = ""
    if name and txt:
        return "%d %s (%s)" % (code, name, txt)
    if name:
        return "%d %s" % (code, name)
    return "%d%s" % (code, (" " + txt) if txt else "")


class ToolError(Exception):
    """Fatal tool / enumeration / WinUSB setup error -> exit code 2."""


# ---------------------------------------------------------------------------
# Enumeration
# ---------------------------------------------------------------------------
def discover_winusb_guid():
    """Read the WinUSB device interface GUID out of winusb.inf when available."""
    root = os.environ.get("SystemRoot", r"C:\Windows")
    inf = os.path.join(root, "INF", "winusb.inf")
    try:
        with open(inf, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError:
        return None, None
    m = re.search(r"DeviceInterfaceGUIDs[^\"\r\n]*\"(\{[0-9A-Fa-f\-]+\})\"", text)
    if m:
        return m.group(1), inf
    return None, inf


def _get_reg_prop(hdev, did, prop_id):
    need = wt.DWORD(0)
    setupapi.SetupDiGetDeviceRegistryPropertyW(
        hdev, ctypes.byref(did), prop_id, None, None, 0, ctypes.byref(need))
    if need.value == 0:
        return None
    buf = (ctypes.c_ubyte * need.value)()
    ptype = wt.DWORD(0)
    ok = setupapi.SetupDiGetDeviceRegistryPropertyW(
        hdev, ctypes.byref(did), prop_id, ctypes.byref(ptype), buf, need.value,
        ctypes.byref(need))
    if not ok:
        return None
    raw = bytes(bytearray(buf[:need.value]))
    if ptype.value in (REG_SZ, REG_MULTI_SZ):
        txt = raw.decode("utf-16-le", "replace").rstrip("\x00")
        if ptype.value == REG_MULTI_SZ:
            return [x for x in txt.split("\x00") if x]
        return txt
    return raw


def _get_dev_prop(hdev, did, fmtid, pid):
    key = DEVPROPKEY(GUID(fmtid), pid)
    need = wt.DWORD(0)
    setupapi.SetupDiGetDevicePropertyW(
        hdev, ctypes.byref(did), ctypes.byref(key), None, None, 0,
        ctypes.byref(need), 0)
    if need.value == 0:
        return None
    buf = (ctypes.c_ubyte * need.value)()
    ptype = wt.ULONG(0)
    ok = setupapi.SetupDiGetDevicePropertyW(
        hdev, ctypes.byref(did), ctypes.byref(key), ctypes.byref(ptype), buf,
        need.value, ctypes.byref(need), 0)
    if not ok:
        return None
    raw = bytes(bytearray(buf[:need.value]))
    if ptype.value in (DEVPROP_TYPE_STRING, 0x00000012):
        return raw.decode("utf-16-le", "replace").rstrip("\x00")
    if ptype.value == DEVPROP_TYPE_STRING_LIST:
        return [x for x in raw.decode("utf-16-le", "replace").split("\x00") if x]
    return raw


def enumerate_candidates(guids, log):
    """Enumerate device interfaces for each GUID; returns a de-duplicated list."""
    found = {}
    for gtext in guids:
        try:
            guid = GUID(gtext)
        except ValueError as exc:
            log("  ! bad GUID %s: %s" % (gtext, exc))
            continue
        hdev = setupapi.SetupDiGetClassDevsW(
            ctypes.byref(guid), None, None, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)
        if not hdev or hdev == INVALID_HANDLE_VALUE:
            log("  ! SetupDiGetClassDevsW(%s) failed: %s" % (gtext, _winerr(_err())))
            continue
        log("  GUID %s -> enumerating" % gtext)
        index = 0
        try:
            while True:
                did = SP_DEVICE_INTERFACE_DATA()
                did.cbSize = ctypes.sizeof(SP_DEVICE_INTERFACE_DATA)
                if not setupapi.SetupDiEnumDeviceInterfaces(
                        hdev, None, ctypes.byref(guid), index, ctypes.byref(did)):
                    e = _err()
                    if e != ERROR_NO_MORE_ITEMS:
                        log("  ! SetupDiEnumDeviceInterfaces failed: %s" % _winerr(e))
                    break
                need = wt.DWORD(0)
                setupapi.SetupDiGetDeviceInterfaceDetailW(
                    hdev, ctypes.byref(did), None, 0, ctypes.byref(need), None)
                if need.value == 0:
                    log("  ! detail size query failed: %s" % _winerr(_err()))
                    index += 1
                    continue
                buf = (ctypes.c_ubyte * need.value)()
                detail = ctypes.cast(
                    buf, ctypes.POINTER(SP_DEVICE_INTERFACE_DETAIL_DATA_W))
                detail.contents.cbSize = ctypes.sizeof(
                    SP_DEVICE_INTERFACE_DETAIL_DATA_W)
                di = SP_DEVINFO_DATA()
                di.cbSize = ctypes.sizeof(SP_DEVINFO_DATA)
                ok = setupapi.SetupDiGetDeviceInterfaceDetailW(
                    hdev, ctypes.byref(did), detail, need.value,
                    ctypes.byref(need), ctypes.byref(di))
                if not ok:
                    log("  ! detail fetch failed: %s" % _winerr(_err()))
                    index += 1
                    continue
                path = ctypes.wstring_at(ctypes.addressof(detail.contents) + 4)
                inbuf = ctypes.create_unicode_buffer(512)
                inlen = wt.DWORD(512)
                inst = ""
                if setupapi.SetupDiGetDeviceInstanceIdW(
                        hdev, ctypes.byref(di), inbuf, 512, ctypes.byref(inlen)):
                    inst = inbuf.value
                hwids = _get_reg_prop(hdev, di, SPDRP_HARDWAREID) or []
                if isinstance(hwids, str):
                    hwids = [hwids]
                comp = _get_reg_prop(hdev, di, SPDRP_COMPATIBLEIDS) or []
                if isinstance(comp, str):
                    comp = [comp]
                # DEVPKEY_Device_BusReportedDeviceDesc
                # {540b947e-8b40-45bc-a8a2-6a0b894cbda2}, 4
                busdesc = None
                try:
                    busdesc = _get_dev_prop(
                        hdev, di, "540b947e-8b40-45bc-a8a2-6a0b894cbda2", 4)
                except Exception:
                    busdesc = None
                entry = {
                    "path": path,
                    "instance_id": inst,
                    "hwids": hwids,
                    "compatible": comp,
                    "friendly": _get_reg_prop(hdev, di, SPDRP_FRIENDLYNAME),
                    "desc": _get_reg_prop(hdev, di, SPDRP_DEVICEDESC),
                    "service": _get_reg_prop(hdev, di, SPDRP_SERVICE),
                    "class": _get_reg_prop(hdev, di, SPDRP_CLASS),
                    "busdesc": busdesc,
                    "guid": gtext,
                }
                key = path.upper()
                if key not in found:
                    found[key] = entry
                index += 1
        finally:
            setupapi.SetupDiDestroyDeviceInfoList(hdev)
    return list(found.values())


VID_RE = re.compile(r"VID_([0-9A-Fa-f]{4})")
PID_RE = re.compile(r"PID_([0-9A-Fa-f]{4})")
MI_RE = re.compile(r"MI_([0-9A-Fa-f]{2})")


def parse_ids(entry):
    vid = pid = mi = None
    hay = " ".join([entry["path"]] + list(entry["hwids"])).upper()
    m = VID_RE.search(hay)
    if m:
        vid = int(m.group(1), 16)
    m = PID_RE.search(hay)
    if m:
        pid = int(m.group(1), 16)
    m = MI_RE.search(hay)
    if m:
        mi = int(m.group(1), 16)
    return vid, pid, mi


def is_winusb_compatible(entry):
    return any("MS_COMP_WINUSB" in (c or "").upper() for c in entry["compatible"])


def select_candidate(entries, want_vid, want_pid, want_mi):
    """Pick the best candidate. Returns (entry, notes)."""
    notes = []
    scored = []
    for e in entries:
        vid, pid, mi = parse_ids(e)
        e["vid"], e["pid"], e["mi"] = vid, pid, mi
        if want_vid is not None and vid != want_vid:
            continue
        if want_pid is not None and pid != want_pid:
            continue
        if want_mi is not None and mi != want_mi:
            continue
        score = 0
        if is_winusb_compatible(e):
            score += 4
        if e["guid"].upper() == GUID_DEVINTERFACE_WINUSB.upper():
            score += 2
        if want_mi is not None and mi == want_mi:
            score += 2
        scored.append((score, e))
    if not scored:
        return None, notes
    scored.sort(key=lambda t: -t[0])
    if len(scored) > 1 and scored[0][0] == scored[1][0]:
        notes.append("multiple equally-ranked candidates; using the first one")
    return scored[0][1], notes


# ---------------------------------------------------------------------------
# Read-only diagnostics: why is this interface not openable?
# ---------------------------------------------------------------------------
def _reg_subkeys(hkey):
    names, i = [], 0
    while True:
        buf = ctypes.create_unicode_buffer(512)
        ln = wt.DWORD(512)
        if advapi32.RegEnumKeyExW(hkey, i, buf, ctypes.byref(ln), None, None,
                                  None, None) != 0:
            break
        names.append(buf.value)
        i += 1
    return names


def _reg_values(hkey):
    out, i = {}, 0
    while True:
        name = ctypes.create_unicode_buffer(512)
        nlen = wt.DWORD(512)
        typ = wt.DWORD(0)
        size = wt.DWORD(0)
        if advapi32.RegEnumValueW(hkey, i, name, ctypes.byref(nlen), None,
                                  ctypes.byref(typ), None,
                                  ctypes.byref(size)) != 0:
            break
        data = None
        if size.value:
            buf = (ctypes.c_ubyte * size.value)()
            sz2 = wt.DWORD(size.value)
            if advapi32.RegQueryValueExW(hkey, name.value, None,
                                        ctypes.byref(typ), buf,
                                        ctypes.byref(sz2)) == 0:
                raw = bytes(bytearray(buf[:sz2.value]))
                if typ.value in (REG_SZ, 2):
                    data = raw.decode("utf-16-le", "replace").rstrip("\x00")
                elif typ.value == REG_MULTI_SZ:
                    data = [x for x in
                            raw.decode("utf-16-le", "replace").split("\x00") if x]
                else:
                    data = raw
        out[name.value] = data
        i += 1
    return out


def enumerate_winusb_devnodes(want_vid):
    """All present devnodes (any setup class) whose hardware ID matches a VID."""
    hdev = setupapi.SetupDiGetClassDevsW(
        None, None, None, DIGCF_PRESENT | DIGCF_ALLCLASSES)
    if not hdev or hdev == INVALID_HANDLE_VALUE:
        raise ToolError("SetupDiGetClassDevsW(all classes) failed: %s"
                        % _winerr(_err()))
    nodes = []
    try:
        i = 0
        while True:
            di = SP_DEVINFO_DATA()
            di.cbSize = ctypes.sizeof(SP_DEVINFO_DATA)
            if not setupapi.SetupDiEnumDeviceInfo(hdev, i, ctypes.byref(di)):
                break
            i += 1
            buf = ctypes.create_unicode_buffer(512)
            ln = wt.DWORD(512)
            inst = ""
            if setupapi.SetupDiGetDeviceInstanceIdW(
                    hdev, ctypes.byref(di), buf, 512, ctypes.byref(ln)):
                inst = buf.value
            hwids = _get_reg_prop(hdev, di, SPDRP_HARDWAREID) or []
            if isinstance(hwids, str):
                hwids = [hwids]
            hay = (" ".join([inst] + list(hwids))).upper()
            m = VID_RE.search(hay)
            if want_vid is not None and (not m or int(m.group(1), 16) != want_vid):
                continue
            node = {
                "instance_id": inst,
                "hwids": hwids,
                "service": _get_reg_prop(hdev, di, SPDRP_SERVICE),
                "class": _get_reg_prop(hdev, di, SPDRP_CLASS),
                "classguid": _get_reg_prop(hdev, di, SPDRP_CLASSGUID),
                "desc": _get_reg_prop(hdev, di, SPDRP_DEVICEDESC),
                "friendly": _get_reg_prop(hdev, di, SPDRP_FRIENDLYNAME),
                "compatible": _get_reg_prop(hdev, di, SPDRP_COMPATIBLEIDS) or [],
                "devparam_subkeys": [],
                "devparam_values": {},
            }
            if isinstance(node["compatible"], str):
                node["compatible"] = [node["compatible"]]
            hkey = setupapi.SetupDiOpenDevRegKey(
                hdev, ctypes.byref(di), DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ)
            if hkey and hkey != INVALID_HANDLE_VALUE:
                try:
                    node["devparam_subkeys"] = _reg_subkeys(hkey)
                    node["devparam_values"] = _reg_values(hkey)
                finally:
                    advapi32.RegCloseKey(hkey)
            nodes.append(node)
    finally:
        setupapi.SetupDiDestroyDeviceInfoList(hdev)
    return nodes


def _interface_map(guid_text, log_silent=True):
    """guid -> {instance_id: [device paths]}"""
    out = {}
    try:
        entries = enumerate_candidates([guid_text], (lambda *_a: None))
    except Exception:
        return out
    for e in entries:
        out.setdefault(e.get("instance_id") or "", []).append(e["path"])
    return out


def _deviceclasses_hits(hashes):
    """Count DeviceClasses registry entries whose symbolic-link name contains
    each instance hash.  Answers: which interface GUIDs is this devnode
    actually registered under?  Uses winreg (standard library)."""
    import winreg
    out = {h: [] for h in hashes}
    base = r"SYSTEM\CurrentControlSet\Control\DeviceClasses"
    try:
        root = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, base, 0,
                              winreg.KEY_READ | winreg.KEY_WOW64_64KEY)
    except OSError as exc:
        return {"__error__": str(exc)}
    try:
        i = 0
        while True:
            try:
                g = winreg.EnumKey(root, i)
            except OSError:
                break
            i += 1
            try:
                sub = winreg.OpenKey(root, g, 0,
                                     winreg.KEY_READ | winreg.KEY_WOW64_64KEY)
            except OSError:
                continue
            names, j = [], 0
            while True:
                try:
                    names.append(winreg.EnumKey(sub, j))
                except OSError:
                    break
                j += 1
            winreg.CloseKey(sub)
            low = [n.lower() for n in names]
            for h in hashes:
                hits = [n for n in low if h in n]
                if hits:
                    out[h].append((g, len(hits), hits[0]))
        winreg.CloseKey(root)
    except OSError:
        pass
    return out


def _winusb_init_probe(path, log):
    """Try CreateFile + WinUsb_Initialize on a path; report GetLastError.

    Read-only with respect to the device: no pipe IO, and the handle is freed
    immediately.  Claiming is implicit in WinUsb_Initialize and released by
    WinUsb_Free.  A dangling symbolic link (device re-enumerating) is retried
    once after a short delay, because that failure is transient.
    """
    for ov in (False, True):
        flags = FILE_FLAG_OVERLAPPED if ov else 0
        mode = "overlapped" if ov else "sync      "
        h = k32.CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, None,
                            OPEN_EXISTING, flags, None)
        if not h or h == INVALID_HANDLE_VALUE:
            e = _err()
            if e in (ERROR_FILE_NOT_FOUND, ERROR_NOT_FOUND):
                time.sleep(1.0)
                h = k32.CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, None,
                                    OPEN_EXISTING, flags, None)
                if h and h != INVALID_HANDLE_VALUE:
                    log("      CreateFileW (%s) OK after 1 s retry (link was"
                        " dangling - device re-enumerating)" % mode)
                else:
                    e = _err()
            if not h or h == INVALID_HANDLE_VALUE:
                extra = ""
                if e in (ERROR_FILE_NOT_FOUND, ERROR_NOT_FOUND):
                    extra = ("  -> dangling/absent symbolic link: the device was"
                             " re-enumerating or is gone")
                elif e == ERROR_ACCESS_DENIED:
                    extra = "  -> interface busy / claimed elsewhere"
                log("      CreateFileW (%s) FAILED  err=%s%s"
                    % (mode, _winerr(e), extra))
                continue
        hw = wt.HANDLE()
        ok = winusb.WinUsb_Initialize(h, ctypes.byref(hw))
        e = _err()
        if ok:
            spd = wt.ULONG(0)
            ln = wt.ULONG(4)
            winusb.WinUsb_QueryDeviceInformation(
                hw, DEVICE_SPEED, ctypes.byref(ln), ctypes.byref(spd))
            log("      CreateFileW (%s) OK; WinUsb_Initialize OK  speed=%s"
                % (mode, SPEED_NAMES.get(spd.value, spd.value)))
            winusb.WinUsb_Free(hw)
        else:
            log("      CreateFileW (%s) OK; WinUsb_Initialize FAILED  err=%s"
                % (mode, _winerr(e)))
        k32.CloseHandle(h)


def run_diagnose(args, guids, log):
    """Explain, with evidence, whether the vendor interface is reachable."""
    log("=" * 78)
    log("READ-ONLY DIAGNOSIS: is the WinUSB vendor interface reachable?")
    log("=" * 78)
    try:
        nodes = enumerate_winusb_devnodes(args.vid)
    except Exception as exc:
        log("  devnode enumeration failed: %s" % exc)
        return EXIT_TOOL
    if not nodes:
        log("  no devnode with VID %s present"
            % (hex(args.vid) if args.vid is not None else "any"))
        return EXIT_TOOL

    # All present WINUSB-bound devnodes on the host (any VID).  Needed both for
    # the correlation table and to learn which GUIDs other devices declare.
    try:
        all_winusb = [n for n in enumerate_winusb_devnodes(None)
                      if (n["service"] or "").upper() == "WINUSB"]
    except Exception:
        all_winusb = []

    # ---- candidate GUID list: built-ins, declared values, recorded subkeys --
    probe = list(guids)

    def _add_probe(gs):
        if gs and gs.upper() not in [p.upper() for p in probe]:
            probe.append(gs)

    guid_re = re.compile(r"\{[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-"
                         r"[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\}")
    for n in list(nodes) + list(all_winusb):
        for g in n["devparam_subkeys"]:
            gs = "{%s}" % g.strip("{}").upper()
            if re.fullmatch(r"\{[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-"
                            r"[0-9A-F]{4}-[0-9A-F]{12}\}", gs):
                _add_probe(gs)
        for key in ("DeviceInterfaceGUIDs", "DeviceInterfaceGUID"):
            v = n["devparam_values"].get(key)
            vals = v if isinstance(v, list) else ([v] if v else [])
            for item in vals:
                for hit in guid_re.findall(str(item).strip()):
                    _add_probe(hit.upper())

    # ---- (1) per-GUID candidate list with device paths ---------------------
    log("")
    log("[1] device-interface enumeration, once per GUID")
    maps = {}
    for g in probe:
        try:
            entries = enumerate_candidates([g], (lambda *_a: None))
        except Exception as exc:
            log("  %s -> enumeration error %s" % (g, exc))
            entries = []
        m = {}
        for e in entries:
            m.setdefault(e.get("instance_id") or "", []).append(e)
        maps[g.upper()] = m
        log("")
        log("  %s" % g)
        if not entries:
            log("      -> 0 device interfaces")
        for e in entries:
            vs = (e.get("service") or "").upper() == "WINUSB"
            log("      service=%-12s class=%-10s %s%s"
                % (e.get("service"), e.get("class"), e["path"],
                   "   [WinUSB]" if vs else ""))
            log("          instance=%s" % e.get("instance_id"))

    # ---- (2) DeviceClasses registry hits for each instance hash ------------
    hashes = {}
    for n in nodes:
        h = (n["instance_id"] or "").rsplit("\\", 1)[-1].lower()
        if h:
            hashes[n["instance_id"]] = h
    log("")
    log("[2] DeviceClasses registry entries whose symbolic link contains the"
        " devnode instance hash")
    hits = _deviceclasses_hits(list(hashes.values()))
    if "__error__" in hits:
        log("  registry scan failed: %s" % hits["__error__"])
    for inst, h in hashes.items():
        rows = hits.get(h) or []
        if not rows:
            log("  %-58s hash=%-20s -> 0 interface GUIDs" % (inst[:58], h))
        for g, cnt, sample in rows:
            log("  %-58s hash=%-20s -> %s (x%d)" % (inst[:58], h, g, cnt))

    # ---- (3) Device Parameters / DeviceInterfaceGUIDs + reachability -------
    log("")
    log("[3] devnode detail: DeviceInterfaceGUIDs presence and registered paths")
    winusb_usable, path_only = [], []
    for n in nodes:
        log("")
        log("  instance : %s" % n["instance_id"])
        log("    class  : %s %s" % (n["class"], n["classguid"]))
        log("    service: %s%s" % (n["service"],
                                   "   <-- WinUSB function driver"
                                   if (n["service"] or "").upper() == "WINUSB"
                                   else ""))
        log("    hwids  : %s" % (n["hwids"],))
        log("    DevParam subkeys: %s" % (n["devparam_subkeys"] or "(none)"))
        plural = n["devparam_values"].get("DeviceInterfaceGUIDs")
        singular = n["devparam_values"].get("DeviceInterfaceGUID")
        is_winusb = (n["service"] or "").upper() == "WINUSB"
        if plural is None and singular is None:
            log("    DeviceInterfaceGUIDs : *** ABSENT ***")
            if is_winusb:
                log("        => winusb.sys registers NO interface for this"
                    " devnode (registration is gated on this value)")
        else:
            log("    DeviceInterfaceGUIDs : %s" % (plural,))
            log("    DeviceInterfaceGUID  : %s" % (singular,))
        mine = []
        for g, m in maps.items():
            for e in m.get(n["instance_id"], []):
                mine.append((g, e["path"]))
        if not mine:
            log("    => NOT openable: no device interface registered")
            continue
        for g, p in mine:
            log("    => registered under %s" % g)
            log("       path: %s" % p)
            if (n["service"] or "").upper() == "WINUSB":
                winusb_usable.append((n["instance_id"], g, p))
            else:
                path_only.append((n["instance_id"], g, p))

    # ---- (4) WinUsb_Initialize attempt on every registered path ------------
    log("")
    log("[4] WinUsb_Initialize probe on every registered candidate path")
    allpaths = []
    for g, m in maps.items():
        for inst, es in m.items():
            for e in es:
                if e["path"] not in [p for _, _, p in allpaths]:
                    allpaths.append((inst, g, e["path"]))
    if not allpaths:
        log("  (no candidate path to try - this is the blocker)")
    for inst, g, p in allpaths:
        log("")
        log("  candidate: %s" % p)
        log("    guid=%s  instance=%s" % (g, inst))
        try:
            _winusb_init_probe(p, log)
        except Exception as exc:
            log("      probe raised %s" % exc)

    # ---- (5) WINUSB correlation table: declaration gates registration -----
    log("")
    log("[5] every present devnode with service==WINUSB: does it declare a")
    log("    DeviceInterfaceGUID(s), and what did SetupDi actually enumerate?")
    log("    (READ-ONLY; this is the gating proof)")
    corr = {"decl_reg": 0, "decl_noreg": 0, "nodecl_reg": 0, "nodecl_noreg": 0}
    for k, n in enumerate(all_winusb, 1):
        plural = n["devparam_values"].get("DeviceInterfaceGUIDs")
        singular = n["devparam_values"].get("DeviceInterfaceGUID")
        if plural is not None:
            decl = "DeviceInterfaceGUIDs = %s" % (plural,)
        elif singular is not None:
            decl = "DeviceInterfaceGUID (legacy) = %s" % (singular,)
        else:
            decl = "*** NONE ***"
        regs = sorted(g for g, m in maps.items() if m.get(n["instance_id"]))
        log("")
        log("    [%d] %s" % (k, n["instance_id"]))
        log("        declares        : %s" % decl)
        log("        registered GUIDs: %s" % (", ".join(regs) if regs else "(none)"))
        log("        -> %s" % ("registers %d interface(s)" % len(regs)
                              if regs else "registers NOTHING"))
        declared = plural is not None or singular is not None
        if declared and regs:
            corr["decl_reg"] += 1
        elif declared and not regs:
            corr["decl_noreg"] += 1
        elif not declared and regs:
            corr["nodecl_reg"] += 1
        else:
            corr["nodecl_noreg"] += 1
    log("")
    log("    correlation over %d WINUSB devnode(s):" % len(all_winusb))
    log("      declares GUID  + registers >=1 : %d" % corr["decl_reg"])
    log("      declares GUID  + registers 0  : %d  (expected 0)" % corr["decl_noreg"])
    log("      declares NONE  + registers >=1 : %d  (expected 0)" % corr["nodecl_reg"])
    log("      declares NONE  + registers 0  : %d" % corr["nodecl_noreg"])
    if (all_winusb and corr["decl_noreg"] == 0 and corr["nodecl_reg"] == 0
            and corr["decl_reg"] >= 1 and corr["nodecl_noreg"] >= 1):
        log("    => registration is GATED on a DeviceInterfaceGUID(s) value.")
        log("       A WCID-composite vendor interface (bound by the inbox")
        log("       winusb.inf, no such value) therefore gets NO device path")
        log("       and CANNOT be opened by any WinUSB host application.")
    elif not all_winusb:
        log("    => no WINUSB devnode present (device re-enumerating?); the")
        log("       correlation could not be evaluated in this run.")
    else:
        log("    => correlation inconclusive in this run (need at least one")
        log("       devnode in each group); inspect the rows above.")

    # ---- verdict ----------------------------------------------------------
    log("")
    log("-- verdict --")
    if winusb_usable:
        for inst, g, p in winusb_usable:
            log("  PASS: %s is a WinUSB device interface" % inst)
            log("        via %s" % g)
        return EXIT_PASS
    for inst, g, p in path_only:
        log("  path exists but driver is not WinUSB: %s (%s)" % (inst, g))
    log("  BLOCKED: no devnode with VID %s exposes a WinUSB-usable device"
        % (hex(args.vid) if args.vid is not None else "any"))
    log("           interface, so it cannot be opened with")
    log("           CreateFile / WinUsb_Initialize.")
    log("")
    log("  Root cause: %s" % _winusb_inf_note())
    log("  Plus: interface registration is gated on a DeviceInterfaceGUID(s)")
    log("        value in the device key (measured: the one WINUSB devnode that")
    log("        declares one gets an interface, the one that does not gets none).")
    log("")
    log("  Fixes (pick one):")
    log("   (a) firmware (recommended): add an MS OS 2.0 Registry Property")
    log("       feature (wDescriptorType=4, wPropertyDataType=0x0007 REG_MULTI_SZ,")
    log("       PropertyData UTF-16LE with a trailing double NUL) on the VENDOR")
    log("       interface, name 'DeviceInterfaceGUIDs', value")
    log("         %s" % GUID_DEVINTERFACE_USB_DEVICE)
    log("       (that is the GUID Chrome/Edge WebUSB enumerates; REG_MULTI_SZ may")
    log("       also carry %s)" % GUID_DEVINTERFACE_WINUSB)
    log("   (b) host workaround (needs Administrator):")
    for n in nodes:
        if (n["service"] or "").upper() == "WINUSB":
            log("       reg add \"HKLM\\SYSTEM\\CurrentControlSet\\Enum\\%s\\Device Parameters\" /v DeviceInterfaceGUIDs /t REG_MULTI_SZ /d \"%s\" /f"
                % (n["instance_id"], GUID_DEVINTERFACE_USB_DEVICE))
            break
    log("       then disable/enable the device (Device Manager or pnputil).")
    log("   (c) design: make the vendor function non-composite (bDeviceClass=")
    log("       0xFF, no IAD/0xEF).  winusb.sys then binds the whole device and")
    log("       the {A5DCBF10-...} USB device path is directly WinUSB-openable.")
    return EXIT_TOOL


def _winusb_inf_note():
    guid, inf = discover_winusb_guid()
    if not inf:
        return "winusb.inf not found"
    return ("%s maps 'USB\\MS_COMP_WINUSB' to the WINUSB section, but that"
            " section's .HW sub-section is empty, so no DeviceInterfaceGUIDs"
            " is written (the only DeviceInterfaceGUIDs in that INF belongs to"
            " the ADB section)." % inf)


# ---------------------------------------------------------------------------
# IN stream: heartbeat stripping + echo accumulator
# ---------------------------------------------------------------------------
class InStream:
    """Splits the shared IN stream into loopback echo bytes and HB heartbeat lines.

    The loopback payload is generated LF-free (byte 0x0A never appears), so a LF
    byte in the IN stream can only come from the firmware heartbeat, which is
    written as "\\nHB <uptime_ms>\\n".  That makes the split unambiguous.
    """

    def __init__(self):
        self.cv = threading.Condition()
        self.echo = bytearray()
        self.total_in = 0
        self.hb_bytes = 0
        self.hb_events = []          # (host_monotonic, uptime_ms, raw_line)
        self.pre_case_junk = 0
        self.stray_lf = 0
        self.dead = False
        self.dead_reason = None
        self.last_growth = None
        self._pending = bytearray()  # bytes after an unmatched LF
        self._in_line = False        # True while inside a possible HB line
        self.raw_head = bytearray()  # first RAW IN bytes since begin_case()

    # -- reader thread side -------------------------------------------------
    def feed(self, data):
        out = bytearray()
        pend = self._pending
        hb_hits = []
        stray = 0
        in_line = self._in_line
        for byte in data:
            if in_line:
                if byte == 0x0A:
                    line = bytes(pend)
                    pend.clear()
                    in_line = False
                    s = line.strip()
                    if s.startswith(b"HB "):
                        rest = s[3:].strip()
                        ms = int(rest) if rest.isdigit() else None
                        hb_hits.append((time.perf_counter(), ms, line))
                    else:
                        stray += 1
                        out += b"\n" + line + b"\n"
                    continue
                pend.append(byte)
                if len(pend) > 64:   # implausible HB line: give the bytes back
                    stray += 1
                    out += b"\n" + bytes(pend)
                    pend.clear()
                    in_line = False
                continue
            if byte == 0x0A:
                pend.clear()
                in_line = True
                continue
            out.append(byte)
        self._in_line = in_line
        with self.cv:
            self.total_in += len(data)
            if len(self.raw_head) < 128:
                self.raw_head.extend(data[:128 - len(self.raw_head)])
            if out:
                self.echo.extend(out)
                self.last_growth = time.perf_counter()
            for ts, ms, line in hb_hits:
                self.hb_events.append((ts, ms, line))
                self.hb_bytes += len(line) + 2
            self.stray_lf += stray
            self.cv.notify_all()

    def mark_dead(self, reason):
        with self.cv:
            self.dead = True
            self.dead_reason = reason
            self.cv.notify_all()

    # -- verifier side ------------------------------------------------------
    def echo_len(self):
        with self.cv:
            return len(self.echo)

    def wait_echo(self, nbytes, timeout_s):
        deadline = time.perf_counter() + timeout_s
        with self.cv:
            while len(self.echo) < nbytes:
                if self.dead:
                    return False
                remain = deadline - time.perf_counter()
                if remain <= 0:
                    return False
                self.cv.wait(min(remain, 0.25))
            return True

    def begin_case(self, quiet_s=0.4, max_wait_s=3.0):
        """Wait for the IN stream to go quiet, then drop whatever is left."""
        t_start = time.perf_counter()
        last = self.echo_len()
        stable_since = time.perf_counter()
        while time.perf_counter() - t_start < max_wait_s:
            time.sleep(0.05)
            cur = self.echo_len()
            if cur != last:
                last = cur
                stable_since = time.perf_counter()
            elif time.perf_counter() - stable_since >= quiet_s:
                break
        with self.cv:
            junk = len(self.echo)
            self.echo.clear()
            self.raw_head.clear()
            self.pre_case_junk += junk
            # CRITICAL: reset the heartbeat-stripping state too.  A previous
            # reader can be stopped in the middle of a "\nHB ..." line, leaving
            # _in_line=True / _pending non-empty.  If that leaked into the next
            # case, the case's first 0x0A would close the stale fragment and the
            # stream would be mangled from offset 0 (observed: strayLFlines=2,
            # hbBytes=0, +13 junk bytes, 1-byte mismatch at offset 0).
            self._pending.clear()
            self._in_line = False
        return junk

    def snapshot_hb(self):
        with self.cv:
            return list(self.hb_events)


# ---------------------------------------------------------------------------
# WinUSB device
# ---------------------------------------------------------------------------
class WinUsbDevice:
    def __init__(self, path, overlapped=True, io_retry_delay=10.0):
        self.path = path
        self.overlapped = overlapped
        self.file = None
        self.handle = None
        self.speed = None
        self.iface = None
        self.pipes = []          # list of dicts
        self.api_note = []

    # -- lifecycle ----------------------------------------------------------
    def open(self, access=GENERIC_READ | GENERIC_WRITE):
        flags = FILE_FLAG_OVERLAPPED if self.overlapped else 0
        h = k32.CreateFileW(self.path, access,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, None,
                            OPEN_EXISTING, flags, None)
        if not h or h == INVALID_HANDLE_VALUE:
            e = _err()
            hint = ""
            if e == ERROR_ACCESS_DENIED:
                hint = ("  -> interface already claimed / opened exclusively "
                        "(e.g. a browser tab holding claimInterface, or another "
                        "copy of this tool). Close it and retry.")
            elif e in (ERROR_FILE_NOT_FOUND, ERROR_NOT_FOUND):
                hint = "  -> device re-enumerating or unplugged."
            raise ToolError("CreateFileW failed: %s%s" % (_winerr(e), hint))
        self.file = h
        hw = wt.HANDLE()
        if not winusb.WinUsb_Initialize(h, ctypes.byref(hw)):
            e = _err()
            k32.CloseHandle(h)
            self.file = None
            raise ToolError(
                "WinUsb_Initialize failed: %s  -> this device path is not bound "
                "to WinUSB (usbccgp parent nodes behave this way)." % _winerr(e))
        self.handle = hw
        self._apply_default_policies()

    def _apply_default_policies(self):
        to = wt.ULONG(2000)
        try:
            if not winusb.WinUsb_SetPipePolicy(
                    self.handle, 0, PIPE_TRANSFER_TIMEOUT, 4, ctypes.byref(to)):
                self.api_note.append("SetPipePolicy(EP0 timeout) failed: %s"
                                     % _winerr(_err()))
        except Exception as exc:
            self.api_note.append("SetPipePolicy(EP0) raised %s" % exc)

    def set_transfer_timeout(self, pipe_id, ms):
        val = wt.ULONG(int(ms))
        if not winusb.WinUsb_SetPipePolicy(
                self.handle, pipe_id, PIPE_TRANSFER_TIMEOUT, 4,
                ctypes.byref(val)):
            self.api_note.append(
                "SetPipePolicy(PIPE_TRANSFER_TIMEOUT=%d) on pipe 0x%02X failed: %s"
                % (ms, pipe_id, _winerr(_err())))
        one = ctypes.c_ubyte(1)
        if not winusb.WinUsb_SetPipePolicy(
                self.handle, pipe_id, ALLOW_PARTIAL_READS, 1,
                ctypes.byref(one)):
            self.api_note.append(
                "SetPipePolicy(ALLOW_PARTIAL_READS) on pipe 0x%02X failed: %s"
                % (pipe_id, _winerr(_err())))

    def quit(self):
        try:
            for p in self.pipes:
                try:
                    winusb.WinUsb_AbortPipe(self.handle, p["pipe_id"])
                except Exception:
                    pass
        except Exception:
            pass
        if self.handle:
            try:
                winusb.WinUsb_Free(self.handle)
            except Exception:
                pass
            self.handle = None
        if self.file:
            try:
                k32.CloseHandle(self.file)
            except Exception:
                pass
            self.file = None

    # -- discovery ---------------------------------------------------------
    def query(self):
        alt = ctypes.c_ubyte(0)
        cur_alt = None
        if winusb.WinUsb_GetCurrentAlternateSetting(self.handle, ctypes.byref(alt)):
            cur_alt = alt.value
        d = USB_INTERFACE_DESCRIPTOR()
        if not winusb.WinUsb_QueryInterfaceSettings(self.handle, 0, ctypes.byref(d)):
            raise ToolError("WinUsb_QueryInterfaceSettings failed: %s"
                            % _winerr(_err()))
        self.iface = {
            "bLength": d.bLength,
            "bInterfaceNumber": d.bInterfaceNumber,
            "bAlternateSetting": d.bAlternateSetting,
            "bNumEndpoints": d.bNumEndpoints,
            "bInterfaceClass": d.bInterfaceClass,
            "bInterfaceSubClass": d.bInterfaceSubClass,
            "bInterfaceProtocol": d.bInterfaceProtocol,
            "iInterface": d.iInterface,
            "current_alt": cur_alt,
        }
        self.pipes = []
        for i in range(d.bNumEndpoints):
            p = WINUSB_PIPE_INFORMATION()
            if not winusb.WinUsb_QueryPipe(self.handle, 0, i, ctypes.byref(p)):
                raise ToolError("WinUsb_QueryPipe(%d) failed: %s" % (i, _winerr(_err())))
            self.pipes.append({
                "index": i,
                "pipe_id": p.PipeId & 0xFF,
                "type": p.PipeType,
                "max_packet": p.MaximumPacketSize & 0xFFFF,
                "interval": p.Interval & 0xFF,
                "direction": "IN" if (p.PipeId & 0x80) else "OUT",
            })
        ln = wt.ULONG(4)
        spd = wt.ULONG(0)
        if winusb.WinUsb_QueryDeviceInformation(
                self.handle, DEVICE_SPEED, ctypes.byref(ln), ctypes.byref(spd)):
            self.speed = spd.value
        return self.iface

    def set_alt_setting(self, n):
        return bool(winusb.WinUsb_SetCurrentAlternateSetting(self.handle, n))

    def find_pipes(self):
        out_p = [p for p in self.pipes if p["direction"] == "OUT"]
        in_p = [p for p in self.pipes if p["direction"] == "IN"]
        bulk_out = [p for p in out_p if p["type"] == 2]
        bulk_in = [p for p in in_p if p["type"] == 2]
        # Prefer a matching bulk pair even if the endpoint numbers differ.
        bism = {}
        bosm = {}
        for p in bulk_in:
            bism[p["max_packet"]] = p
        for p in bulk_out:
            bosm[p["max_packet"]] = p
        pair = None
        for mp in sorted(set(bism) & set(bosm)):
            pair = (bism[mp], bosm[mp])
            break
        return {
            "in": pair[0] if pair else (bulk_in[0] if bulk_in else None),
            "out": pair[1] if pair else (bulk_out[0] if bulk_out else None),
        }

    # -- raw pipe IO -------------------------------------------------------
    def _new_overlapped(self):
        ev = k32.CreateEventW(None, True, False, None)
        if not ev:
            raise ToolError("CreateEventW failed: %s" % _winerr(_err()))
        ov = OVERLAPPED()
        ov.hEvent = ev
        return ov

    def read(self, pipe_id, length, timeout_ms):
        """Returns bytes (b'' on timeout). Raises ToolError on hard failure."""
        buf = (ctypes.c_ubyte * length)()
        transferred = wt.ULONG(0)
        if self.overlapped:
            ov = self._new_overlapped()
            try:
                ok = winusb.WinUsb_ReadPipe(
                    self.handle, pipe_id, buf, length,
                    ctypes.byref(transferred), ctypes.byref(ov))
                if not ok:
                    e = _err()
                    if e != ERROR_IO_PENDING:
                        if e in (ERROR_SEM_TIMEOUT, ERROR_OPERATION_ABORTED):
                            return b""
                        raise ToolError("WinUsb_ReadPipe(0x%02X) failed: %s"
                                        % (pipe_id, _winerr(e)))
                rc = k32.WaitForSingleObject(ov.hEvent, int(timeout_ms))
                if rc == WAIT_TIMEOUT:
                    k32.CancelIoEx(self.file, ctypes.byref(ov))
                    k32.GetOverlappedResult(self.file, ctypes.byref(ov),
                                            ctypes.byref(transferred), True)
                    return b""
                if rc != WAIT_OBJECT_0:
                    raise ToolError("WaitForSingleObject(read) rc=%d" % rc)
                if not k32.GetOverlappedResult(self.file, ctypes.byref(ov),
                                               ctypes.byref(transferred), False):
                    e = _err()
                    if e in (ERROR_OPERATION_ABORTED, ERROR_SEM_TIMEOUT):
                        return b""
                    raise ToolError("GetOverlappedResult(read) failed: %s"
                                    % _winerr(e))
                return bytes(bytearray(buf[:transferred.value]))
            finally:
                k32.CloseHandle(ov.hEvent)
        else:
            ok = winusb.WinUsb_ReadPipe(
                self.handle, pipe_id, buf, length, ctypes.byref(transferred), None)
            if not ok:
                e = _err()
                if e in (ERROR_SEM_TIMEOUT, ERROR_OPERATION_ABORTED):
                    return b""
                raise ToolError("WinUsb_ReadPipe(0x%02X, sync) failed: %s"
                                % (pipe_id, _winerr(e)))
            return bytes(bytearray(buf[:transferred.value]))

    def write(self, pipe_id, data, timeout_ms):
        """Returns (ok, nbytes). ok=False means timeout."""
        n = len(data)
        buf = (ctypes.c_ubyte * n).from_buffer_copy(data)
        transferred = wt.ULONG(0)
        if self.overlapped:
            ov = self._new_overlapped()
            try:
                ok = winusb.WinUsb_WritePipe(
                    self.handle, pipe_id, buf, n,
                    ctypes.byref(transferred), ctypes.byref(ov))
                if not ok:
                    e = _err()
                    if e != ERROR_IO_PENDING:
                        if e == ERROR_SEM_TIMEOUT:
                            return False, 0
                        raise ToolError("WinUsb_WritePipe(0x%02X) failed: %s"
                                        % (pipe_id, _winerr(e)))
                rc = k32.WaitForSingleObject(ov.hEvent, int(timeout_ms))
                if rc == WAIT_TIMEOUT:
                    k32.CancelIoEx(self.file, ctypes.byref(ov))
                    k32.GetOverlappedResult(self.file, ctypes.byref(ov),
                                            ctypes.byref(transferred), True)
                    return False, transferred.value
                if rc != WAIT_OBJECT_0:
                    raise ToolError("WaitForSingleObject(write) rc=%d" % rc)
                if not k32.GetOverlappedResult(self.file, ctypes.byref(ov),
                                               ctypes.byref(transferred), False):
                    e = _err()
                    if e == ERROR_SEM_TIMEOUT:
                        return False, transferred.value
                    raise ToolError("GetOverlappedResult(write) failed: %s"
                                    % _winerr(e))
                return True, transferred.value
            finally:
                k32.CloseHandle(ov.hEvent)
        else:
            ok = winusb.WinUsb_WritePipe(
                self.handle, pipe_id, buf, n, ctypes.byref(transferred), None)
            if not ok:
                e = _err()
                if e == ERROR_SEM_TIMEOUT:
                    return False, transferred.value
                raise ToolError("WinUsb_WritePipe(0x%02X, sync) failed: %s"
                                % (pipe_id, _winerr(e)))
            return True, transferred.value

    def abort_pipe(self, pipe_id):
        try:
            winusb.WinUsb_AbortPipe(self.handle, pipe_id)
        except Exception:
            pass

    def reset_pipe(self, pipe_id):
        try:
            winusb.WinUsb_ResetPipe(self.handle, pipe_id)
        except Exception:
            pass


# ---------------------------------------------------------------------------
# Reader thread
# ---------------------------------------------------------------------------
class Reader(threading.Thread):
    def __init__(self, dev, pipe_id, read_size, timeout_ms, stream, name="in-reader"):
        super().__init__(name=name, daemon=True)
        self.dev = dev
        self.pipe_id = pipe_id
        self.read_size = read_size
        self.timeout_ms = timeout_ms
        self.stream = stream
        self.stop_flag = threading.Event()
        self.error = None
        self.cycles = 0
        self.bytes = 0
        self.empty_timeouts = 0

    def run(self):
        while not self.stop_flag.is_set():
            try:
                data = self.dev.read(self.pipe_id, self.read_size, self.timeout_ms)
            except Exception as exc:
                if self.stop_flag.is_set():
                    # Shutdown race: AbortPipe() makes the in-flight request fail.
                    # That is expected, not a device/tool error.
                    return
                self.error = "%s" % exc
                self.stream.mark_dead(self.error)
                return
            self.cycles += 1
            if data:
                self.bytes += len(data)
                self.stream.feed(data)
            else:
                self.empty_timeouts += 1
                if self.stop_flag.is_set():
                    return

    def stop(self):
        self.stop_flag.set()
        self.dev.abort_pipe(self.pipe_id)
        # Clear the abort/halt state so the NEXT reader on this pipe starts
        # clean; without this the next WinUsb_ReadPipe can fail immediately.
        self.dev.reset_pipe(self.pipe_id)


# ---------------------------------------------------------------------------
# Payload + loopback
# ---------------------------------------------------------------------------
def make_payload(n, seed):
    """Deterministic, offset-unique, LF-free pseudorandom payload.

    Byte 0x0A (LF) is remapped so the heartbeat delimiter can never appear in
    loopback data -> the HB split is unambiguous.
    """
    buf = bytearray(n)
    x = (seed ^ 0x9E3779B9) & 0xFFFFFFFF
    for i in range(n):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= (x >> 17)
        x ^= (x << 5) & 0xFFFFFFFF
        b = x & 0xFF
        buf[i] = 0x0B if b == 0x0A else b
    return bytes(buf)


def human_bytes(n):
    if n >= 1024 * 1024:
        return "%.3f MiB" % (n / (1024.0 * 1024.0))
    if n >= 1024:
        return "%.3f KiB" % (n / 1024.0)
    return "%d B" % n


def _hexlines(data, width=16):
    out = []
    for i in range(0, len(data), width):
        chunk = data[i:i + width]
        hexs = " ".join("%02X" % b for b in chunk)
        asc = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        out.append("%04X  %-*s  |%s|" % (i, width * 3 - 1, hexs, asc))
    return out


def hex_diff_block(expected, actual, indent="", width=16, limit=64):
    """Side-by-side hex of the first bytes, marking mismatching columns."""
    exp = expected[:limit]
    act = actual[:limit]
    n = max(len(exp), len(act))
    lines = ["%s%-6s %-*s   %-*s   %s"
             % (indent, "off", width * 3 - 1, "EXPECTED", width * 3 - 1,
                "ACTUAL", "diff")]
    for i in range(0, n, width):
        e = exp[i:i + width]
        a = act[i:i + width]
        eh = " ".join("%02X" % b for b in e).ljust(width * 3 - 1)
        ah = " ".join("%02X" % b for b in a).ljust(width * 3 - 1)
        mark = ""
        for j in range(width):
            if i + j >= max(len(exp), len(act)):
                break
            eb = exp[i + j] if i + j < len(exp) else None
            ab = act[i + j] if i + j < len(act) else None
            mark += ("^" if eb != ab else " ")
        lines.append("%s%04X   %s   %s   %s" % (indent, i, eh, ah, mark))
    if not exp:
        lines.append("%s(no expected bytes)" % indent)
    if not act:
        lines.append("%s(no actual bytes received)" % indent)
    return "\n".join(lines)


def hex_dump_block(data, label, indent="", width=16):
    lines = ["%s%s:" % (indent, label)]
    for ln in _hexlines(data, width):
        lines.append("%s  %s" % (indent, ln))
    return "\n".join(lines)


class CaseResult:
    def __init__(self, size, chunk):
        self.size = size
        self.chunk = chunk
        self.ok = False
        self.reason = None
        self.written = 0
        self.verified = 0
        self.mismatches = 0
        self.first_bad_offset = None
        self.first_bad_expected = None
        self.first_bad_actual = None
        self.elapsed = 0.0
        self.rtt_min = None
        self.rtt_avg = None
        self.rtt_max = None
        self.rtt_samples = 0
        self.pre_junk = 0
        self.hb_before = 0
        self.hb_after = 0
        self.write_timeouts = 0
        self.echo_timeouts = 0
        self.blocks = 0
        # diagnostic heads (first N bytes) for hex comparison
        self.head_expected = b""
        self.head_actual = b""
        self.raw_head = b""
        self.echo_total = 0
        # raw IN accounting (the number the stream-alignment diagnosis needs):
        #   k = raw_in - size   -> total stream shift, in bytes
        self.raw_in = 0
        self.hb_bytes = 0
        self.stray_lf = 0

    def mbps(self):
        if self.elapsed <= 0:
            return 0.0
        return (self.written * 8.0 / 1e6) / self.elapsed  # Mbit/s

    def mibps(self):
        if self.elapsed <= 0:
            return 0.0
        return (self.written / (1024.0 * 1024.0)) / self.elapsed


def run_case(dev, stream, size, chunk, in_pipe, out_pipe, op_timeout_ms,
             case_timeout_s, pipeline, quiet_s, log, hexdump=64):
    res = CaseResult(size, chunk)
    payload = make_payload(size, 0x5EED0000 ^ size)
    res.pre_junk = stream.begin_case(quiet_s=quiet_s)
    res.hb_before = len(stream.snapshot_hb())
    with stream.cv:
        _raw0 = stream.total_in
        _hb0 = stream.hb_bytes
        _stray0 = stream.stray_lf
    if stream.dead:
        res.reason = "IN reader died before the case: %s" % stream.dead_reason
        return res

    in_pid = in_pipe["pipe_id"]
    out_pid = out_pipe["pipe_id"]
    # Cap the IN read size to the case size: for size < chunk a full-chunk read
    # would only complete on the pipe timeout (the device's echo of e.g. 1024 B
    # is exactly 2 full 512 B packets, i.e. no short packet to end the transfer).
    read_size = max(64, min(chunk, size))

    reader = Reader(dev, in_pid, read_size, op_timeout_ms, stream)
    reader.start()
    time.sleep(0.05)  # let the first IN read get posted BEFORE the first write

    offsets = list(range(0, size, chunk))
    pending = deque()
    rtts = []
    hard_deadline = time.perf_counter() + case_timeout_s
    t0 = time.perf_counter()
    try:
        for idx, off in enumerate(offsets):
            if time.perf_counter() > hard_deadline:
                res.reason = ("case total timeout (%.1fs) exceeded at offset %d/%d"
                              % (case_timeout_s, off, size))
                break
            blk = payload[off:off + chunk]
            n = len(blk)
            issue = time.perf_counter()
            ok, nw = dev.write(out_pid, blk, op_timeout_ms)
            done = time.perf_counter()
            res.blocks += 1
            if not ok:
                res.write_timeouts += 1
                res.reason = ("OUT write timed out after %d ms at offset %d "
                              "(wrote %d/%d B)" % (op_timeout_ms, off, nw, n))
                break
            res.written += nw
            pending.append((off, blk, issue, done))
            last = (idx == len(offsets) - 1)
            while pending and (len(pending) >= pipeline or last):
                poff, pblk, pissue, pdone = pending.popleft()
                target = poff + len(pblk)
                wl = max(0.0, min(hard_deadline, pdone + op_timeout_ms / 1000.0)
                         - time.perf_counter())
                if not stream.wait_echo(target, max(wl, 0.0)):
                    res.echo_timeouts += 1
                    if stream.dead:
                        res.reason = "IN reader died: %s" % stream.dead_reason
                    else:
                        res.reason = ("echo timeout waiting for %d B (have %d B) "
                                      "while verifying offset %d"
                                      % (target, stream.echo_len(), poff))
                    break
                got = bytes(stream.echo[poff:target])
                rtts.append(time.perf_counter() - pissue)
                if got != pblk:
                    res.mismatches += 1
                    if res.first_bad_offset is None:
                        res.first_bad_offset = poff
                        for i in range(len(pblk)):
                            if got[i] != pblk[i]:
                                res.first_bad_offset = poff + i
                                res.first_bad_expected = pblk[i]
                                res.first_bad_actual = got[i]
                                break
                res.verified += len(pblk)
            if res.reason:
                break
    finally:
        reader.stop()
        reader.join(timeout=3.0)
        try:
            dev.reset_pipe(out_pid)
        except Exception:
            pass

    if res.reason is None and reader.error:
        res.reason = "IN reader error: %s" % reader.error
    res.elapsed = time.perf_counter() - t0
    if rtts:
        res.rtt_min = min(rtts)
        res.rtt_max = max(rtts)
        res.rtt_avg = sum(rtts) / len(rtts)
        res.rtt_samples = len(rtts)
    res.hb_after = len(stream.snapshot_hb())
    with stream.cv:
        res.raw_in = stream.total_in - _raw0
        res.hb_bytes = stream.hb_bytes - _hb0
        res.stray_lf = stream.stray_lf - _stray0
    if hexdump > 0:
        with stream.cv:
            res.head_expected = bytes(payload[:hexdump])
            res.head_actual = bytes(stream.echo[:hexdump])
            res.raw_head = bytes(stream.raw_head[:hexdump])
            res.echo_total = len(stream.echo)
    res.ok = (res.reason is None and res.mismatches == 0
              and res.verified == size and res.written == size)
    if res.reason is None and not res.ok:
        res.reason = ("incomplete: wrote %d/%d B, verified %d/%d B, mismatches=%d"
                      % (res.written, size, res.verified, size, res.mismatches))
    log("  case %-10s chunk=%-5d wrote=%d verified=%d mismatch=%d %s"
        % (human_bytes(size), chunk, res.written, res.verified, res.mismatches,
           "PASS" if res.ok else "FAIL"))
    return res


def heartbeat_probe(dev, stream, in_pipe, idle_wait_s, op_timeout_ms, size, chunk, log):
    """Stop writing, wait for the firmware idle heartbeat, report device uptime."""
    info = {"events": [], "waited": idle_wait_s, "ok": False}
    before = stream.snapshot_hb()
    reader = Reader(dev, in_pipe["pipe_id"], max(chunk, 512), op_timeout_ms, stream)
    reader.start()
    t0 = time.perf_counter()
    try:
        while time.perf_counter() - t0 < idle_wait_s:
            time.sleep(0.05)
            if stream.snapshot_hb():
                break
    finally:
        reader.stop()
        reader.join(timeout=3.0)
    events = stream.snapshot_hb()
    info["events"] = events[len(before):]
    info["uptimes"] = [e[1] for e in info["events"] if e[1] is not None]
    info["ok"] = bool(info["events"])
    if info["ok"]:
        log("  heartbeat: %d line(s); device uptime %s ms"
            % (len(info["events"]), info["uptimes"]))
    else:
        log("  heartbeat: none seen within %.1fs" % idle_wait_s)
    return info


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------
def print_candidates(entries, log):
    log("")
    log("=" * 78)
    log("WinUSB device interface candidates")
    log("=" * 78)
    if not entries:
        log("  (none)")
        return
    for i, e in enumerate(entries):
        vid, pid, mi = e.get("vid"), e.get("pid"), e.get("mi")
        log("[%d] %s" % (i, e["path"]))
        log("    interface GUID : %s" % e["guid"])
        if vid is not None:
            log("    VID/PID/MI     : %04X / %s / %s"
                % (vid, ("%04X" % pid) if pid is not None else "-",
                   ("%02X" % mi) if mi is not None else "-"))
        log("    hardware IDs   : %s" % (e["hwids"],))
        log("    compatible IDs : %s" % (e["compatible"],))
        log("    friendly name  : %s" % e.get("friendly"))
        log("    device desc    : %s" % e.get("desc"))
        log("    bus reported   : %s" % e.get("busdesc"))
        log("    WinUSB-compat  : %s" % ("yes" if is_winusb_compatible(e) else "no"))


def print_pipes(dev, log):
    iface = dev.iface
    log("  interface: bInterfaceNumber=%d bAlternateSetting=%d bNumEndpoints=%d"
        % (iface["bInterfaceNumber"], iface["bAlternateSetting"],
           iface["bNumEndpoints"]))
    log("             class/subclass/protocol = 0x%02X/0x%02X/0x%02X  iInterface=%d"
        % (iface["bInterfaceClass"], iface["bInterfaceSubClass"],
           iface["bInterfaceProtocol"], iface["iInterface"]))
    log("             current alternate setting = %s" % iface["current_alt"])
    log("  pipes (%d):" % len(dev.pipes))
    log("    idx  PipeId  dir  type         wMaxPacketSize  Interval")
    log("    ---  ------  ---  -----------  --------------  --------")
    for p in dev.pipes:
        log("    %-3d  0x%02X    %-3s  %-11s  %-14d  %d"
            % (p["index"], p["pipe_id"], p["direction"],
               PIPE_TYPE_NAMES.get(p["type"], "?%d" % p["type"]),
               p["max_packet"], p["interval"]))


def print_report(lines, args, entries, chosen, dev, guids, guid_src, case_results,
                 hb_info, tool_notes, verdict, log):
    w = log
    w("")
    w("=" * 78)
    w("MeidaP4 host-side WinUSB report")
    w("=" * 78)
    w("timestamp        : %s" % time.strftime("%Y-%m-%d %H:%M:%S"))
    w("python           : %s" % sys.version.replace("\n", " "))
    w("host             : %s / %s" % (os.environ.get("COMPUTERNAME", "?"),
                                      os.environ.get("OS", "?")))
    w("tool             : tools/winusb_loopback.py")
    w("requested vid/pid/mi : %s / %s / %s"
      % (hex(args.vid) if args.vid is not None else "any",
         hex(args.pid) if args.pid is not None else "any",
         ("%02X" % args.mi) if args.mi is not None else "any"))
    w("io mode          : %s" % ("overlapped (FILE_FLAG_OVERLAPPED)" if dev.overlapped
                                 else "synchronous + reader thread"))
    w("")
    w("-- interface GUID discovery " + "-" * 50)
    w("winusb.inf       : %s" % (guid_src or "(not found)"))
    if guids and guids[0].upper() != GUID_DEVINTERFACE_WINUSB.upper():
        w("NOTE             : %s came from winusb.inf DeviceInterfaceGUIDs"
          % guids[0])
    for g in guids:
        w("probed GUID      : %s" % g)
    w("NOTE             : GUID_DEVINTERFACE_USB_DEVICE = %s is NOT the WinUSB"
      % GUID_DEVINTERFACE_USB_DEVICE)
    w("                   interface GUID; it only exposes the usbccgp parent,")
    w("                   which cannot be WinUsb_Initialize'd.")
    w("")
    w("-- candidate enumeration " + "-" * 54)
    w("interfaces found : %d" % len(entries))
    if chosen is None:
        w("selected         : (none)")
    else:
        w("selected path    : %s" % chosen["path"])
        w("selected GUID    : %s" % chosen["guid"])
        w("selected hwids   : %s" % (chosen["hwids"],))
        w("selected compat  : %s" % (chosen["compatible"],))
        w("selected friendly: %s" % (chosen.get("friendly"),))
        w("selected busdesc : %s" % (chosen.get("busdesc"),))
    if dev.iface is not None:
        w("")
        w("-- open / claim / pipes " + "-" * 55)
        w("CreateFileW      : OK (%s)" % ("overlapped" if dev.overlapped else "sync"))
        w("WinUsb_Initialize: OK")
        w("claim            : implicit via WinUsb_Initialize")
        w("                   (WinUsb_ClaimInterface is NOT exported by")
        w("                    winusb.dll on this system - verified by export scan)")
        spd = SPEED_NAMES.get(dev.speed, "unknown(%s)" % dev.speed)
        w("negotiated speed : %s" % spd)
        w("")
        print_pipes(dev, w)
        if dev.api_note:
            w("")
            w("driver notes     :")
            for n in dev.api_note:
                w("  - %s" % n)
    if case_results:
        w("")
        w("-- loopback results " + "-" * 58)
        w("case      chunk  bytes      elapsed    Mbit/s   MiB/s    RTT min/avg/max (ms)   mismatch  first-bad-off  verdict")
        for r in case_results:
            rtt = "-"
            if r.rtt_samples:
                rtt = "%.3f/%.3f/%.3f" % (r.rtt_min * 1e3, r.rtt_avg * 1e3, r.rtt_max * 1e3)
            w("%-9s %-6d %-10s %-9.3fs %-8.2f %-8.2f %-21s %-9d %-14s %s"
              % (human_bytes(r.size), r.chunk, human_bytes(r.written),
                 r.elapsed, r.mbps(), r.mibps(), rtt, r.mismatches,
                 r.first_bad_offset if r.first_bad_offset is not None else "-",
                 "PASS" if r.ok else "FAIL"))
            if not r.ok:
                w("    reason : %s" % r.reason)
            if r.first_bad_offset is not None:
                w("    first mismatch @ offset %d: expected 0x%02X got 0x%02X"
                  % (r.first_bad_offset, r.first_bad_expected or 0,
                     r.first_bad_actual or 0))
            w("    pre-case carry-over drained = %d B (previous case's echo, "
              "discarded to re-zero the offset), hb lines before/after = %d/%d, "
              "write-timeouts = %d, echo-timeouts = %d, blocks = %d"
              % (r.pre_junk, r.hb_before, r.hb_after, r.write_timeouts,
                 r.echo_timeouts, r.blocks))
            w("    rawInBytes = %d  (size = %d, k = rawIn - size = %+d)  "
              "hbBytes = %d  strayLFlines = %d  strippedEcho = %d"
              % (r.raw_in, r.size, r.raw_in - r.size, r.hb_bytes, r.stray_lf,
                 r.echo_total))
            if not r.ok and (r.head_expected or r.head_actual):
                w("    echo buffer held %d B total (expected %d B)"
                  % (r.echo_total, r.size))
                w("    --- first bytes: EXPECTED vs ACTUAL(received, HB stripped) ---")
                w(hex_diff_block(r.head_expected, r.head_actual, indent="      "))
                if r.raw_head and r.raw_head != r.head_actual[:len(r.raw_head)]:
                    w("    --- first bytes RAW (before HB stripping) ---")
                    w(hex_dump_block(r.raw_head,
                                     "RAW IN", indent="      "))
        total_w = sum(r.written for r in case_results)
        total_t = sum(r.elapsed for r in case_results)
        w("total            : %s in %.3fs (%.2f Mbit/s)"
          % (human_bytes(total_w), total_t,
             (total_w * 8.0 / 1e6) / total_t if total_t else 0.0))
    if hb_info is not None:
        w("")
        w("-- heartbeat " + "-" * 65)
        w("waited           : %.1fs idle" % hb_info["waited"])
        w("lines parsed     : %d" % len(hb_info["events"]))
        if hb_info["events"]:
            w("device uptime    : %s ms (%s s)"
              % (hb_info["uptimes"], ", ".join("%.1f" % (u / 1000.0) for u in hb_info["uptimes"])))
            w("raw lines        : %s" % ([e[2].decode("latin1") for e in hb_info["events"]],))
            w("stripped from IN : yes (LF-free payload => LF can only be heartbeat)")
        else:
            w("result           : no heartbeat observed (device busy or firmware")
            w("                   does not emit HB)")
    w("")
    w("-- tool notes " + "-" * 64)
    if tool_notes:
        for n in tool_notes:
            w("  - %s" % n)
    else:
        w("  (none)")
    w("")
    w("=" * 78)
    w("VERDICT: %s" % verdict)
    w("=" * 78)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def parse_size(text):
    t = str(text).strip().lower().replace("_", "")
    m = re.fullmatch(r"(\d+(?:\.\d+)?)\s*([kmg]?)(?:i?b)?", t)
    if not m:
        raise argparse.ArgumentTypeError("bad size: %r" % text)
    val = float(m.group(1))
    mult = {"": 1, "k": 1024, "m": 1024 ** 2, "g": 1024 ** 3}[m.group(2)]
    out = int(val * mult)
    if out <= 0:
        raise argparse.ArgumentTypeError("size must be > 0")
    return out


def auto_int(text):
    """Accept 0x303A / 303A / 01 / 1 for id-like arguments.

    A leading 0x (or any a-f digit) means hex; otherwise decimal, so the
    natural `--mi 01` works.  USB ids are usually written in hex, so prefer
    the explicit 0x form in scripts.
    """
    t = str(text).strip().lower().replace("_", "")
    neg = t.startswith("-")
    if neg:
        t = t[1:]
    if t.startswith("0x"):
        val = int(t, 16)
    elif t.startswith("0b"):
        val = int(t, 2)
    elif re.search(r"[a-f]", t):
        val = int(t, 16)
    else:
        val = int(t, 10)
    return -val if neg else val


def build_parser():
    p = argparse.ArgumentParser(
        description="MeidaP4 WinUSB claim + bulk loopback validator",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--vid", type=auto_int, default=0x303A,
                   help="USB vendor id filter (default 0x303A)")
    p.add_argument("--pid", type=auto_int, default=None,
                   help="USB product id filter (mode A=0x4001, mode B=0x4002; "
                        "write 0x4002, a bare 4002 is decimal)")
    p.add_argument("--mi", type=auto_int, default=None,
                   help="interface number filter (mode A=0, mode B=1; "
                        "'01', '1' and '0x01' all work)")
    p.add_argument("--size", type=parse_size, action="append", default=None,
                   help="loopback size(s); repeatable or comma separated "
                        "(default 1k,64k,1m)")
    p.add_argument("--chunk", type=int, default=4096,
                   help="block size, clamped to %d (default 4096)" % MAX_CHUNK)
    p.add_argument("--timeout", type=int, default=3000,
                   help="per-operation timeout in ms (default 3000)")
    p.add_argument("--case-timeout", type=float, default=60.0,
                   help="whole-case timeout in seconds (default 60)")
    p.add_argument("--pipeline", type=int, default=1,
                   help="OUT writes in flight before draining (default 1; >1 "
                        "improves throughput, blurs RTT)")
    p.add_argument("--no-loopback", action="store_true",
                   help="enumerate + open + pipe inventory only, no bulk test")
    p.add_argument("--list", action="store_true",
                   help="only list candidates, do not open the device")
    p.add_argument("--diagnose", action="store_true",
                   help="read-only diagnosis: show which devnodes exist, which "
                        "device interfaces they register, and say exactly why "
                        "the interface cannot be opened")
    p.add_argument("--io-mode", choices=("overlapped", "sync"), default="overlapped",
                   help="pipe IO mode (default overlapped)")
    p.add_argument("--hb-wait", type=float, default=4.0,
                   help="seconds to wait for an idle heartbeat (default 4)")
    p.add_argument("--hb-only", action="store_true",
                   help="only run the heartbeat probe")
    p.add_argument("--quiet", type=float, default=0.4,
                   help="seconds of IN silence before each case (default 0.4)")
    p.add_argument("--hexdump", type=int, default=64,
                   help="on failure, print this many bytes of EXPECTED vs ACTUAL"
                        " hex (default 64; 0 disables)")
    p.add_argument("--open-retries", type=int, default=2,
                   help="CreateFile attempts on failure, 10 s apart (default 2)")
    p.add_argument("--guid", action="append", default=None,
                   help="extra/override device interface GUID to probe")
    p.add_argument("--device-path", default=None,
                   help="bypass enumeration and use this device path")
    p.add_argument("--report", default=None,
                   help="also write the plain-text report to this file")
    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    buf = []

    def log(msg=""):
        line = str(msg)
        buf.append(line)
        print(line, flush=True)

    sizes = args.size
    if sizes is None:
        sizes = [1024, 64 * 1024, 1024 * 1024]
    chunk = min(max(1, args.chunk), MAX_CHUNK)
    chunk_clamped = chunk != args.chunk
    tool_notes = []
    if chunk_clamped:
        tool_notes.append(
            "--chunk %d clamped to %d (device side buffers ~12 KB; serial or "
            "oversized blocking would deadlock)" % (args.chunk, MAX_CHUNK))

    log("MeidaP4 WinUSB host-side loopback tool")
    log("  GUID_DEVINTERFACE_WINUSB      : %s" % GUID_DEVINTERFACE_WINUSB)
    log("  GUID_DEVINTERFACE_USB_DEVICE  : %s (Chrome/Edge WebUSB)"
        % GUID_DEVINTERFACE_USB_DEVICE)

    # -- GUID discovery ----------------------------------------------------
    # Probe order: the canonical WinUSB interface GUID (hard-coded in
    # winusb.sys), then the USB device interface GUID that Chrome/Edge
    # enumerate, then the winusb.inf [ADB] GUID, then user-supplied ones.
    guids = [GUID_DEVINTERFACE_WINUSB, GUID_DEVINTERFACE_USB_DEVICE]
    inf_guid, inf_path = discover_winusb_guid()
    guid_src = inf_path
    if inf_guid and inf_guid.upper() not in [g.upper() for g in guids]:
        guids.append(inf_guid)
        tool_notes.append(
            "winusb.inf DeviceInterfaceGUIDs = %s belongs to its [ADB.HW.AddReg]"
            " section (ADB devices), not to generic WinUSB" % inf_guid)
    for g in (args.guid or []):
        if g.upper() not in [x.upper() for x in guids]:
            guids.append(g)
    log("  probing GUIDs: %s" % ", ".join(guids))

    if args.diagnose:
        return run_diagnose(args, guids, log)

    # -- enumerate ---------------------------------------------------------
    log("")
    log("enumerating device interfaces ...")
    try:
        entries = enumerate_candidates(guids, log)
    except Exception as exc:
        log("FATAL: enumeration raised %s: %s" % (type(exc).__name__, exc))
        return EXIT_TOOL
    for e in entries:
        e["vid"], e["pid"], e["mi"] = parse_ids(e)
    print_candidates(entries, log)

    chosen, sel_notes = select_candidate(
        entries, args.vid, args.pid, args.mi)
    tool_notes.extend(sel_notes)
    if args.device_path:
        chosen = {
            "path": args.device_path, "guid": "(override)", "hwids": [],
            "compatible": [], "friendly": None, "desc": None, "busdesc": None,
            "vid": None, "pid": None, "mi": None,
        }
        tool_notes.append("--device-path override in use; VID/PID/MI filters skipped")

    if args.list:
        # Purely informational: list what was found and stop.  Exits 0 even when
        # nothing matched, because "the device registered no interface" is a
        # finding, not a tool error.
        log("")
        if chosen is None:
            log("--list: no candidate matched vid=%s pid=%s mi=%s (see the"
                " candidate list above; run --diagnose for the reason)."
                % (hex(args.vid) if args.vid is not None else "any",
                   hex(args.pid) if args.pid is not None else "any",
                   ("%02X" % args.mi) if args.mi is not None else "any"))
        else:
            log("--list: would select %s" % chosen["path"])
        log("--list given: not opening the device.")
        return EXIT_PASS

    if chosen is None:
        log("")
        log("No candidate matched vid=%s pid=%s mi=%s."
            % (hex(args.vid) if args.vid is not None else "any",
               hex(args.pid) if args.pid is not None else "any",
               ("%02X" % args.mi) if args.mi is not None else "any"))
        log("Note: a device that registers NO device interface cannot appear")
        log("      here at all - there is no device path to match.")
        log("      Run with --diagnose to see exactly which devnodes exist and")
        log("      which device interfaces (if any) they register.")
        log("      Probe GUIDs used: %s" % ", ".join(guids))
        if not args.device_path:
            return EXIT_TOOL

    log("")
    log("selected: %s" % chosen["path"])

    # -- open --------------------------------------------------------------
    dev = WinUsbDevice(chosen["path"], overlapped=(args.io_mode == "overlapped"))
    last_err = None
    attempts = max(1, args.open_retries)
    for attempt in range(attempts):
        try:
            dev.open()
            last_err = None
            break
        except ToolError as exc:
            last_err = exc
            msg = str(exc)
            if "ACCESS_DENIED" in msg or "already claimed" in msg:
                break  # retrying a busy interface is pointless
            if attempt + 1 < attempts:
                log("  open attempt %d/%d failed: %s" % (attempt + 1, attempts, msg))
                log("  waiting 10 s before retrying (device may be re-enumerating) ...")
                time.sleep(10.0)
    if last_err is not None:
        log("")
        log("FATAL: %s" % last_err)
        log("")
        log("Diagnosis hints:")
        log("  * Service on the interface must be WINUSB (winusb.inf / WCID).")
        log("    Check: Get-PnpDevice -PresentOnly | ? InstanceId -like '*VID_303A*'")
        log("  * Device Manager -> the MI_0x interface should show no warning icon.")
        log("  * If a browser tab holds claimInterface, close it first.")
        return EXIT_TOOL

    case_results = []
    hb_info = None
    try:
        log("")
        log("opening OK; querying interface + pipes ...")
        iface = dev.query()
        print_pipes(dev, log)
        if dev.speed is not None:
            log("  negotiated speed: %s (%d)"
                % (SPEED_NAMES.get(dev.speed, "unknown"), dev.speed))

        # Claiming: WinUsb_Initialize already claims the interface exclusively.
        # WinUsb_ClaimInterface does not exist in winusb.dll on this system.
        try:
            if hasattr(winusb, "WinUsb_ClaimInterface"):
                ok = winusb.WinUsb_ClaimInterface(
                    dev.handle, iface["bInterfaceNumber"], None)
                log("  WinUsb_ClaimInterface(bInterfaceNumber=%d) -> %s"
                    % (iface["bInterfaceNumber"], bool(ok)))
            else:
                log("  WinUsb_ClaimInterface: not exported by winusb.dll ->")
                log("    claim is implicit on WinUsb_Initialize (exclusive).")
                tool_notes.append(
                    "WinUsb_ClaimInterface is absent from winusb.dll exports on "
                    "this host; claim is implicit via WinUsb_Initialize.")
        except Exception as exc:
            log("  WinUsb_ClaimInterface raised %s (treated as implicit claim)"
                % exc)

        if iface["current_alt"] not in (None, 0):
            log("  setting alternate setting 0 (was %s) ..." % iface["current_alt"])
            dev.set_alt_setting(0)

        sel = dev.find_pipes()
        if not sel["in"] or not sel["out"]:
            log("FATAL: could not find a bulk IN/OUT pipe pair on this interface")
            log("  pipes: %s" % dev.pipes)
            log("  hint: on the MTP interface (MI_00) the endpoints are "
                "interrupt/bulk; pick the vendor interface (MI_01).")
            return EXIT_TOOL
        log("  selected pipes: IN 0x%02X (maxpkt %d) / OUT 0x%02X (maxpkt %d)"
            % (sel["in"]["pipe_id"], sel["in"]["max_packet"],
               sel["out"]["pipe_id"], sel["out"]["max_packet"]))
        if sel["in"]["max_packet"] == 512:
            tool_notes.append("bulk wMaxPacketSize=512 => device is linked at High Speed")
        elif sel["in"]["max_packet"] == 64:
            tool_notes.append("bulk wMaxPacketSize=64 => device is linked at Full Speed")

        for p in (sel["in"], sel["out"]):
            dev.set_transfer_timeout(p["pipe_id"], args.timeout)

        stream = InStream()

        if not args.no_loopback:
            log("")
            log("heartbeat probe (idle %.1fs) ..." % args.hb_wait)
            hb_info = heartbeat_probe(dev, stream, sel["in"], args.hb_wait,
                                      args.timeout, sizes[0], chunk, log)
            if args.hb_only:
                log("")
                log("--hb-only given: skipping loopback cases.")
            else:
                log("")
                log("loopback cases (concurrent IN reader + OUT writer) ...")
                for size in sizes:
                    r = run_case(dev, stream, size, chunk, sel["in"], sel["out"],
                                 args.timeout, args.case_timeout, max(1, args.pipeline),
                                 args.quiet, log, hexdump=args.hexdump)
                    case_results.append(r)
                    if stream.dead:
                        tool_notes.append("IN stream ended early: %s" % stream.dead_reason)
                        break
                if hb_info is not None and not hb_info["ok"]:
                    hb_info2 = heartbeat_probe(dev, stream, sel["in"], args.hb_wait,
                                               args.timeout, sizes[0], chunk, log)
                    if hb_info2["ok"]:
                        hb_info = hb_info2
        else:
            log("")
            log("--no-loopback given: skipping loopback and heartbeat.")
    finally:
        dev.quit()

    # -- verdict -----------------------------------------------------------
    if args.no_loopback:
        verdict = "ENUMERATE+CLAIM OK (loopback skipped by --no-loopback)"
        code = EXIT_PASS
    elif args.hb_only:
        verdict = ("ENUMERATE+CLAIM OK, heartbeat %s"
                   % ("OK" if hb_info and hb_info["ok"] else "NOT OBSERVED"))
        code = EXIT_PASS
    else:
        failed = [r for r in case_results if not r.ok]
        if not case_results:
            verdict = "NO CASES RAN"
            code = EXIT_TOOL
        elif failed:
            verdict = ("FAIL: %d/%d case(s) failed (%s)"
                       % (len(failed), len(case_results),
                          ", ".join(human_bytes(r.size) for r in failed)))
            code = EXIT_FAIL
        else:
            total_w = sum(r.written for r in case_results)
            total_t = sum(r.elapsed for r in case_results) or 1e-9
            verdict = ("PASS: %d/%d cases byte-exact, %s echoed at %.2f Mbit/s"
                       % (len(case_results), len(case_results),
                          human_bytes(total_w), (total_w * 8.0 / 1e6) / total_t))
            if hb_info is not None and not hb_info["ok"]:
                tool_notes.append("loopback passed but no heartbeat line was "
                                  "observed within %.1fs idle" % args.hb_wait)
                verdict += " (heartbeat NOT observed)"
            code = EXIT_PASS

    try:
        print_report(buf, args, entries, chosen, dev, guids, guid_src,
                     case_results, hb_info, tool_notes, verdict, log)
    except Exception as exc:
        log("WARN: report rendering error: %s: %s" % (type(exc).__name__, exc))

    if args.report:
        try:
            with open(args.report, "w", encoding="utf-8") as fh:
                fh.write("\n".join(buf) + "\n")
            print("\nreport written to %s" % args.report)
        except OSError as exc:
            print("could not write report: %s" % exc, file=sys.stderr)

    return code


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
        sys.exit(EXIT_TOOL)
    except SystemExit:
        raise
    except Exception:
        import traceback
        print("\nINTERNAL ERROR (unexpected exception) - this is a tool bug:",
              file=sys.stderr)
        traceback.print_exc()
        sys.exit(EXIT_TOOL)
