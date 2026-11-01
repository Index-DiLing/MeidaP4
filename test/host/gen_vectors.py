#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""MeidaP4 · I0 线协议黄金向量生成器（verifier 独立实现）

规约依据**只有** docs/protocol.md（帧头 12 B / 小端 / CRC-16/CCITT-FALSE /
length ≤ 1024 / resync / ZLP 语义）。

⚠️ 独立性声明（这是本文件的全部价值所在）：
    本生成器**不 import、不调用、不对照**以下任何产品代码，否则整条验证链退化为循环论证：
      - components/webusb_proto/wu_codec.c   （C 实现，被验证对象）
      - web/proto.js                          （JS 实现，被验证对象）
    CRC 只与 CRC catalogue 的 check 值对照（CRC-16/CCITT-FALSE: "123456789" → 0x29B1），
    并用**两种独立算法**（逐位 / 256 项查表）互检。

输出：tools/proto_vectors.json —— 顶层 JSON 数组，每个元素至少含
      {name, kind, channel, flags, seq, length, payload_hex, frame_hex, crc16}
      （docs/protocol.md §8.1 要求的六个字段 + kind/length/notes；流向量额外带
       stream_hex / chunk_size / chunk_plan / zero_push_every / expect_* 字段）

用法：
    python test/host/gen_vectors.py [--out tools/proto_vectors.json] [--check]
    --check  只生成到内存并校验，不写文件（用于 CI 里的漂移检查）
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import random
import struct
import sys

# --------------------------------------------------------------------------
# 0. 协议常量（逐条抄 docs/protocol.md §2/§3，不抄任何实现）
# --------------------------------------------------------------------------

MAGIC0 = 0x4D            # 'M'
MAGIC1 = 0x50            # 'P'
VERSION = 0x01
HDR_LEN = 12             # magic2 + version1 + channel1 + flags2 + length2 + seq2 + crc2
MAX_PAYLOAD = 1024
POLY = 0x1021
INIT = 0xFFFF

CH_CTRL, CH_VIDEO, CH_FILE, CH_LOG, CH_TELEM = 0, 1, 2, 3, 4

FLAG_MORE = 0x0001
FLAG_ACK = 0x0002
FLAG_RETX = 0x0004
FLAG_ABORT = 0x0008


# --------------------------------------------------------------------------
# 1. CRC-16/CCITT-FALSE —— 两种独立算法 + catalogue check 值
# --------------------------------------------------------------------------

def crc16_bitwise(data: bytes) -> int:
    """poly=0x1021 init=0xFFFF refin=false refout=false xorout=0（逐位实现）"""
    crc = INIT
    for b in data:
        crc ^= (b << 8) & 0xFFFF
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ POLY) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def _build_table() -> list[int]:
    table = []
    for i in range(256):
        c = (i << 8) & 0xFFFF
        for _ in range(8):
            c = ((c << 1) ^ POLY) & 0xFFFF if (c & 0x8000) else ((c << 1) & 0xFFFF)
        table.append(c)
    return table


_TABLE = _build_table()


def crc16_table(data: bytes) -> int:
    """与 crc16_bitwise 数学等价、但实现路径完全不同的查表版"""
    crc = INIT
    for b in data:
        crc = ((crc << 8) & 0xFFFF) ^ _TABLE[((crc >> 8) ^ b) & 0xFF]
    return crc


CRC16 = crc16_bitwise


def crc16_selftest() -> None:
    # CRC catalogue 的 check 值（第三方常量，不是我算出来的）
    assert CRC16(b"123456789") == 0x29B1, "CRC-16/CCITT-FALSE check value 必须是 0x29B1"
    assert CRC16(b"") == INIT, "空输入应等于 init"
    # 单字节 0x00：手推：0xFFFF -> 0xEFDF -> 0xCF9F -> 0x8F1F -> 0x0E1F -> 0x1C3E -> 0x387C -> 0x70F8 -> 0xE1F0
    assert CRC16(b"\x00") == 0xE1F0
    # 两种独立实现对撞
    rnd = random.Random(0x4D50)          # 固定种子 -> 可复现
    samples = [b"", b"\x00", b"MP", bytes(range(256)), b"123456789", b"\xff" * 1024]
    samples += [bytes(rnd.randrange(256) for _ in range(rnd.randrange(0, 300))) for _ in range(64)]
    for s in samples:
        a, b = crc16_bitwise(s), crc16_table(s)
        assert a == b, ("两种 CRC 实现不一致", s[:16], hex(a), hex(b))


