#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""MeidaP4 · 无浏览器的 WebUSB 验收工具（Windows + WinUSB）

为什么需要它：Chrome 的 `requestDevice/claimInterface` 是人工操作、无法进 CI，也无法把
「EP3 到底有没有帧」变成可 grep 的证据。本工具用 WinUSB 直接打开复合设备的两个 vendor
接口（MI_01 = 视频 IN EP3，MI_02 = 控制 IN/OUT EP4），跑一遍 I0 线协议的最小主机侧：

  HELLO/HELLO_ACK 握手 → ACK 流控 → VIDEO 分片重组（存 .jpg）→ FILE 通道（列目录/读写/下载）

用法（用 ESP-IDF 自带 python，ctypes 用标准库即可）：
    C:\\Espressif\\tools\\python\\v6.1\\venv\\Scripts\\python.exe tools\\webusb_app.py --list
    ... tools\\webusb_app.py --video-seconds 8 --save-frames
    ... tools\\webusb_app.py --file-test
    ... tools\\webusb_app.py --video-seconds 5 --save-frames --file-test

判据：结尾打印 `RESULT PASS` / `RESULT FAIL`；关键行可 grep：
    VIDEO frames=... fps=... bytes=... dropped=...
    FILE list=... open=... write=... readback=... download=...