# --------------------------------------------------------------------------
# 2. 帧封装（docs/protocol.md §2）
# --------------------------------------------------------------------------

def pack(channel: int, flags: int, seq: int, payload: bytes,
         length: int | None = None, crc: int | None = None,
         version: int = VERSION) -> bytes:
    """按 §2 组装一帧。length 显式给出时用于构造**非法/畸形**向量。"""
    n = len(payload) if length is None else length
    hdr10 = bytes([MAGIC0, MAGIC1, version, channel]) + struct.pack("<HHH", flags, n, seq)
    assert len(hdr10) == 10
    c = CRC16(hdr10 + payload) if crc is None else crc
    return hdr10 + struct.pack("<H", c) + payload


def pack_crc(hdr10: bytes, payload: bytes) -> int:
    assert len(hdr10) == 10
    return CRC16(hdr10 + payload)


def corrupt_crc(frame: bytes) -> bytes:
    """把帧尾之前两字节 CRC 改坏（异或 0x0100，保证与原值不同）"""
    b = bytearray(frame)
    b[10] ^= 0x01
    return bytes(b)


def corrupt_byte(frame: bytes, idx: int) -> bytes:
    b = bytearray(frame)
    b[idx] ^= 0x5A
    return bytes(b)


# --------------------------------------------------------------------------
# 3. 向量素材
# --------------------------------------------------------------------------

def payload_ctrl_ping() -> bytes:
    # §5.1 CTRL 子头：opcode 0x03 PING / status 0 / arg 0
    return struct.pack("<BBH", 0x03, 0x00, 0x0000)


def payload_ctrl_hello() -> bytes:
    # §5.1 HELLO：opcode 0x01 / status 0 / arg=协议版本 0x0001 / body u32 caps=0x7F
    return struct.pack("<BBH", 0x01, 0x00, 0x0001) + struct.pack("<I", 0x0000007F)


def payload_video() -> bytes:
    # §5.2 VIDEO 固定 16 B 头 + 编码数据
    hdr = struct.pack("<BBHHHHIH", 0, 1, 1280, 720, 30, 1, 0x00000005, 0)
    assert len(hdr) == 16
    return hdr + bytes([0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46])


def payload_file() -> bytes:
    # §5.3 FILE 固定 8 B 头：op=4 READ_REQ / status 0 / xid 0x1234 / offset 0x00010000
    hdr = struct.pack("<BBHI", 0x04, 0x00, 0x1234, 0x00010000)
    assert len(hdr) == 8
    return hdr + b"hello"


def payload_log() -> bytes:
    return b"log: boot ok\n"


def payload_telem() -> bytes:
    # §5.4 TELEM 定长 36 B
    p = struct.pack("<IIIIIIII", 123456, 200000, 3000000, 4096, 8192, 1500, 3, 3000)
    p += bytes([0xD8, 0x07]) + b"\x00\x00"
    assert len(p) == 36
    return p


# 帧实例（frame_hex 的原始素材）
FRAME_CTRL_PING = pack(CH_CTRL, FLAG_ACK, 0x0000, payload_ctrl_ping())
FRAME_CTRL_HELLO = pack(CH_CTRL, FLAG_ACK, 0x0001, payload_ctrl_hello())
FRAME_VIDEO = pack(CH_VIDEO, FLAG_MORE, 0x0102, payload_video())
FRAME_FILE = pack(CH_FILE, FLAG_ACK | FLAG_RETX, 0xFFFF, payload_file())
FRAME_LOG = pack(CH_LOG, 0x0000, 42, payload_log())
FRAME_TELEM = pack(CH_TELEM, 0x0000, 300, payload_telem())
FRAME_LEN0 = pack(CH_CTRL, FLAG_ACK, 0x0000, b"")
FRAME_LEN1024_PAT = pack(CH_VIDEO, 0x0000, 0xAAAA, bytes([i & 0xFF for i in range(MAX_PAYLOAD)]))
FRAME_LEN1024_ONES = pack(CH_LOG, 0x0000, 0x0001, b"\xff" * MAX_PAYLOAD)
FRAME_FLAGS_ALL = pack(CH_FILE, FLAG_MORE | FLAG_ACK | FLAG_RETX | FLAG_ABORT, 0x00FF, b"all four flags")

# 垃圾字节：刻意含 0x4D 与 0x50 但**都不同时相邻**（制造"假 magic"陷阱）
GARBAGE = bytes([0x00, 0x11, 0x22, 0x4D, 0x33, 0x50, 0x7F])
assert b"\x4d\x50" not in GARBAGE
# 3000 字节垃圾：只用 'a'..'f'，永不含 0x4D
JUNK3000 = bytes([0x61 + (i % 6) for i in range(3000)])
assert b"\x4d\x50" not in JUNK3000

# 非法 length=1025 的候选帧：magic/version 合法，length 越界
ILLEGAL_1025_PAYLOAD = b"\xaa" * 32
ILLEGAL_1025 = pack(CH_CTRL, 0x0000, 0x0001, ILLEGAL_1025_PAYLOAD, length=1025)
# 冻结口径（components/webusb_proto/include/wu_codec.h:148-164 的 2026-10 裁决）：
#   非法候选帧头自身的 12 字节**不计入** resync_bytes（由 bad_header 以"事件"计），
#   只计入其后到下一个 4D 50 之间被跳过的垃圾字节。
#   → 本向量的 resync_bytes = 32（不是 44；"候选头也计入"的估计已明文声明**不作冻结值**）
ILLEGAL_1025_GARBAGE = len(ILLEGAL_1025_PAYLOAD)

# 带"嵌套完整帧"的坏 CRC 帧：payload 本身是一帧，用于抓"从帧内重扫"的实现
NESTED = pack(CH_CTRL, FLAG_ACK, 0x0009, b"nested")
NESTED_BAD = corrupt_crc(pack(CH_FILE, FLAG_ACK, 0x0003, NESTED))


# --------------------------------------------------------------------------
# 4. 向量装配
# --------------------------------------------------------------------------

def frame_vector(name: str, channel: int, flags: int, seq: int, payload: bytes,
                 frame: bytes, notes: str) -> dict:
    crc = int.from_bytes(frame[10:12], "little")
    assert crc == pack_crc(frame[:10], payload), "内部一致性：frame 的 crc 字段必须等于复算值"
    assert frame == pack(channel, flags, seq, payload), "内部一致性：frame 必须可被 pack 复现"
    return {
        "name": name,
        "kind": "frame",
        "channel": channel,
        "flags": flags,
        "seq": seq,
        "length": len(payload),
        "payload_hex": payload.hex(),
        "frame_hex": frame.hex(),
        "crc16": crc,
        "notes": notes,
    }


def crc_vector(name: str, data: bytes, expect: int | None, notes: str) -> dict:
    got = CRC16(data)
    if expect is not None:
        assert got == expect, (name, hex(got), hex(expect))
    return {
        "name": name,
        "kind": "crc",
        "channel": 0,
        "flags": 0,
        "seq": 0,
        "length": len(data),
        "payload_hex": data.hex(),
        "frame_hex": "",
        "crc16": got,
        "notes": notes,
    }


def expect_frame(frame: bytes) -> dict:
    """stream 向量里期望解出的帧（只留断言需要的字段）"""
    assert len(frame) >= HDR_LEN
    n = int.from_bytes(frame[6:8], "little")
    assert len(frame) == HDR_LEN + n
    return {
        "channel": frame[3],
        "flags": int.from_bytes(frame[4:6], "little"),
        "seq": int.from_bytes(frame[8:10], "little"),
        "length": n,
        "payload_hex": frame[HDR_LEN:].hex(),
    }


EMPTY_EXPECT = {"channel": 0, "flags": 0, "seq": 0, "length": 0, "payload_hex": ""}