⚠️ 边界：本工具验证的是**设备侧协议与数据面**；它不能代替 Chrome 的
`requestDevice/claimInterface`（那是浏览器策略层，需要人工在 Chrome 里点一次）。
"""

from __future__ import annotations

import argparse
import collections
import ctypes
import os
import queue
import struct
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import winusb_loopback as wl  # noqa: E402  (同目录的 WinUSB plumbing，见其文件头)

# ---------------------------------------------------------------------------
# I0 协议常量（docs/protocol.md，与 components/webusb_proto/include/wu_codec.h 一致）
# ---------------------------------------------------------------------------

MAGIC0, MAGIC1, VERSION, HDR_LEN = 0x4D, 0x50, 0x01, 12
MAX_PAYLOAD = 1024

CH_CTRL, CH_VIDEO, CH_FILE, CH_LOG, CH_TELEM = 0, 1, 2, 3, 4
FLAG_MORE, FLAG_ACK, FLAG_RETX, FLAG_ABORT = 0x0001, 0x0002, 0x0004, 0x0008

OP_HELLO, OP_HELLO_ACK, OP_PING, OP_PONG, OP_ACK = 0x01, 0x02, 0x03, 0x04, 0x05
OP_STATS_REQ, OP_STATS_RSP = 0x40, 0x41

WU_VIDEO_HDR_LEN, WU_FILE_HDR_LEN, WU_CTRL_HDR_LEN, WU_TELEM_LEN = 16, 8, 4, 36

FILE_OPS = {
    0: "LIST_REQ", 1: "LIST_RSP", 2: "OPEN_REQ", 3: "OPEN_RSP", 4: "READ_REQ", 5: "READ_RSP",
    6: "WRITE_REQ", 7: "WRITE_RSP", 8: "CLOSE_REQ", 9: "CLOSE_RSP",
    10: "DELETE", 11: "MKDIR", 12: "RENAME",
}
STATUS = {0: "OK", 1: "EINVAL", 2: "ENOENT", 3: "EIO", 4: "ENOSPC", 5: "EBUSY", 6: "ENOTSUP"}
FILE_ENTRY_LEN = 4 + 1 + 256  # u32 size + u8 is_dir + char name[256]
FILE_MAX_ENTRIES_PER_MSG = 3  # 3 × 261 = 783 ≤ 1016


# ---------------------------------------------------------------------------
# 帧编解码
# ---------------------------------------------------------------------------

def _crc16_table():
    table = []
    for i in range(256):
        crc = i << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
        table.append(crc)
    return table


_CRC16_TABLE = _crc16_table()


def crc16(data: bytes) -> int:
    """CRC-16/CCITT-FALSE（查表版）。

    ⚠️ 必须查表：逐位实现是 O(8·n) 的 Python 循环，≈500 KB/s 就饱和，
    会把"设备能不能 30 fps"测成"Python 能不能算 CRC"。
    """
    crc = 0xFFFF
    tbl = _CRC16_TABLE
    for b in data:
        crc = ((crc << 8) & 0xFFFF) ^ tbl[((crc >> 8) ^ b) & 0xFF]
    return crc


def encode_frame(ch: int, flags: int, seq: int, payload: bytes) -> bytes:
    hdr = bytes([MAGIC0, MAGIC1, VERSION, ch]) + struct.pack("<HHH", flags, len(payload), seq)
    crc = crc16(hdr + payload)
    return hdr + struct.pack("<H", crc) + payload


class Decoder:
    """流式帧解码器（magic 重扫 + CRC 校验；与设备侧 wu_decoder 语义一致）"""

    def __init__(self):
        self.buf = bytearray()
        self.frames = 0
        self.crc_errors = 0
        self.resync_bytes = 0
        self.bad_header = 0

    def push(self, data: bytes):
        self.buf += data
        out = []
        while True:
            idx = self.buf.find(b"MP")
            if idx < 0:
                if self.buf:
                    # keep last byte in case 'M' is at the boundary
                    keep = 1 if self.buf.endswith(b"M") else 0
                    self.resync_bytes += len(self.buf) - keep
                    self.buf = self.buf[-keep:] if keep else bytearray()
                return out
            if idx > 0:
                self.resync_bytes += idx
                del self.buf[:idx]
            if len(self.buf) < HDR_LEN:
                return out
            if self.buf[2] != VERSION:
                self.bad_header += 1
                del self.buf[:1]
                continue
            length = struct.unpack_from("<H", self.buf, 6)[0]
            if length > MAX_PAYLOAD:
                self.bad_header += 1
                del self.buf[:1]
                continue
            total = HDR_LEN + length
            if len(self.buf) < total:
                return out
            body = bytes(self.buf[:total])
            want = struct.unpack_from("<H", body, 10)[0]
            if crc16(body[:10] + body[HDR_LEN:]) != want:
                self.crc_errors += 1
                del self.buf[:total]        # 丢整帧，从该帧之后继续（协议 §6.4）
                continue
            ch = body[3]
            # 帧头布局（协议 §2 / wu_codec.c wu_encode）：
            #   0 magic0 | 1 magic1 | 2 version | 3 channel
            #   4 flags(u16) | 6 length(u16) | 8 seq(u16) | 10 crc16
            # ⚠️ 真机踩过：把 seq 从偏移 6 读出来会等于 length，于是每一帧都被判成
            #    "乱序/重复"而不投递（表现是握手永远不成立、hello=0）。
            flags = struct.unpack_from("<H", body, 4)[0]
            seq = struct.unpack_from("<H", body, 8)[0]
            payload = body[HDR_LEN:]
            self.frames += 1
            out.append({"ch": ch, "flags": flags, "seq": seq, "payload": payload})
            del self.buf[:total]


# ---------------------------------------------------------------------------
# 小工具
# ---------------------------------------------------------------------------

class Log:
    def __init__(self, quiet=False):
        self.quiet = quiet
        self.lines = []

    def __call__(self, msg):
        self.lines.append(msg)
        if not self.quiet:
            print(msg, flush=True)


def jpeg_ok(data: bytes) -> bool:
    return len(data) > 4 and data[:2] == b"\xff\xd8" and data[-2:] == b"\xff\xd9"


def cstr(buf: bytes, off: int) -> str:
    end = buf.find(b"\x00", off)
    if end < 0:
        end = len(buf)
    return buf[off:end].decode("utf-8", "replace")


def path_bytes(p: str) -> bytes:
    return p.encode("utf-8") + b"\x00"


# ---------------------------------------------------------------------------
# 设备打开
# ---------------------------------------------------------------------------

def find_interface(entries, vid, pid, mi, log):
    """按 MI 找到对应的 WinUSB 设备接口候选；同时给出 '为什么没有' 的可读结论。"""
    best = None
    for e in entries:
        ev, ep, em = wl.parse_ids(e)
        if ev != vid or ep != pid:
            continue
        if em != mi:
            continue
        if e.get("service") != "WINUSB":
            continue
        best = e
        break
    if best is not None:
        return best
    # 诊断：是不是设备在，但这个 MI 没有 WinUSB？
    for e in entries:
        ev, ep, em = wl.parse_ids(e)
        if ev == vid and ep == pid and em == mi:
            log("  ! MI_%02X 存在但 service=%r（不是 WINUSB）—— 固件可能没有该接口的 WCID"
                % (mi, e.get("service")))
            return None
    log("  ! 没找到 MI_%02X（设备是否插好？固件是否已烧入 WCID 版本？）" % mi)
    return None


def open_winusb(entry, log, access=None):
    dev = wl.WinUsbDevice(entry["path"], overlapped=True)
    try:
        dev.open(access=wl.GENERIC_READ | wl.GENERIC_WRITE)
    except wl.ToolError as exc:
        log("  .. 读写打开失败（%s），改用只读打开（只有 IN 端点的接口常见）" % exc)
        dev.open(access=wl.GENERIC_READ)
    dev.query()
    # ⚠️ 必须给每个管道设 PIPE_TRANSFER_TIMEOUT + ALLOW_PARTIAL_READS：
    #    设备一次 IN 传输可能正好是 512 字节整数倍（MPS 的整包），没有 short packet 收尾；
    #    不设 ALLOW_PARTIAL_READS 时，请求 4KB 的读会一直等满 4KB → 表现为"设备完全没数据"。
    for p in dev.pipes:
        dev.set_transfer_timeout(p["pipe_id"], 200)
    return dev


# ---------------------------------------------------------------------------
# 链路（主机侧）
# ---------------------------------------------------------------------------

class Link:
    """主机侧 I0：握手、ACK、消息分片/重组、VIDEO 帧组装、FILE 请求/响应。"""

    def __init__(self, ctrl_dev, video_dev, log):
        self.ctrl = ctrl_dev           # MI_02：0x84 IN / 0x04 OUT
        self.video = video_dev         # MI_01：0x83 IN
        self.log = log

        self.in_pipe = None
        self.out_pipe = None
        self.video_in_pipe = None
        self._pipes()

        self.lock = threading.Lock()      # 保护 OUT 写
        self.tx_lock = threading.Lock()   # 取号 + 写 必须原子（保序！）
        self.state_lock = threading.Lock()
        self.tx_seq = {CH_CTRL: 0, CH_FILE: 0}
        self.rx_expect = {CH_CTRL: 0, CH_FILE: 0}

        self.dec_ctrl = Decoder()
        self.dec_video = Decoder()
        self.ctrl_msgs = queue.Queue()
        self.video_msgs = queue.Queue()
        self.logs = collections.deque(maxlen=200)
        self.telems = collections.deque(maxlen=20)
        self.file_pending = {}         # xid -> {"evt", "msg"}
        self.file_pending_lock = threading.Lock()
        self.next_xid = 1

        self.caps = 0
        self.link_up = False
        self.hello_seen = 0
        self.pongs = 0
        self.pings_sent = 0
        self.acks_sent = 0
        self.frames_from_dev = 0

        # VIDEO 组装
        self._vseq = None
        self._vchunks = []
        self._vmsg_parts = []
        self.video_frames = []          # list of dict(fmt,w,h,seq,data,ts)
        self.video_bytes = 0
        self.video_dropped = 0
        self._video_cb = None
        self.video_raw_bytes = 0
        self.raw_path = None
        self._raw_fh = None

        self.stop_evt = threading.Event()
        self.need_resync = False
        self.debug = False
        self.threads = []

    def _pipes(self):
        pipes = self.ctrl.pipes
        ins = [p for p in pipes if p["direction"] == "IN" and p["type"] == 2]
        outs = [p for p in pipes if p["direction"] == "OUT" and p["type"] == 2]
        if not ins or not outs:
            raise wl.ToolError("MI_02 上找不到 bulk IN/OUT（实际 pipes=%r）" % (pipes,))
        self.in_pipe = ins[0]["pipe_id"]
        self.out_pipe = outs[0]["pipe_id"]
        vins = [p for p in self.video.pipes if p["direction"] == "IN" and p["type"] == 2]
        if not vins:
            raise wl.ToolError("MI_01 上找不到 bulk IN（实际 pipes=%r）" % (self.video.pipes,))
        self.video_in_pipe = vins[0]["pipe_id"]

    # -- 线程 ------------------------------------------------------------
    def start(self):
        self._reader_init()
        t1 = threading.Thread(target=self._read_ctrl, name="ctrl-rx", daemon=True)
        t2 = threading.Thread(target=self._read_video, name="video-rx", daemon=True)
        t3 = threading.Thread(target=self._keepalive, name="keepalive", daemon=True)
        self.threads = [t1, t2, t3]
        for t in self.threads:
            t.start()

    def stop(self):
        self.stop_evt.set()

    # -- 快速读（复用同一个 OVERLAPPED/事件，避免每次 CreateEventW 的开销）--
    def _reader_init(self):
        self._ev = self.ctrl._new_overlapped()
        self._ev_video = self.video._new_overlapped()

    def _fast_read(self, dev, ov, pipe, length, timeout_ms):
        wl.k32.ResetEvent(ov.hEvent)
        buf = (ctypes.c_ubyte * length)()
        transferred = wl.wt.ULONG(0)
        rc = wl.winusb.WinUsb_ReadPipe(
            dev.handle, pipe, buf, length,
            ctypes.byref(transferred), ctypes.byref(ov))
        if not rc:
            err = wl._err()
            if err != wl.ERROR_IO_PENDING:
                if err in (wl.ERROR_SEM_TIMEOUT, wl.ERROR_OPERATION_ABORTED):
                    return b""
                raise wl.ToolError("WinUsb_ReadPipe(0x%02X) failed: %s"
                                   % (pipe, wl._winerr(err)))
        w = wl.k32.WaitForSingleObject(ov.hEvent, int(timeout_ms))
        if w == wl.WAIT_TIMEOUT:
            wl.k32.CancelIoEx(dev.file, ctypes.byref(ov))
            # ⚠️ 取消后必须看实际传输量：设备可能恰好在超时边缘完成了这一笔，
            #    直接返回 b"" 会把已经收进来的帧丢掉 → 主机不回 ACK → 设备重传 5 次
            #    → link_down → 视频 0 帧（这是真机上"偶发 0 帧"的根因之一）。
            ok2 = wl.k32.GetOverlappedResult(dev.file, ctypes.byref(ov),
                                             ctypes.byref(transferred), True)
            if ok2 and transferred.value > 0:
                return bytes(bytearray(buf[:transferred.value]))
            return b""
        if w != 0:  # WAIT_OBJECT_0
            raise wl.ToolError("WaitForSingleObject(read) rc=%d" % w)
        if not wl.k32.GetOverlappedResult(dev.file, ctypes.byref(ov),
                                          ctypes.byref(transferred), False):
            err = wl._err()
            if err in (wl.ERROR_OPERATION_ABORTED, wl.ERROR_SEM_TIMEOUT):
                return b""
            raise wl.ToolError("GetOverlappedResult(read) failed: %s" % wl._winerr(err))
        return bytes(bytearray(buf[:transferred.value]))

    def _read_ctrl(self):
        while not self.stop_evt.is_set():
            try:
                data = self._fast_read(self.ctrl, self._ev, self.in_pipe, 8192, 1000)
            except wl.ToolError as exc:
                self.log("  ! ctrl 读失败：%s" % exc)
                time.sleep(0.2)
                continue
            if not data:
                continue
            for f in self.dec_ctrl.push(data):
                self._on_frame(f, video=False)

    def _read_video(self):
        while not self.stop_evt.is_set():
            try:
                data = self.video.read(self.video_in_pipe, 8192, 100)
            except wl.ToolError as exc:
                self.log("  ! video 读失败：%s" % exc)
                time.sleep(0.2)
                continue
            if not data:
                continue
            self.video_raw_bytes += len(data)
            if self.raw_path:
                try:
                    self._raw_fh.write(data)
                    self._raw_fh.flush()
                except Exception:
                    pass
            for f in self.dec_video.push(data):
                self._on_frame(f, video=True)

    # -- 发送 ------------------------------------------------------------
    def _write(self, data):
        with self.lock:
            ok, n = self.ctrl.write(self.out_pipe, data, 1000)
        good = ok and n == len(data)
        if not good:
            # ⚠️ 主机侧 TX 丢了一帧会让"我方 TX seq vs 设备 RX 期望"永久错位
            #（协议没有 NACK/重同步），表现为设备一直重传直到 link_down。
            # 这里标记需要一次 §6.3 重同步（HELLO_ACK + 两侧归零），由 keepalive 执行。
            self.need_resync = True
        return good

    def _send_msg(self, ch, payload, want_ack=True, timeout_ms=2000):
        """按 WU_MAX_PAYLOAD 分片发送一条消息。

        ⚠️ 真机踩过的坑：ACK 由接收线程发、PING/请求由 keepalive 线程发，两个线程都从
        `tx_seq` 取号 —— 若"取号"与"真正写到 USB"不在同一把锁里，就会**先发序号大的帧、
        后发序号小的帧**。设备的 seq 门是严格的，小序号晚到会被判 gap 丢掉，
        **从此我方 TX seq 与设备 rx_expect 永久错位**（协议没有 NACK/重同步），
        表现为设备一直重传直到 link_down。所以取号 + 编码 + 写 必须在 `tx_lock` 内完成。
        """
        off = 0
        total = max(1, (len(payload) + MAX_PAYLOAD - 1) // MAX_PAYLOAD)
        for i in range(total):
            chunk = payload[off:off + MAX_PAYLOAD]
            off += len(chunk)
            flags = (FLAG_ACK if want_ack else 0) | (FLAG_MORE if i + 1 < total else 0)
            with self.tx_lock:
                with self.state_lock:
                    seq = self.tx_seq[ch]
                    self.tx_seq[ch] = (seq + 1) & 0xFFFF
                frame = encode_frame(ch, flags, seq, chunk)
                deadline = time.time() + timeout_ms / 1000.0
                while True:
                    if self._write(frame):
                        break
                    if time.time() > deadline:
                        return False
                    time.sleep(0.002)
            if total > 1:
                time.sleep(0.001)
        return True

    def send_ctrl(self, op, status=0, arg=0, body=b""):
        payload = bytes([op, status]) + struct.pack("<H", arg) + body
        return self._send_msg(CH_CTRL, payload, want_ack=True)

    def send_bootstrap(self, op, arg=0, body=b""):
        """引导帧：seq=0、flags=0（§6.3 ④）"""
        payload = bytes([op, 0]) + struct.pack("<H", arg) + body
        frame = encode_frame(CH_CTRL, 0, 0, payload)
        return self._write(frame)

    # -- 收 --------------------------------------------------------------
    def _send_ack(self, ch, next_expected):
        # ⚠️ 2026-10-31 协议修订（§6.5②）：ACK 帧不消耗通道 seq（seq 固定 0），
        # 接收方在 seq 门之前按"长度 4 + 首字节 0x05"识别并消费。
        payload = bytes([OP_ACK, 0]) + struct.pack("<H", next_expected)
        with self.tx_lock:
            seq = 0
            frame = encode_frame(ch, 0, seq, payload)
            if self.debug:
                self.log("    [tx ack] ch=%d seq=%u arg=%u" % (ch, seq, next_expected))
            for _ in range(8):
                if self._write(frame):
                    break
                time.sleep(0.001)
        self.acks_sent += 1

    def _on_frame(self, f, video=False):
        self.frames_from_dev += 1
        ch = f["ch"]

        if ch in (CH_CTRL, CH_FILE):
            # ACK 帧不参与 seq 期望值（2026-10-31 协议修订）：先消费，不推进 expect
            if len(f["payload"]) == WU_CTRL_HDR_LEN and f["payload"][0] == OP_ACK:
                return
            with self.state_lock:
                expect = self.rx_expect[ch]
                if f["seq"] != expect:
                    # 重复/乱序：只回累计 ACK，不投递
                    if f["flags"] & FLAG_ACK:
                        need_ack = expect
                    else:
                        need_ack = None
                    upcoming = expect
                else:
                    self.rx_expect[ch] = (expect + 1) & 0xFFFF
                    need_ack = self.rx_expect[ch] if (f["flags"] & FLAG_ACK) else None
                    upcoming = expect + 1
            if f["seq"] != expect:
                if self.debug:
                    self.log("    [rx drop] ch=%d seq=%u expect=%u flags=0x%04X" % (ch, f["seq"], expect, f["flags"]))
                if need_ack is not None:
                    self._send_ack(ch, need_ack)
                return
            if self.debug:
                self.log("    [rx ok] ch=%d seq=%u flags=0x%04X len=%d" % (ch, f["seq"], f["flags"], len(f["payload"])))
            if need_ack is not None:
                self._send_ack(ch, need_ack)

        payload = f["payload"]
        if ch == CH_CTRL:
            self._on_ctrl(payload)
        elif ch == CH_FILE:
            self._on_file(payload)
        elif ch == CH_VIDEO:
            self._on_video_msg(payload)
        elif ch == CH_LOG:
            self.logs.append(payload.rstrip(b"\x00").decode("utf-8", "replace"))
        elif ch == CH_TELEM and len(payload) >= WU_TELEM_LEN:
            self.telems.append(payload[:WU_TELEM_LEN])

    def _on_ctrl(self, payload):
        if len(payload) < WU_CTRL_HDR_LEN:
            return
        op = payload[0]
        status = payload[1]
        arg = struct.unpack_from("<H", payload, 2)[0]
        body = payload[WU_CTRL_HDR_LEN:]
        if op != OP_HELLO and not self.link_up:
            # 收到任何非 HELLO 的 CTRL 帧 ⇒ 设备已经 link_up（TELEM/LOG/PONG/ACK 都会出现）
            self.link_up = True
        if op == OP_HELLO:
            self.hello_seen += 1
            if len(body) >= 4:
                self.caps = struct.unpack_from("<I", body, 0)[0]
            # HELLO 表示一轮新的 link：本侧序号归零（§6.3）
            with self.state_lock:
                self.tx_seq[CH_CTRL] = 0
                self.tx_seq[CH_FILE] = 0
                self.rx_expect[CH_CTRL] = 0
                self.rx_expect[CH_FILE] = 0
            self.link_up = False
            self.send_bootstrap(OP_HELLO_ACK, VERSION, b"")
        elif op == OP_PING:
            # PING 是普通可靠帧（不是引导帧）：回复 PONG 即可；本工具不额外计数
            self.send_ctrl(OP_PONG, arg=0, body=b"")
        elif op == OP_PONG:
            self.pongs += 1
        elif op == OP_ACK:
            pass
        elif op == OP_STATS_RSP and len(body) >= WU_TELEM_LEN:
            self.telems.append(body[:WU_TELEM_LEN])

    def _on_file(self, payload):
        if len(payload) < WU_FILE_HDR_LEN:
            return
        op = payload[0]
        status = payload[1]
        xid = struct.unpack_from("<H", payload, 2)[0]
        offset = struct.unpack_from("<I", payload, 4)[0]
        body = payload[WU_FILE_HDR_LEN:]
        with self.file_pending_lock:
            entry = self.file_pending.pop(xid, None)
        if entry is not None:
            entry["msg"] = {"op": op, "status": status, "xid": xid, "offset": offset,
                            "body": body}
            entry["evt"].set()
        else:
            self.log("  .. 未匹配 xid=%u 的 FILE 响应（op=%s）" % (xid, FILE_OPS.get(op, op)))

    def _on_video_msg(self, payload):
        if len(payload) < WU_VIDEO_HDR_LEN:
            return
        fmt = payload[0]
        keyframe = payload[1]
        w, h = struct.unpack_from("<HH", payload, 2)
        fps_num, fps_den = struct.unpack_from("<HH", payload, 6)
        frame_seq = struct.unpack_from("<I", payload, 10)[0]
        chunk_index = struct.unpack_from("<H", payload, 14)[0]
        self.video_bytes += len(payload) - WU_VIDEO_HDR_LEN

        if self._vseq is None or frame_seq != self._vseq:
            if self._vseq is not None:
                self.video_dropped += 1     # 上一帧没收完
            self._vseq = frame_seq
            self._vchunks = []
        if chunk_index != len(self._vchunks):
            # 分片缺口：整帧作废
            self.video_dropped += 1
            self._vseq = None
            self._vchunks = []
            return
        self._vchunks.append(payload[WU_VIDEO_HDR_LEN:])

        # 最后一片 = MORE 未置位的那条消息（由帧头 flags 判断）
        # 说明：flags 在 _on_frame 里已经处理；这里用"下一条消息的 frame_seq/chunk_index"
        # 判断是否收尾不可靠，所以由调用方在 _on_frame 中把 MORE 传进来更准确。
        # 为简化，这里依靠 payload 内 chunk_index 连续 + 外部调用 drain 时判断。
        self._last_more = None

    # -- VIDEO 收尾（需要 flags，故在 _on_frame 里补） --------------------
    def deliver_video_frames(self):
        """把已完成的 VIDEO 消息组装成帧（由 _on_frame 在 MORE=0 时调用）。"""
        if not self._vchunks:
            return
        data = b"".join(self._vchunks)
        self.video_frames.append({"fmt": 0, "w": 0, "h": 0, "seq": self._vseq, "data": data,
                                  "ts": time.time()})
        self._vchunks = []
        self._vseq = None

    def resync(self, why=""):
        """强制一次 §6.3 握手：HELLO_ACK + 本侧 TX/RX 归零（设备 link_up 时也会归零）。"""
        with self.state_lock:
            self.tx_seq[CH_CTRL] = 0
            self.tx_seq[CH_FILE] = 0
            self.rx_expect[CH_CTRL] = 0
            self.rx_expect[CH_FILE] = 0
        self.link_up = False
        self.send_bootstrap(OP_HELLO_ACK, VERSION, b"")
        if why:
            self.log("  .. 重同步：%s" % why)

    # -- 握手 ------------------------------------------------------------
    def handshake(self, timeout_s=10.0):
        """发 HELLO_ACK 并等设备 link_up。

        ⚠️ 关键（真机踩过）：HELLO_ACK 会让设备进入 link_up 并**把自己所有 TX seq 归 0**。
        如果主机像心跳一样反复发 HELLO_ACK，设备就会被反复重置：它发 seq=0,1,2…，
        而主机的 rx_expect 已经涨上去 ⇒ 每一帧都被判成"重复/乱序"⇒ 永远看不到 TELEM/LOG
        ⇒ 握手永远不成立（本次实测 gen 飙到 17）。所以这里**只在需要时发一次**，
        并且发完之后把本侧 rx_expect 也归 0（与设备 §6.3 的归零动作对齐）。
        """
        self.log("  .. 发 HELLO_ACK 等设备 link_up（设备每 2s 重发 HELLO 直到收到）")
        deadline = time.time() + timeout_s
        last_ack = 0.0
        while time.time() < deadline:
            now = time.time()
            if self._linkup_evidence():
                self.link_up = True
                self.log("  .. link_up：caps=0x%08X（hello=%d frames=%d）"
                         % (self.caps, self.hello_seen, self.frames_from_dev))
                return True
            if now - last_ack > 2.5:
                # 一次性引导动作：发 HELLO_ACK + 两侧序号归零（设备 link_up 会做同样的事）
                with self.state_lock:
                    self.tx_seq[CH_CTRL] = 0
                    self.tx_seq[CH_FILE] = 0
                    self.rx_expect[CH_CTRL] = 0
                    self.rx_expect[CH_FILE] = 0
                self.send_bootstrap(OP_HELLO_ACK, VERSION, b"")
                last_ack = now
            time.sleep(0.02)
        self.log("  !! 握手超时：hello=%d frames_rx=%d crc_err=%d bad_hdr=%d"
                 % (self.hello_seen, self.dec_ctrl.frames, self.dec_ctrl.crc_errors,
                    self.dec_ctrl.bad_header))
        return False

    def _linkup_evidence(self):
        # 设备 link_up 后会立刻发 TELEM/LOG/PING；HELLO 本身不算
        return bool(self.telems) or bool(self.logs) or self.pongs > 0

    def _keepalive(self):
        """PING 保活 + 周期性累计 ACK。

        ⚠️ 真机教训：单发一次 ACK 可能丢（WinUSB 取消/调度），设备重传 5 次就 link_down，
        链路 1~2 s 就翻一次 ⇒ 视频只能发出零星几帧。这里每 300 ms 补一条累计 ACK
        （arg = rx_expect，语义幂等，重复 ACK 只会把同一个窗口再释放一次）。
        """
        last_ping = 0.0
        next_ack = 0.0
        while not self.stop_evt.is_set():
            time.sleep(0.05)
            if self.stop_evt.is_set():
                return
            now = time.time()
            if self.need_resync:
                self.need_resync = False
                self.resync("主机 TX 写失败 → 主动重同步")
                next_ack = now
                last_ping = now
                continue
            try:
                if self.link_up and now - next_ack >= 0.3:
                    next_ack = now
                    with self.state_lock:
                        e_ctrl = self.rx_expect[CH_CTRL]
                        e_file = self.rx_expect[CH_FILE]
                    self._send_ack(CH_CTRL, e_ctrl)
                    if e_file or self.file_pending:
                        self._send_ack(CH_FILE, e_file)
                if self.link_up and now - last_ping >= 1.0:
                    last_ping = now
                    self.pings_sent += 1
                    self.send_ctrl(OP_PING, arg=0, body=b"")
                if not self.link_up and now - last_ping >= 1.0:
                    # 设备在等 HELLO_ACK（它每 2 s 重发 HELLO）；补一发并两侧归零
                    last_ping = now
                    with self.state_lock:
                        self.tx_seq[CH_CTRL] = 0
                        self.tx_seq[CH_FILE] = 0
                        self.rx_expect[CH_CTRL] = 0
                        self.rx_expect[CH_FILE] = 0
                    self.send_bootstrap(OP_HELLO_ACK, VERSION, b"")
            except Exception:
                pass

    # -- FILE 客户端 -----------------------------------------------------
    def file_request(self, op, status=0, offset=0, body=b"", timeout_s=3.0):
        with self.file_pending_lock:
            xid = self.next_xid
            self.next_xid = (self.next_xid % 0xFFFF) + 1
            entry = {"evt": threading.Event(), "msg": None}
            self.file_pending[xid] = entry
        payload = bytes([op, status]) + struct.pack("<HI", xid, offset) + body
        if not self._send_msg(CH_FILE, payload, want_ack=True):
            with self.file_pending_lock:
                self.file_pending.pop(xid, None)
            raise wl.ToolError("FILE 请求发不出去（OUT 端点忙）")
        entry["evt"].wait(timeout_s)
        with self.file_pending_lock:
            self.file_pending.pop(xid, None)
        if entry["msg"] is None:
            raise wl.ToolError("FILE %s xid=%u 超时 %.1fs（设备侧 FILE 服务是否已实现？）"
                               % (FILE_OPS.get(op, op), xid, timeout_s))
        return entry["msg"]

    def file_list(self, path="/", timeout_s=3.0):
        entries = []
        start = 0
        for _ in range(64):
            msg = self.file_request(0, offset=start, body=path_bytes(path), timeout_s=timeout_s)
            if msg["op"] != 1:
                raise wl.ToolError("LIST 期望 LIST_RSP(1)，收到 op=%s" % FILE_OPS.get(msg["op"]))
            if msg["status"] != 0:
                raise wl.ToolError("LIST %s 失败：%s" % (path, STATUS.get(msg["status"])))
            body = msg["body"]
            n = len(body) // FILE_ENTRY_LEN
            for i in range(n):
                off = i * FILE_ENTRY_LEN
                size = struct.unpack_from("<I", body, off)[0]
                is_dir = body[off + 4] != 0
                name = cstr(body, off + 5)
                entries.append({"name": name, "size": size, "is_dir": is_dir})
            if n == 0 or n < FILE_MAX_ENTRIES_PER_MSG:
                break
            start += n
        return entries

    def file_open(self, path, mode=0, timeout_s=3.0):
        body = bytes([mode & 0xFF]) + path_bytes(path)
        msg = self.file_request(2, body=body, timeout_s=timeout_s)
        if msg["op"] != 3 or msg["status"] != 0:
            raise wl.ToolError("OPEN %s 失败：op=%s status=%s"
                               % (path, FILE_OPS.get(msg["op"]), STATUS.get(msg["status"])))
        b = msg["body"]
        if len(b) < 9:
            raise wl.ToolError("OPEN_RSP body=%d B < 9" % len(b))
        handle, size = struct.unpack_from("<II", b, 0)
        return {"handle": handle, "size": size, "is_dir": b[8] != 0}

    def file_read(self, handle, offset, length, timeout_s=3.0):
        msg = self.file_request(4, offset=offset,
                                body=struct.pack("<II", handle, length), timeout_s=timeout_s)
        if msg["op"] != 5 or msg["status"] != 0:
            raise wl.ToolError("READ 失败：op=%s status=%s"
                               % (FILE_OPS.get(msg["op"]), STATUS.get(msg["status"])))
        return msg["body"]

    def file_write(self, handle, offset, data, timeout_s=3.0):
        msg = self.file_request(6, offset=offset,
                                body=struct.pack("<I", handle) + data, timeout_s=timeout_s)
        if msg["op"] != 7 or msg["status"] != 0:
            raise wl.ToolError("WRITE 失败：op=%s status=%s"
                               % (FILE_OPS.get(msg["op"]), STATUS.get(msg["status"])))
        b = msg["body"]
        return struct.unpack_from("<I", b, 0)[0] if len(b) >= 4 else 0

    def file_close(self, handle, timeout_s=3.0):
        msg = self.file_request(8, body=struct.pack("<I", handle), timeout_s=timeout_s)
        if msg["op"] != 9:
            raise wl.ToolError("CLOSE 期望 CLOSE_RSP(9)，收到 op=%s" % FILE_OPS.get(msg["op"]))
        return msg["status"] == 0

    def file_mkdir(self, path, timeout_s=3.0):
        msg = self.file_request(11, body=path_bytes(path), timeout_s=timeout_s)
        return msg["status"]

    # -- 视频采集 --------------------------------------------------------
    def collect_video(self, seconds):
        """在 seconds 内收集完整 VIDEO 帧（靠 MORE=0 收尾）。"""
        self.video_frames = []
        deadline = time.time() + seconds
        got = 0
        while time.time() < deadline:
            time.sleep(0.02)
        # 收尾：把收到一半的帧丢弃
        self._vseq = None
        self._vchunks = []
        self._vmsg_parts = []
        got = len(self.video_frames)
        return got


# ---------------------------------------------------------------------------
# VIDEO 收尾钩子：_on_frame 里需要 MORE 标志，Link._on_video_msg 只存分片。
# 为保持逻辑集中，这里给 Link 打一个补丁式方法。
# ---------------------------------------------------------------------------

_orig_on_frame = Link._on_frame


def _on_frame_with_video(self, f, video=False):
    # 先按原逻辑处理（含 ACK/其它通道）
    ch = f["ch"]
    if ch == CH_VIDEO:
        # 完全在这里处理 VIDEO，避免 _on_video_msg 里丢 MORE 信息。
        #
        # ⚠️ 协议 §5.2：**一条消息 = 一整个编码帧**；16 B 视频头只在消息开头出现一次；
        # 消息 > 1024 B 时按普通协议分片（MORE=1 表示"同一条消息还有分片"）。
        # 因此这里先做**消息级**拼接：MORE=1 的分片先攒着，MORE=0 时才是一条完整消息。
        # （2026-10-31：旧固件给每个协议帧都塞了 16 B 头，本工具按旧语义逐帧解析，
        #   反而掩盖了网页侧"每隔 1008 B 多出 16 B"的花屏 bug。）
        self.frames_from_dev += 1
        payload = f["payload"]
        self._vmsg_parts.append(payload)
        if f["flags"] & FLAG_MORE:
            return
        msg = b"".join(self._vmsg_parts)
        self._vmsg_parts = []
        if len(msg) < WU_VIDEO_HDR_LEN:
            return
        fmt = msg[0]
        w, h = struct.unpack_from("<HH", msg, 2)
        frame_seq = struct.unpack_from("<I", msg, 10)[0]
        data = msg[WU_VIDEO_HDR_LEN:]
        self.video_bytes += len(data)
        if not data:
            return
        self.video_frames.append({"fmt": fmt, "w": w, "h": h, "seq": frame_seq,
                                  "data": data, "ts": time.time()})
        return
    _orig_on_frame(self, f, video=video)


Link._on_frame = _on_frame_with_video


# ---------------------------------------------------------------------------
# 测试用例
# ---------------------------------------------------------------------------

def do_video(link, args, log):
    log("-- VIDEO " + "-" * 60)
    t0 = time.time()
    link.video_frames = []
    while time.time() - t0 < args.video_seconds:
        time.sleep(0.05)
    frames = list(link.video_frames)
    dt = max(1e-3, time.time() - t0)
    fps = len(frames) / dt
    total_bytes = sum(len(f["data"]) for f in frames)
    seqs = [f["seq"] for f in frames]
    gaps = 0
    for a, b in zip(seqs, seqs[1:]):
        if (b - a) & 0xFFFFFFFF != 1:
            gaps += 1
    good = sum(1 for f in frames if jpeg_ok(f["data"]))
    log("VIDEO link: acks=%d pings=%d hello=%d dev_frames=%d"
        % (link.acks_sent, link.pings_sent, link.hello_seen, link.frames_from_dev))
    log("VIDEO raw=%d dec_frames=%d crc=%d bad=%d"
        % (link.video_raw_bytes, link.dec_video.frames,
           link.dec_video.crc_errors, link.dec_video.bad_header))
    log("VIDEO frames=%d fps=%.2f bytes=%d avg=%d jpeg_ok=%d gaps=%d dropped=%d"
        % (len(frames), fps, total_bytes,
           (total_bytes // len(frames)) if frames else 0, good, gaps, link.video_dropped))
    if frames:
        f = frames[-1]
        log("VIDEO last: %dx%d fmt=%d seq=%u bytes=%d" % (f["w"], f["h"], f["fmt"],
                                                         f["seq"], len(f["data"])))
    if args.save_frames and frames:
        os.makedirs(args.out, exist_ok=True)
        saved = 0
        for i, f in enumerate(frames):
            if not jpeg_ok(f["data"]):
                continue
            fn = os.path.join(args.out, "frame_%04d.jpg" % i)
            with open(fn, "wb") as fh:
                fh.write(f["data"])
            saved += 1
        log("VIDEO 已保存 %d 帧到 %s" % (saved, os.path.abspath(args.out)))
    ok = len(frames) > 0 and good == len(frames)
    return ok, ("%d 帧 / %d 帧 JPEG 合法" % (len(frames), good))


def do_file(link, args, log):
    log("-- FILE " + "-" * 61)
    ok_all = True
    try:
        entries = link.file_list("/")
        names = [e["name"] for e in entries]
        log("FILE list=ok entries=%d first=%r" % (len(entries), names[:6]))
        if not entries:
            log("  ! 根目录为空（SD 卡没挂载？）")
            ok_all = False
    except wl.ToolError as exc:
        log("FILE list=FAIL %s" % exc)
        ok_all = False
        return False, "LIST 失败"

    # 写入 /PC2P4.TXT 并读回
    try:
        payload = ("PC->P4 share probe %s\n"
                   % time.strftime("%Y-%m-%d %H:%M:%S")).encode()
        info = link.file_open("/PC2P4.TXT", mode=1)
        wrote = link.file_write(info["handle"], 0, payload)
        link.file_close(info["handle"])
        log("FILE write=ok handle=%u written=%u size=%u" % (info["handle"], wrote, len(payload)))
        if wrote != len(payload):
            ok_all = False

        info = link.file_open("/PC2P4.TXT", mode=0)
        back = link.file_read(info["handle"], 0, len(payload))
        link.file_close(info["handle"])
        same = back == payload
        log("FILE readback=%s bytes=%d sourcelen=%d" % ("ok" if same else "MISMATCH",
                                                       len(back), len(payload)))
        if not same:
            ok_all = False
            log("  ! 读回内容：%r" % back[:120])
    except wl.ToolError as exc:
        log("FILE write/readback=FAIL %s" % exc)
        ok_all = False

    # 下载开机写入的 SD info 文件（证明 P4 侧文件对主机可见）
    try:
        info = link.file_open("/MeidaP4-SD-INFO.txt", mode=0)
        data = link.file_read(info["handle"], 0, min(256, max(1, info["size"])))
        link.file_close(info["handle"])
        log("FILE download=ok /MeidaP4-SD-INFO.txt size=%u head=%r"
            % (info["size"], data[:80]))
    except wl.ToolError as exc:
        log("FILE download=FAIL %s" % exc)
        ok_all = False

    # 目录操作（失败不影响 PASS，只作信息）
    st = link.file_mkdir("/P4-TESTDIR")
    log("FILE mkdir=/P4-TESTDIR status=%s" % STATUS.get(st, st))
    return ok_all, ("LIST/写/读回/下载")


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(
        description="MeidaP4 WebUSB 验收工具（WinUSB 直连，无需浏览器）",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--vid", type=wl.auto_int, default=0x303A)
    ap.add_argument("--pid", type=wl.auto_int, default=0x4103)
    ap.add_argument("--list", action="store_true", help="只枚举接口，不打开")
    ap.add_argument("--video-seconds", type=float, default=0.0,
                    help="采集视频的秒数（0 = 不测）")
    ap.add_argument("--save-frames", action="store_true", help="把合法 JPEG 帧存到 --out")
    ap.add_argument("--save-raw", metavar="PATH", default=None,
                    help="把 EP3 收到的原始字节流原样追加到文件（用于离线复现网页侧重组）")
    ap.add_argument("--file-test", action="store_true", help="跑 FILE 通道用例")
    ap.add_argument("--get", metavar="PATH", default=None,
                    help="从设备读一个文件并打印（例如 --get /PC-MTP-TEST.txt）")
    ap.add_argument("--put", nargs=2, metavar=("LOCAL", "PATH"), default=None,
                    help="把本地文件写到设备（例如 --put web/app.html /meidap4.html）")
    ap.add_argument("--delete", metavar="PATH", default=None, help="删除设备上的文件")
    ap.add_argument("--out", default="out", help="视频帧输出目录（默认 out/）")
    ap.add_argument("--timeout", type=float, default=3.0)
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--debug-link", action="store_true", help="打印 CTRL/ACK 细节（诊断序号错位）")
    args = ap.parse_args(argv)

    log = Log(quiet=args.quiet)

    log("== 枚举 WinUSB 设备接口 ==")
    entries = wl.enumerate_candidates(
        [wl.GUID_DEVINTERFACE_WINUSB, wl.GUID_DEVINTERFACE_USB_DEVICE], log)
    mine = []
    for e in entries:
        vid, pid, mi = wl.parse_ids(e)
        if vid == args.vid and pid == args.pid:
            mine.append((mi, e))
    mine.sort(key=lambda x: (x[0] is None, x[0]))
    if not mine:
        log("RESULT FAIL: 没找到 VID_%04X&PID_%04X（设备未插/未烧 WCID 固件）"
            % (args.vid, args.pid))
        return 2
    log("== 目标设备接口 ==")
    for mi, e in mine:
        log("  MI_%-2s service=%-10s path=%s"
            % ("%02X" % mi if mi is not None else "??", e.get("service"), e.get("path")))
    if args.list:
        log("RESULT PASS: 枚举到 %d 个接口" % len(mine))
        return 0

    e_ctl = find_interface(entries, args.vid, args.pid, 2, log)
    e_vid = find_interface(entries, args.vid, args.pid, 1, log)
    if e_ctl is None:
        log("RESULT FAIL: MI_02（控制接口）不可用")
        return 2
    if e_vid is None:
        log("RESULT FAIL: MI_01（视频接口）不可用 —— 固件需要带 IF1 WCID（bcdDevice 0x0110+）")
        return 2

    dev_ctl = open_winusb(e_ctl, log)
    dev_vid = open_winusb(e_vid, log)
    log("  MI_02 iface=%r" % (dev_ctl.iface,))
    log("  MI_01 iface=%r" % (dev_vid.iface,))
    log("  MI_02 pipes=%r" % [(p["pipe_id"], p["direction"], p["type"], p["max_packet"])
                            for p in dev_ctl.pipes])
    log("  MI_01 pipes=%r" % [(p["pipe_id"], p["direction"], p["type"], p["max_packet"])
                            for p in dev_vid.pipes])

    link = Link(dev_ctl, dev_vid, log)
    link.debug = args.debug_link
    if args.save_raw:
        link.raw_path = args.save_raw
        link._raw_fh = open(args.save_raw, "wb")
    link.start()
    try:
        if not link.handshake(timeout_s=10.0):
            log("RESULT FAIL: 10s 内没有完成 HELLO/HELLO_ACK 握手（见上：caps/帧统计）")
            log("  dev_frames=%d hello=%d crc_err=%d bad_hdr=%d"
                % (link.frames_from_dev, link.hello_seen,
                   link.dec_ctrl.crc_errors, link.dec_ctrl.bad_header))
            return 1
        time.sleep(0.3)
        results = []
        if args.put:
            local, remote = args.put
            try:
                with open(local, "rb") as fh:
                    blob = fh.read()
                info = link.file_open(remote, mode=1)
                written = 0
                while written < len(blob):
                    chunk = blob[written:written + 1012]
                    n = link.file_write(info["handle"], written, chunk,
                                         timeout_s=max(6.0, args.timeout))
                    if n <= 0:
                        raise wl.ToolError("WRITE 在 offset=%d 写入 0 字节" % written)
                    written += n
                link.file_close(info["handle"])
                log("PUT %s -> %s bytes=%d" % (local, remote, written))
                results.append(("PUT", written == len(blob), "%s (%d B)" % (remote, written)))
            except (OSError, wl.ToolError) as exc:
                log("PUT %s FAIL: %s" % (remote, exc))
                results.append(("PUT", False, str(exc)))
        if args.delete:
            try:
                link.file_request(10, body=(args.delete.encode("utf-8") + b"\x00"))
                log("DELETE %s ok" % args.delete)
                results.append(("DELETE", True, args.delete))
            except wl.ToolError as exc:
                log("DELETE %s FAIL: %s" % (args.delete, exc))
                results.append(("DELETE", False, str(exc)))
        if args.get:
            try:
                info = link.file_open(args.get, mode=0)
                data = link.file_read(info["handle"], 0, min(info["size"], 4096))
                link.file_close(info["handle"])
                log("GET %s size=%u data=%r" % (args.get, info["size"], data[:200]))
                results.append(("GET", info["size"] > 0, "%s (%u B)" % (args.get, info["size"])))
            except wl.ToolError as exc:
                log("GET %s FAIL: %s" % (args.get, exc))
                results.append(("GET", False, str(exc)))
        if args.video_seconds > 0:
            ok, detail = do_video(link, args, log)
            results.append(("VIDEO", ok, detail))
        if args.file_test:
            ok, detail = do_file(link, args, log)
            results.append(("FILE", ok, detail))
        if not results:
            log("RESULT PASS: 握手成功（未指定用例；加 --video-seconds / --file-test）")
            return 0
        all_ok = all(r[1] for r in results)
        for name, ok, detail in results:
            log("%s %s : %s" % (name, "PASS" if ok else "FAIL", detail))
        log("RESULT %s" % ("PASS" if all_ok else "FAIL"))
        return 0 if all_ok else 1
    finally:
        link.stop()
        time.sleep(0.1)
        if getattr(link, "_raw_fh", None):
            try:
                link._raw_fh.close()
            except Exception:
                pass
        dev_ctl.quit()
        dev_vid.quit()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except wl.ToolError as exc:
        print("TOOL ERROR: %s" % exc, file=sys.stderr)
        sys.exit(2)
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        sys.exit(2)