def stream_vector(name: str, stream: bytes, expect_frames: list[bytes], notes: str, *,
                  chunk_size: int = 0, chunk_plan: list[int] | None = None,
                  zero_push_every: int = 0,
                  expect_crc_errors: int = 0, expect_bad_header: int = 0,
                  expect_resync_bytes: int = 0,
                  invariant_group: str | None = None) -> dict:
    """expect_resync_bytes 的口径（**冻结**：components/webusb_proto/include/wu_codec.h:148-164
    的 2026-10 裁决，Lead 明确声明"候选头也计入（约 44~53）"的口头估计**不作为冻结值**）：

    `resync_bytes` = 纯粹为"找下一个候选帧头"而跳过的字节数（**字节计数**，与 bad_header /
    crc_errors 两个**事件计数**正交）：
      * 非法候选帧头自身的 12 字节 **不计入**（那一段由 `bad_header` 以事件计）；
      * 只计入它之后到下一个 `4D 50` 之间被跳过的垃圾字节。
    该值必须与**喂入粒度无关**（整块喂 == 逐字节喂），这也是本文件里
    `invariant_group` 断言的内容。
    """
    exp = [expect_frame(f) for f in expect_frames]
    head = exp[0] if exp else EMPTY_EXPECT
    if chunk_plan is not None:
        assert sum(chunk_plan) == len(stream), (name, sum(chunk_plan), len(stream))
        chunk_size = -1        # 标记：用 chunk_plan
    v = {
        "name": name,
        "kind": "stream",
        "channel": head["channel"],
        "flags": head["flags"],
        "seq": head["seq"],
        "length": head["length"],
        "payload_hex": head["payload_hex"],
        "frame_hex": expect_frames[0].hex() if expect_frames else "",
        "crc16": int.from_bytes(expect_frames[0][10:12], "little") if expect_frames else 0,
        "stream_hex": stream.hex(),
        "chunk_size": chunk_size,
        "zero_push_every": zero_push_every,
        "expect_frames": exp,
        "expect_crc_errors": expect_crc_errors,
        "expect_bad_header": expect_bad_header,
        "expect_resync_bytes": expect_resync_bytes,
        "notes": notes,
    }
    if chunk_plan is not None:
        v["chunk_plan"] = chunk_plan
    if invariant_group is not None:
        v["invariant_group"] = invariant_group
    return v


def build_vectors() -> list[dict]:
    crc16_selftest()

    vs: list[dict] = []

    # ---- A. 常规帧（逐通道 + 边界） --------------------------------------
    vs.append(frame_vector("frame/ctrl/ping", CH_CTRL, FLAG_ACK, 0x0000,
                           payload_ctrl_ping(), FRAME_CTRL_PING,
                           "CTRL 通道最小帧（PING，payload 4 B）"))
    vs.append(frame_vector("frame/ctrl/hello", CH_CTRL, FLAG_ACK, 0x0001,
                           payload_ctrl_hello(), FRAME_CTRL_HELLO,
                           "CTRL HELLO，含 u32 caps 体"))
    vs.append(frame_vector("frame/video/chunk0", CH_VIDEO, FLAG_MORE, 0x0102,
                           payload_video(), FRAME_VIDEO,
                           "VIDEO 通道，固定 16 B 子头 + MORE 置位"))
    vs.append(frame_vector("frame/file/read_req", CH_FILE, FLAG_ACK | FLAG_RETX, 0xFFFF,
                           payload_file(), FRAME_FILE,
                           "FILE 通道，seq 回绕上界 0xFFFF + ACK|RETX"))
    vs.append(frame_vector("frame/log/line", CH_LOG, 0x0000, 42,
                           payload_log(), FRAME_LOG,
                           "LOG 通道，无 flag"))
    vs.append(frame_vector("frame/telem/36b", CH_TELEM, 0x0000, 300,
                           payload_telem(), FRAME_TELEM,
                           "TELEM 通道，定长 36 B payload"))
    vs.append(frame_vector("frame/ctrl/length0", CH_CTRL, FLAG_ACK, 0x0000, b"",
                           FRAME_LEN0, "边界：length=0（合法，payload_hex 为空串）"))
    vs.append(frame_vector("frame/video/length1024_pattern", CH_VIDEO, 0x0000, 0xAAAA,
                           bytes([i & 0xFF for i in range(MAX_PAYLOAD)]), FRAME_LEN1024_PAT,
                           "边界：length=1024（上限），payload = 0..255 循环"))
    vs.append(frame_vector("frame/log/length1024_ones", CH_LOG, 0x0000, 0x0001,
                           b"\xff" * MAX_PAYLOAD, FRAME_LEN1024_ONES,
                           "边界：length=1024，全 0xFF（CRC 边界）"))
    vs.append(frame_vector("frame/file/all_flags", CH_FILE,
                           FLAG_MORE | FLAG_ACK | FLAG_RETX | FLAG_ABORT, 0x00FF,
                           b"all four flags", FRAME_FLAGS_ALL,
                           "四个 flag 位同时置位，验证 flags 字段无损"))

    # ---- B. CRC 单点向量 -------------------------------------------------
    vs.append(crc_vector("crc/check_123456789", b"123456789", 0x29B1,
                         "CRC catalogue check 值（第三方常量）"))
    vs.append(crc_vector("crc/empty", b"", 0xFFFF, "空输入 = init"))
    vs.append(crc_vector("crc/single_zero", b"\x00", 0xE1F0, "单字节 0x00"))
    vs.append(crc_vector("crc/mp_magic", b"MP", None, "magic 字面量"))
    vs.append(crc_vector("crc/00_ff_256", bytes(range(256)), None, "0x00..0xFF 全字节序列"))

    # ---- C. 流式鲁棒性向量 ----------------------------------------------
    vs.append(stream_vector(
        "stream/two_frames_one_push", FRAME_CTRL_PING + FRAME_TELEM,
        [FRAME_CTRL_PING, FRAME_TELEM],
        "两帧首尾相连，一次喂入应解出两帧（协议 §6.4）",
        invariant_group="two_frames"))
    vs.append(stream_vector(
        "stream/two_frames_bytewise", FRAME_CTRL_PING + FRAME_TELEM,
        [FRAME_CTRL_PING, FRAME_TELEM],
        "同上，但每 1 字节喂一次 + 每 4 次插一个 0 字节空 push（ZLP 语义 §4.1）",
        chunk_size=1, zero_push_every=4, invariant_group="two_frames"))
    vs.append(stream_vector(
        "stream/three_frames_chunk7", FRAME_CTRL_PING + FRAME_TELEM + FRAME_FILE,
        [FRAME_CTRL_PING, FRAME_TELEM, FRAME_FILE],
        "7 字节分片（跨帧边界、非 2 的幂），应解出全部三帧", chunk_size=7))
    vs.append(stream_vector(
        "stream/resync_garbage", GARBAGE + FRAME_FILE, [FRAME_FILE],
        "7 字节垃圾（含孤立的 0x4D / 0x50 陷阱）后重新同步，后续帧不得丢",
        expect_resync_bytes=len(GARBAGE), invariant_group="garbage_resync"))
    vs.append(stream_vector(
        "stream/resync_garbage_bytewise", GARBAGE + FRAME_FILE, [FRAME_FILE],
        "同上，1 字节喂入", chunk_size=1, expect_resync_bytes=len(GARBAGE),
        invariant_group="garbage_resync"))
    vs.append(stream_vector(
        "stream/crc_field_bad", corrupt_crc(FRAME_LOG) + FRAME_TELEM, [FRAME_TELEM],
        "CRC 字段被改坏 -> 丢该帧且不影响后续帧（crc_errors=1，bad_header=0）",
        expect_crc_errors=1))
    vs.append(stream_vector(
        "stream/crc_payload_bad", corrupt_byte(FRAME_FILE, HDR_LEN) + FRAME_CTRL_PING,
        [FRAME_CTRL_PING],
        "payload 被改坏 -> 同一帧 CRC 失败丢弃，后续帧照解",
        expect_crc_errors=1))
    vs.append(stream_vector(
        "stream/junk3000_then_frame", JUNK3000 + FRAME_CTRL_PING, [FRAME_CTRL_PING],
        "3000 字节无 magic 垃圾（超解码器 1036 B 缓冲）后仍能解出后续帧",
        expect_resync_bytes=len(JUNK3000)))
    vs.append(stream_vector(
        "stream/length1025_illegal", ILLEGAL_1025 + FRAME_CTRL_PING, [FRAME_CTRL_PING],
        "非法 length=1025 -> 丢弃候选帧并 resync（bad_header=1），后续帧照解。"
        "resync_bytes=32 = 候选头之后被跳过的 32 B 垃圾；候选头自身 12 B 归 bad_header（事件），"
        "不重复计入（冻结头 wu_codec.h:148-164 的 2026-10 裁决）",
        expect_bad_header=1, expect_resync_bytes=ILLEGAL_1025_GARBAGE,
        invariant_group="length1025"))
    vs.append(stream_vector(
        "stream/length1025_bytewise", ILLEGAL_1025 + FRAME_CTRL_PING, [FRAME_CTRL_PING],
        "同上，1 字节喂入（与 length1025_illegal 属同一不变量组：喂入粒度不得改变诊断计数）",
        chunk_size=1, expect_bad_header=1, expect_resync_bytes=ILLEGAL_1025_GARBAGE,
        invariant_group="length1025"))
    vs.append(stream_vector(
        "stream/nested_magic_crc_bad", NESTED_BAD + FRAME_TELEM, [FRAME_TELEM],
        "坏 CRC 帧的 payload 里含一个完整合法帧：必须整帧跳过、不得从帧内重扫出伪帧",
        expect_crc_errors=1))
    vs.append(stream_vector(
        "stream/zlp_interleaved", FRAME_CTRL_PING, [FRAME_CTRL_PING],
        "3/0/5/0/4/0 分片喂入：0 字节 push 是合法 no-op，不推进状态（协议 §4.1）",
        chunk_plan=[0, 3, 0, 5, 0, 4, 0, 4]))
    vs.append(stream_vector(
        "stream/partial_header_only", b"MP", [],
        "只有 2 字节候选头：不得产出帧、不得计入 bad_header、不得崩",
        ))
    vs.append(stream_vector(
        "stream/frame_then_partial", FRAME_CTRL_PING + b"MP\x01\x00", [FRAME_CTRL_PING],
        "完整帧 + 半截候选头：只解出前帧"))

    names = [v["name"] for v in vs]
    assert len(names) == len(set(names)), "向量名必须唯一"
    return vs


# --------------------------------------------------------------------------
# 5. 输出
# --------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser()
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(os.path.dirname(here))
    ap.add_argument("--out", default=os.path.join(root, "tools", "proto_vectors.json"))
    ap.add_argument("--check", action="store_true", help="只校验，不写文件")
    args = ap.parse_args()

    vectors = build_vectors()
    text = json.dumps(vectors, ensure_ascii=False, indent=1) + "\n"

    n_frame = sum(1 for v in vectors if v["kind"] == "frame")
    n_crc = sum(1 for v in vectors if v["kind"] == "crc")
    n_stream = sum(1 for v in vectors if v["kind"] == "stream")

    spec = os.path.join(root, "docs", "protocol.md")
    spec_sha = hashlib.sha256(open(spec, "rb").read()).hexdigest() if os.path.exists(spec) else "(missing)"

    print("[gen_vectors] CRC-16/CCITT-FALSE check(\"123456789\") = 0x%04X  (逐位/查表双实现对撞通过)" % CRC16(b"123456789"))
    print("[gen_vectors] 向量统计: frame=%d crc=%d stream=%d  合计=%d" % (n_frame, n_crc, n_stream, len(vectors)))
    print("[gen_vectors] docs/protocol.md sha256 = %s" % spec_sha)
    print("[gen_vectors] 产物 sha256 = %s" % hashlib.sha256(text.encode("utf-8")).hexdigest())

    if args.check:
        print("[gen_vectors] --check：未写文件")
        return 0

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("[gen_vectors] 已写入 %s (%d 字节)" % (args.out, len(text.encode("utf-8"))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
