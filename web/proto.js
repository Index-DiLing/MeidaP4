// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · I0 线协议编解码器（主机侧 / JS）—— 实现
//
// 规约：docs/protocol.md（帧头 12 字节小端 / 通道 / flags / CRC-16-CCITT-FALSE /
//       ZLP / 窗口 W=8 + ACK + 200 ms 重传 ×5 / HELLO·HELLO_ACK / 3 s link_down）
// 对照实现：components/webusb_proto/wu_codec.c（**必须逐字节一致**，由 tools/proto_vectors.json
//       + tools/verify-proto.ps1 双向校验）
//
// ⚠️ 本文件是 **ES module**，且**不依赖任何 DOM / 外部资源** —— `node --input-type=module -e
//    "import('./web/proto.js').then(...)"` 必须能直接跑（离线验收链第 3 步）。
//    ⚠️ `file://` 下浏览器**禁止** ES module 跨文件加载（CORS/opaque origin），所以
//    `web/index.html` **不 import 本文件**：它内联了一份逐字节相同的实现（见 index.html 里
//    `id="wu-proto-inline"` 的脚本块），由 `web/selfcheck.mjs` 做等价性回归。
//    改本文件 = 必须同步改 index.html 的内联块，否则 selfcheck.mjs 会失败。
//
// 与 C 侧的行为口径（协议 §6.4）：
//   * magic/version/length 非法 ⇒ 从下一个字节起重新扫 `4D 50`；候选帧头本身归 badHeader，
//     其余被跳过的字节归 resyncBytes（两者都**不依赖喂入粒度**）；
//   * CRC 失败 ⇒ 丢该帧（crcErrors++）并从该帧之后继续（绝不从帧内重扫）；
//   * 0 字节输入（ZLP）是合法 no-op —— 不产生帧、不推进任何状态。

/* ============================ 常量（协议冻结值） ============================ */

/** 小端读出的 magic 值；字节序为 `4D 50`（ASCII "MP"） */
export const MAGIC = 0x504D;
export const MAGIC0 = 0x4D;
export const MAGIC1 = 0x50;
export const VERSION = 0x01;
export const HDR_LEN = 12;
export const MAX_PAYLOAD = 1024;
export const MAX_FRAME = HDR_LEN + MAX_PAYLOAD; // 1036

export const CHANNEL = { CTRL: 0, VIDEO: 1, FILE: 2, LOG: 3, TELEM: 4 };
export const CHANNEL_NAMES = ['CTRL', 'VIDEO', 'FILE', 'LOG', 'TELEM'];
export const CHANNEL_MAX = 5;

export const FLAGS = { MORE: 0x0001, ACK: 0x0002, RETX: 0x0004, ABORT: 0x0008 };

export const OP = {
  HELLO: 0x01,
  HELLO_ACK: 0x02,
  PING: 0x03,
  PONG: 0x04,
  ACK: 0x05,
  CREDIT: 0x06,
  WIFI_SCAN: 0x10,
  WIFI_SCAN_RSP: 0x11,
  WIFI_CONNECT: 0x12,
  WIFI_STATUS: 0x13,
  UI_CMD: 0x20,
  STATS_REQ: 0x40,
  STATS_RSP: 0x41,
};
export const OP_NAMES = {
  0x01: 'HELLO', 0x02: 'HELLO_ACK', 0x03: 'PING', 0x04: 'PONG', 0x05: 'ACK', 0x06: 'CREDIT',
  0x10: 'WIFI_SCAN', 0x11: 'WIFI_SCAN_RSP', 0x12: 'WIFI_CONNECT', 0x13: 'WIFI_STATUS',
  0x20: 'UI_CMD', 0x40: 'STATS_REQ', 0x41: 'STATS_RSP',
};

export const STATUS = {
  OK: 0x00, EINVAL: 0x01, ENOENT: 0x02, EIO: 0x03,
  ENOSPC: 0x04, EBUSY: 0x05, ENOTSUP: 0x06, EUNKNOWN: 0x7F,
};
export const STATUS_NAMES = ['OK', 'EINVAL', 'ENOENT', 'EIO', 'ENOSPC', 'EBUSY', 'ENOTSUP'];

export const CAP = {
  VIDEO_MJPEG: 1 << 0, VIDEO_H264: 1 << 1, FILE: 1 << 2, LOG: 1 << 3,
  TELEM: 1 << 4, WIFI: 1 << 5, UI_CMD: 1 << 6,
};

export const FILE_OP = {
  LIST_REQ: 0, LIST_RSP: 1, OPEN_REQ: 2, OPEN_RSP: 3, READ_REQ: 4, READ_RSP: 5,
  WRITE_REQ: 6, WRITE_RSP: 7, CLOSE_REQ: 8, CLOSE_RSP: 9, DELETE: 10, MKDIR: 11, RENAME: 12,
};
export const FILE_OP_NAMES = [
  'LIST_REQ', 'LIST_RSP', 'OPEN_REQ', 'OPEN_RSP', 'READ_REQ', 'READ_RSP',
  'WRITE_REQ', 'WRITE_RSP', 'CLOSE_REQ', 'CLOSE_RSP', 'DELETE', 'MKDIR', 'RENAME',
];

export const PROTO_VERSION = 0x01;      // HELLO 的 arg
export const CTRL_HDR_LEN = 4;          // §5.1
export const FILE_HDR_LEN = 8;          // §5.3
export const VIDEO_HDR_LEN = 16;        // §5.2
export const TELEM_LEN = 36;            // §5.4

export const WINDOW = 8;                // §6.1 滑动窗口
export const ACK_TIMEOUT_MS = 200;      // §6.1 重传超时
export const MAX_RETX = 5;              // §6.1 连续重传失败上限
export const LINK_DOWN_MS = 3000;       // §6.3 连续 3 s 无有效帧
export const HELLO_PERIOD_MS = 2000;    // §6.3 设备侧周期（主机侧只用于诊断显示）
export const PING_PERIOD_MS = 1000;     // §5.4 心跳节奏
export const TELEM_PERIOD_MS = 1000;    // §5.4

/** 通道语义表（哪条通道可靠、走哪个端点），供 UI 显示；端点号由 usb_fn_alloc_t 决定，不在这里硬编码 */
export const CHANNEL_INFO = {
  0: { name: 'CTRL', reliable: true, ep: 'ctrl' },
  1: { name: 'VIDEO', reliable: false, ep: 'video' },
  2: { name: 'FILE', reliable: true, ep: 'ctrl' },
  3: { name: 'LOG', reliable: false, ep: 'ctrl' },
  4: { name: 'TELEM', reliable: false, ep: 'ctrl' },
};

/* ============================ 小工具 ============================ */

/** 把各种"字节样"输入统一成 Uint8Array（不复制视图；hex 字符串会被解析） */
export function toBytes(x) {
  if (x === null || x === undefined) return new Uint8Array(0);
  if (x instanceof Uint8Array) return x;
  if (ArrayBuffer.isView(x)) return new Uint8Array(x.buffer, x.byteOffset, x.byteLength);
  if (x instanceof ArrayBuffer) return new Uint8Array(x);
  if (Array.isArray(x)) return Uint8Array.from(x);
  if (typeof x === 'string') return hexToBytes(x);
  throw new TypeError('无法把该值当作字节序列：' + Object.prototype.toString.call(x));
}

export function hexToBytes(hex) {
  const s = String(hex).replace(/[\s,_]/g, '').replace(/^0x/i, '');
  if (s.length % 2 !== 0) throw new Error('hex 长度必须是偶数：' + s.length);
  const out = new Uint8Array(s.length / 2);
  for (let i = 0; i < out.length; i++) {
    const b = parseInt(s.substr(i * 2, 2), 16);
    if (Number.isNaN(b)) throw new Error('非法 hex 字节：' + s.substr(i * 2, 2));
    out[i] = b;
  }
  return out;
}

export function bytesToHex(bytes) {
  const b = toBytes(bytes);
  let s = '';
  for (let i = 0; i < b.length; i++) s += b[i].toString(16).padStart(2, '0');
  return s;
}

export function utf8ToBytes(str) {
  if (typeof TextEncoder !== 'undefined') return new TextEncoder().encode(str);
  const out = [];
  for (let i = 0; i < str.length; i++) {  // 仅兜底：非 BMP 字符不保证正确
    const c = str.charCodeAt(i);
    if (c < 0x80) out.push(c);
    else if (c < 0x800) out.push(0xC0 | (c >> 6), 0x80 | (c & 0x3F));
    else out.push(0xE0 | (c >> 12), 0x80 | ((c >> 6) & 0x3F), 0x80 | (c & 0x3F));
  }
  return Uint8Array.from(out);
}

export function bytesToUtf8(bytes) {
  const b = toBytes(bytes);
  if (typeof TextDecoder !== 'undefined') {
    return new TextDecoder('utf-8', { fatal: false }).decode(b);
  }
  let s = '';
  for (let i = 0; i < b.length; i++) s += String.fromCharCode(b[i]);
  return s;
}

/** 小端读 u16 / u32 */
export function rd16(b, off) { return (b[off] | (b[off + 1] << 8)) & 0xFFFF; }
export function rd32(b, off) {
  return ((b[off] | (b[off + 1] << 8) | (b[off + 2] << 16) | (b[off + 3] << 24)) >>> 0);
}
export function wr16(b, off, v) { b[off] = v & 0xFF; b[off + 1] = (v >> 8) & 0xFF; }
export function wr32(b, off, v) {
  b[off] = v & 0xFF; b[off + 1] = (v >>> 8) & 0xFF; b[off + 2] = (v >>> 16) & 0xFF; b[off + 3] = (v >>> 24) & 0xFF;
}

/* ============================ CRC ============================ */

/**
 * CRC-16/CCITT-FALSE：poly=0x1021 / init=0xFFFF / refin=false / refout=false / xorout=0。
 * 与 C 侧 `wu_crc16()` 逐字节一致。
 *
 * @param {Uint8Array|ArrayBuffer|number[]|string} bytes
 * @param {number} [start=0]
 * @param {number} [end=bytes.length]
 * @returns {number} 0..65535
 */
export function crc16(bytes, start = 0, end = undefined) {
  const b = toBytes(bytes);
  const from = start | 0;
  const to = (end === undefined) ? b.length : end;
  let crc = 0xFFFF;
  for (let i = from; i < to; i++) {
    crc ^= (b[i] & 0xFF) << 8;
    for (let bit = 0; bit < 8; bit++) {
      crc = (crc & 0x8000) ? (((crc << 1) ^ 0x1021) & 0xFFFF) : ((crc << 1) & 0xFFFF);
    }
  }
  return crc & 0xFFFF;
}

/**
 * 协议 §2 的 crc16 字段口径：先喂 hdr[0..10)，再喂 payload。
 * 等价于 C 侧 `wu_frame_crc()`。
 */
export function crc16OfFrame(hdrFirst10, payload) {
  let crc = 0xFFFF;
  const h = toBytes(hdrFirst10);
  for (let i = 0; i < 10; i++) {
    crc ^= (h[i] & 0xFF) << 8;
    for (let bit = 0; bit < 8; bit++) crc = (crc & 0x8000) ? (((crc << 1) ^ 0x1021) & 0xFFFF) : ((crc << 1) & 0xFFFF);
  }
  const p = toBytes(payload);
  for (let i = 0; i < p.length; i++) {
    crc ^= (p[i] & 0xFF) << 8;
    for (let bit = 0; bit < 8; bit++) crc = (crc & 0x8000) ? (((crc << 1) ^ 0x1021) & 0xFFFF) : ((crc << 1) & 0xFFFF);
  }
  return crc & 0xFFFF;
}

/* ============================ 编码 ============================ */

/**
 * 编码一帧（协议 §2）。
 *
 * @param {{channel:number, flags?:number, seq?:number, payload?:any}} o
 * @returns {Uint8Array} 12 + payload 字节
 */
export function encodeFrame(o) {
  const opts = o || {};
  const channel = opts.channel | 0;
  const flags = (opts.flags === undefined ? 0 : opts.flags) & 0xFFFF;
  const seq = (opts.seq === undefined ? 0 : opts.seq) & 0xFFFF;
  const payload = toBytes(opts.payload);
  if (payload.length > MAX_PAYLOAD) {
    throw new RangeError('payload 超过 WU_MAX_PAYLOAD=' + MAX_PAYLOAD + '：' + payload.length);
  }
  const out = new Uint8Array(HDR_LEN + payload.length);
  out[0] = MAGIC0;
  out[1] = MAGIC1;
  out[2] = VERSION;
  out[3] = channel & 0xFF;
  wr16(out, 4, flags);
  wr16(out, 6, payload.length);
  wr16(out, 8, seq);
  wr16(out, 10, crc16OfFrame(out.subarray(0, 10), payload));
  out.set(payload, HDR_LEN);
  return out;
}

/** 把一条消息切成帧（协议 §4）。返回帧数组；`retx` 会置 RETX 位。 */
export function fragmentMessage(channel, msg, seq0, opts = {}) {
  const body = toBytes(msg);
  const wantAck = opts.ack !== false && !!(CHANNEL_INFO[channel] && CHANNEL_INFO[channel].reliable);
  const total = body.length === 0 ? 1 : Math.ceil(body.length / MAX_PAYLOAD);
  const frames = [];
  let seq = seq0 & 0xFFFF;
  for (let i = 0; i < total; i++) {
    let flags = 0;
    if (wantAck) flags |= FLAGS.ACK;
    if (i + 1 < total) flags |= FLAGS.MORE;
    if (opts.retx) flags |= FLAGS.RETX;
    const off = i * MAX_PAYLOAD;
    const chunk = body.subarray(off, Math.min(off + MAX_PAYLOAD, body.length));
    frames.push({ seq, flags, bytes: encodeFrame({ channel, flags, seq, payload: chunk }) });
    seq = (seq + 1) & 0xFFFF;
  }
  return frames;
}

/* ============================ 解码（流式） ============================ */

function findMagic(p, from, avail) {
  for (let k = from; k + 1 < avail; k++) {
    if (p[k] === MAGIC0 && p[k + 1] === MAGIC1) return k;
  }
  return avail; // 未找到
}

/**
 * 流式解码器（协议 §6.4 / §4.1）。与 C 侧 `wu_decoder_push()` 行为一致。
 *
 * `stats` **只**暴露协议 §7 冻结的三个字段：`{crcErrors, resyncBytes, frames}`。
 * 额外诊断量（badHeader / truncated）走 `diagnostics`，避免宿主写死 stats 形状时踩坑。
 */
export class FrameDecoder {
  constructor() {
    this._buf = new Uint8Array(MAX_FRAME);
    this._len = 0;
    this._stats = { crcErrors: 0, resyncBytes: 0, frames: 0, badHeader: 0, truncated: 0 };
  }

  /** 协议 §7 冻结形状 */
  get stats() {
    return { crcErrors: this._stats.crcErrors, resyncBytes: this._stats.resyncBytes, frames: this._stats.frames };
  }

  /** 诊断扩展（C 侧 wu_decoder_stats_t 的另外两个字段） */
  get diagnostics() {
    return {
      crcErrors: this._stats.crcErrors, resyncBytes: this._stats.resyncBytes, frames: this._stats.frames,
      badHeader: this._stats.badHeader, truncated: this._stats.truncated,
    };
  }

  reset() {
    this._len = 0;
    this._stats = { crcErrors: 0, resyncBytes: 0, frames: 0, badHeader: 0, truncated: 0 };
  }

  _consume(n, countAsResync) {
    if (n > this._len) n = this._len;
    if (n <= 0) return;
    if (countAsResync) this._stats.resyncBytes += n;
    this._buf.copyWithin(0, n, this._len);
    this._len -= n;
  }

  /**
   * 喂入字节，解出尽可能多的帧。
   * @param {any} bytes
   * @returns {Array<{channel:number, flags:number, seq:number, length:number, payload:Uint8Array}>}
   */
  push(bytes) {
    const frames = [];
    if (bytes === null || bytes === undefined) return frames;
    const src = toBytes(bytes);
    if (src.length === 0) return frames; // ZLP / 0 字节：合法 no-op（§4.1）
    let pos = 0;

    for (;;) {
      const space = MAX_FRAME - this._len;
      let chunk = src.length - pos;
      if (space !== 0 && chunk !== 0) {
        if (chunk > space) chunk = space;
        this._buf.set(src.subarray(pos, pos + chunk), this._len);
        this._len += chunk;
        pos += chunk;
      }

      for (;;) {
        const avail = this._len;
        const p = this._buf;

        if (avail < 2) {
          if (avail === 1 && p[0] !== MAGIC0) { this._consume(1, true); continue; }
          this._stats.truncated++;
          break;
        }

        if (p[0] !== MAGIC0 || p[1] !== MAGIC1) {
          const k = findMagic(p, 1, avail);
          if (k >= avail) {
            const drop = (p[avail - 1] === MAGIC0) ? (avail - 1) : avail;
            this._consume(drop, true);
            this._stats.truncated++;
            break;
          }
          this._consume(k, true);
          continue;
        }

        if (avail < HDR_LEN) { this._stats.truncated++; break; }

        const version = p[2];
        const length = p[6] | (p[7] << 8);
        if (version !== VERSION || length > MAX_PAYLOAD) {
          // 候选帧头非法：候选头本身归 badHeader，其余跳过的字节归 resyncBytes
          // （两者都不依赖喂入粒度，见文件头注释）
          this._stats.badHeader++;
          const k = findMagic(p, 1, avail);
          const skip = (k >= avail) ? ((p[avail - 1] === MAGIC0) ? (avail - 1) : avail) : k;
          const uncounted = Math.min(skip, HDR_LEN);
          this._consume(uncounted, false);
          this._consume(skip - uncounted, true);
          if (k >= avail) { this._stats.truncated++; break; }
          continue;
        }

        const need = HDR_LEN + length;
        if (avail < need) { this._stats.truncated++; break; }

        const crcWire = p[10] | (p[11] << 8);
        const crcCalc = crc16OfFrame(p.subarray(0, 10), p.subarray(HDR_LEN, HDR_LEN + length));
        if (crcWire !== crcCalc) {
          this._stats.crcErrors++;         // 丢该帧，从该帧之后继续（不从帧内重扫）
          this._consume(need, false);
          continue;
        }

        const payload = new Uint8Array(length);
        payload.set(p.subarray(HDR_LEN, HDR_LEN + length));
        frames.push({ channel: p[3], flags: p[4] | (p[5] << 8), seq: p[8] | (p[9] << 8), length, payload });
        this._stats.frames++;
        this._consume(need, false);
      }

      if (pos >= src.length) break;
      if (this._len === MAX_FRAME) this._consume(1, true); // 防御：缓冲满且无进展
    }
    return frames;
  }
}

/* ============================ 消息重组器 ============================ */

/** 与 C 侧 `wu_assembler_*` 等价的消息重组器（一通道一个）。 */
export class Assembler {
  constructor(channel, cap = 64 * 1024) {
    this.channel = channel;
    this.cap = cap;
    this._parts = [];
    this._len = 0;
    this.inMessage = false;
    this.nextSeq = 0;
    this.seqValid = false;
    this.stats = { msgOk: 0, msgAborted: 0, resets: 0 };
  }

  reset() {
    this._parts = [];
    this._len = 0;
    this.inMessage = false;
    this.seqValid = false;
  }

  /**
   * @param {{seq:number, flags:number, payload:Uint8Array}} frame
   * @returns {Uint8Array|null} 拼装完成的消息，否则 null
   */
  push(frame) {
    if (!frame) return null;
    if (frame.flags & FLAGS.ABORT) {
      this.stats.msgAborted++;
      this.stats.resets++;
      this.reset();
      return null;
    }
    if (this.inMessage && (!this.seqValid || frame.seq !== this.nextSeq)) {
      this.stats.msgAborted++;
      this.stats.resets++;
      this.reset();
    }
    if (!this.inMessage) { this._parts = []; this._len = 0; }

    const payload = toBytes(frame.payload);
    if (this._len + payload.length > this.cap) {
      this.stats.msgAborted++;
      this.stats.resets++;
      this.reset();
      return null;
    }
    if (payload.length) this._parts.push(payload);
    this._len += payload.length;
    this.nextSeq = (frame.seq + 1) & 0xFFFF;
    this.seqValid = true;

    if (frame.flags & FLAGS.MORE) { this.inMessage = true; return null; }

    const msg = new Uint8Array(this._len);
    let off = 0;
    for (const part of this._parts) { msg.set(part, off); off += part.length; }
    this.stats.msgOk++;
    this.reset();
    return msg;
  }
}

/* ============================ CTRL / FILE 载荷构造 ============================ */

/** CTRL 消息载荷（§5.1）：opcode / status / arg(u16) / body */
export function ctrlPayload(opcode, status = 0, arg = 0, body = null) {
  const b = toBytes(body);
  const out = new Uint8Array(CTRL_HDR_LEN + b.length);
  out[0] = opcode & 0xFF;
  out[1] = status & 0xFF;
  wr16(out, 2, arg & 0xFFFF);
  out.set(b, CTRL_HDR_LEN);
  return out;
}

/** 解析 CTRL 载荷 → {opcode, status, arg, body} */
export function parseCtrl(payload) {
  const p = toBytes(payload);
  if (p.length < CTRL_HDR_LEN) return null;
  return { opcode: p[0], status: p[1], arg: rd16(p, 2), body: p.subarray(CTRL_HDR_LEN) };
}

/** FILE 消息载荷（§5.3）：op / status / xid(u16) / offset(u32) / body */
export function filePayload(op, status = 0, xid = 0, offset = 0, body = null) {
  const b = toBytes(body);
  const out = new Uint8Array(FILE_HDR_LEN + b.length);
  out[0] = op & 0xFF;
  out[1] = status & 0xFF;
  wr16(out, 2, xid & 0xFFFF);
  wr32(out, 4, offset >>> 0);
  out.set(b, FILE_HDR_LEN);
  return out;
}

/** 解析 FILE 载荷 → {op, status, xid, offset, body} */
export function parseFile(payload) {
  const p = toBytes(payload);
  if (p.length < FILE_HDR_LEN) return null;
  return { op: p[0], status: p[1], xid: rd16(p, 2), offset: rd32(p, 4), body: p.subarray(FILE_HDR_LEN) };
}

/** 解析 TELEM 载荷（§5.4，36 字节定长）。长度不足返回 null。 */
export function parseTelem(payload) {
  const p = toBytes(payload);
  if (p.length < TELEM_LEN) return null;
  return {
    uptimeMs: rd32(p, 0),
    heapFree: rd32(p, 4),
    psramFree: rd32(p, 8),
    usbInBytes: rd32(p, 12),
    usbOutBytes: rd32(p, 16),
    videoKbps: rd32(p, 20),
    videoDropped: rd32(p, 24),
    videoFpsX100: rd32(p, 28),
    wifiRssi: (p[32] << 24) >> 24,            // i8
    flags: p[33],
    wifiUp: !!(p[33] & 0x01),
    sdMounted: !!(p[33] & 0x02),
    videoActive: !!(p[33] & 0x04),
    reserved: rd16(p, 34),
  };
}

/* ============================ 可靠通道（发送侧） ============================ */

/** seq 的环形序比较：a 在 b 之前（含相等）返回 true */
function seqLE(a, b) { return ((b - a) & 0xFFFF) < 0x8000; }

/**
 * 可靠通道发送侧：滑动窗口 W=8 + 累计 ACK + 200 ms 超时重传 + 连续 5 次失败判死
 * （协议 §6.1）。VIDEO/LOG/TELEM 不用本类。
 */
export class ReliableChannel {
  constructor(opts = {}) {
    this.channel = (opts.channel === undefined) ? CHANNEL.CTRL : opts.channel;
    this.send = opts.send || null;                 // (Uint8Array) => boolean，false = 端点忙
    this.now = opts.now || (() => Date.now());
    this.windowSize = opts.windowSize || WINDOW;
    this.ackTimeoutMs = opts.ackTimeoutMs || ACK_TIMEOUT_MS;
    this.maxRetx = (opts.maxRetx === undefined) ? MAX_RETX : opts.maxRetx;
    this.onLinkDown = opts.onLinkDown || null;
    this.onSend = opts.onSend || null;             // 诊断：每实际发出一个帧调用一次
    this.outbox = [];                              // 无 send 回调时，发出去/被重传的帧落在这里（测试用）
    this._resetState();
    this._stats = { sent: 0, retransmits: 0, acked: 0, linkDown: 0, messages: 0 };
  }

  _resetState() {
    this._nextSeq = 0;
    this._inflight = [];      // [{seq, flags, payload, retries}]
    this._lastProgressMs = this.now();
    this._dead = false;
  }

  /** 复位 TX 序号 / 窗口 / 判死状态（协议 §6.3 —— 主机收到 HELLO 时调用） */
  reset() { this._resetState(); }

  get nextSeq() { return this._nextSeq; }
  get unacked() { return this._inflight.length; }
  get linkDown() { return this._dead; }
  get space() { return this.windowSize - this._inflight.length; }
  get stats() {
    return {
      channel: this.channel, nextSeq: this._nextSeq, unacked: this._inflight.length,
      sent: this._stats.sent, retransmits: this._stats.retransmits, acked: this._stats.acked,
      messages: this._stats.messages, linkDown: this._dead,
    };
  }

  _transmit(seq, flags, payload) {
    const bytes = encodeFrame({ channel: this.channel, flags, seq, payload });
    let ok = true;
    if (typeof this.send === 'function') ok = this.send(bytes) !== false;
    this.outbox.push({ seq, flags, bytes });
    if (this.outbox.length > 4096) this.outbox.splice(0, this.outbox.length - 4096);
    this._stats.sent++;
    if (this.onSend) this.onSend(bytes, { seq, flags });
    return ok;
  }

  /**
   * 发一条消息（自动分片；可靠通道每帧置 ACK 位）。
   * @returns {number} 实际入窗并发出的帧数（窗口满时会少于总片数 → 调用方稍后重试剩余部分）
   */
  sendMessage(payload, opts = {}) {
    if (this._dead) return 0;
    const body = toBytes(payload);
    const total = body.length === 0 ? 1 : Math.ceil(body.length / MAX_PAYLOAD);
    const wantAck = opts.ack !== false;
    let sent = 0;
    for (let i = 0; i < total; i++) {
      if (this._inflight.length >= this.windowSize) break;  // 窗口满：调用方等 ACK 后再续
      const off = i * MAX_PAYLOAD;
      const chunk = body.subarray(off, Math.min(off + MAX_PAYLOAD, body.length));
      let flags = 0;
      if (wantAck) flags |= FLAGS.ACK;
      if (i + 1 < total) flags |= FLAGS.MORE;
      const seq = this._nextSeq;
      this._nextSeq = (this._nextSeq + 1) & 0xFFFF;
      const slot = { seq, flags, payload: new Uint8Array(chunk), retries: 0 };
      this._inflight.push(slot);
      this._transmit(seq, flags, slot.payload);
      sent++;
    }
    if (sent) { this._stats.messages++; this._lastProgressMs = this.now(); }
    return sent;
  }

  /** 收到累计 ACK：`arg` = 对方下一个期望的 seq ⇒ 释放 seq < arg 的全部在途帧（§6.1） */
  onAck(nextExpectedSeq) {
    const ack = nextExpectedSeq & 0xFFFF;
    const before = this._inflight.length;
    this._inflight = this._inflight.filter((slot) => !seqLE(slot.seq, (ack - 1) & 0xFFFF));
    const released = before - this._inflight.length;
    if (released) {
      this._stats.acked += released;
      this._lastProgressMs = this.now();
    }
    return released;
  }

  /**
   * 入站帧要求确认时回一个 ACK 帧（协议 §6.1 + §6.5①）。
   *
   * ⚠️ 两处**必须**按 §6.5 来（我第一版写错过，会破坏可靠通道）：
   *  - §6.5②：ACK 帧吃**本通道自己的**下一个 seq，**不是**回显对方数据帧的 seq
   *    （回显会让本方向自己的 seq 空间乱序 → 对方按 expected++ 收帧时把我们的 ACK 当重复/空洞丢掉，
   *      于是数据永远无法被确认）。
   *  - §6.5③：ACK 帧 `flags = 0`（不置 ACK 位），且**不入窗、不参与重传**，否则会形成 ACK 风暴。
   */
  ackReceivedFrame(frame, rxExpected) {
    if (!frame || !(frame.flags & FLAGS.ACK)) return false;
    // ⚠️ 2026-10-31 协议修订（§6.5②）：ACK 帧**不消耗**通道 seq。
    // ACK 帧不进重传窗口（丢了不补发），旧实现让它吃掉一个 seq 后，一旦这条 ACK 丢失，
    // 接收方的"期望 seq"就永久停住 → 之后所有数据帧被判 gap → 发送方重传 5 次 → link_down 循环。
    // 现在 ACK 帧用 seq=0 发送，双方在 seq 门之前按"长度 4 且首字节 0x05"识别并消费。
    const seq = 0;
    this._transmit(seq, 0x0000, ctrlPayload(OP.ACK, 0, rxExpected & 0xFFFF, null));
    this._stats.acksSent++;
    return true;
  }

  /**
   * 通道层消费一条完整消息：只认 `ACK` / `CREDIT`。
   *
   * §6.5①：ACK 与它确认的数据帧**同通道**。ACK 载荷是 CTRL 的 4 字节子头，而 FILE 消息最短 8 字节
   * ⇒ 可靠通道上"长度恰为 4 且首字节 = ACK opcode"的消息只可能是 ACK，不存在与 FILE 语义的歧义。
   *
   * @returns {boolean} true = 已被通道层消费（调用方不要再当数据消息派发）
   */
  handleMessage(msg) {
    const p = toBytes(msg);
    if (p.length === CTRL_HDR_LEN && p[0] === OP.ACK) { this.onAck(rd16(p, 2)); return true; }
    if (p.length === CTRL_HDR_LEN && p[0] === OP.CREDIT) return true; // 额度：暂不据此收敛窗口（报告"未实现项"）
    return false;
  }

  /** 兼容别名（旧名字，等价于 handleMessage） */
  consumeMessage(msg) { return this.handleMessage(msg); }

  /** 定时器：200 ms 无进展 ⇒ 重传窗口内最老一帧（置 RETX）；连续 5 次失败 ⇒ 判死 */
  tick(nowMs) {
    if (this._dead || this._inflight.length === 0) return 0;
    const t = (nowMs === undefined) ? this.now() : nowMs;
    if (t - this._lastProgressMs < this.ackTimeoutMs) return 0;

    const slot = this._inflight[0];
    if (slot.retries >= this.maxRetx) {
      this._inflight = [];
      this._dead = true;
      this._stats.linkDown++;
      if (this.onLinkDown) this.onLinkDown(this);
      return 0;
    }
    slot.retries++;
    this._stats.retransmits++;
    this._lastProgressMs = t;
    this._transmit(slot.seq, slot.flags | FLAGS.RETX, slot.payload);
    return 1;
  }
}

/* ============================ 链路会话（HELLO / 重连 / ZLP） ============================ */

/**
 * 链路会话状态机（协议 §6.3 / §7）：
 *   - 设备是序号主导方：收到 `HELLO` ⇒ 回 `HELLO_ACK` + 复位本侧所有 TX seq / RX 期望 / 拼接态；
 *   - 连续 3 s 没收到任何**有效帧** ⇒ `link_down`（UI 显示"重连中"），清窗口与拼接态，
 *     期间**不做任何可靠发送**；
 *   - 0 字节 IN 读（ZLP）是合法事件：计入 `zeroLenReads` 并继续，不推进任何状态（§4.1）。
 */
export class LinkSession {
  constructor(opts = {}) {
    this.send = opts.send || null;                    // (Uint8Array) => boolean：把帧写到设备（EP4 OUT）
    this.now = opts.now || (() => Date.now());
    this.linkDownMs = opts.linkDownMs || LINK_DOWN_MS;
    this.decoder = opts.decoder || new FrameDecoder();
    this._handlers = new Map();
    this._assemblers = {};
    this._asmBufs = {};
    this._rxExpected = {};
    for (let ch = 0; ch < CHANNEL_MAX; ch++) {
      this._rxExpected[ch] = 0;
      const a = new Assembler(ch);
      this._assemblers[ch] = a;
      this._asmBufs[ch] = new Uint8Array(MAX_FRAME * 4);
    }
    const mk = (channel) => new ReliableChannel({
      channel, send: this.send, now: this.now,
      onSend: () => {},
      onLinkDown: (c) => this._onChannelDead(c),
    });
    this.ctrl = opts.ctrl || mk(CHANNEL.CTRL);
    this.file = opts.file || mk(CHANNEL.FILE);
    this._reliable = { 0: this.ctrl, 2: this.file };

    this.state = 'waiting_hello';       // 'waiting_hello' | 'up' | 'link_down'
    this.lastRxMs = this.now();
    this.caps = 0;
    this.deviceVersion = null;
    this.zeroLenReads = 0;              // §4.1：ZLP 计数
    this._stats = { frames: 0, helloSent: 0, helloAckSent: 0, helloLost: 0,
                    linkDowns: 0, linkUps: 0, rxMessages: 0, txMessages: 0, crcErrors: 0, resyncBytes: 0,
                    acksSent: 0, pingsSent: 0, pongsSent: 0 };
  }

  /* ---- 事件（极简 emitter，不依赖 DOM） ---- */
  on(name, cb) {
    if (!this._handlers.has(name)) this._handlers.set(name, []);
    this._handlers.get(name).push(cb);
    return this;
  }
  emit(name, data) {
    const list = this._handlers.get(name);
    if (!list) return;
    for (const cb of list) {
      try { cb(data); } catch (e) { /* 回调异常不得打断解析 */ }
    }
  }

  get linkUp() { return this.state === 'up'; }
  get stats() {
    return {
      state: this.state,
      linkUp: this.state === 'up',
      zeroLenReads: this.zeroLenReads,
      crcErrors: this.decoder.stats.crcErrors,
      resyncBytes: this.decoder.stats.resyncBytes,
      frames: this.decoder.stats.frames,
      badHeader: this.decoder.diagnostics.badHeader,
      lastRxMs: this.lastRxMs,
      ctrlUnacked: this.ctrl.unacked,
      fileUnacked: this.file.unacked,
      ...this._stats,
    };
  }

  _sendBytes(bytes) {
    if (typeof this.send === 'function') {
      try { return this.send(bytes) !== false; } catch (e) { return false; }
    }
    return true;
  }

  /* ---- 收到原始 IN 字节 ---- */
  onBytes(bytes) {
    const frames = this.decoder.push(bytes);
    for (const f of frames) this._onFrame(f);
    return frames;
  }

  /** 0 字节 IN 读 = 合法 ZLP（§4.1）：只计数，不推进任何状态 */
  onZeroLenRead() {
    this.zeroLenReads++;
    return this.zeroLenReads;
  }

  _onFrame(frame) {
    this._stats.frames++;
    this.lastRxMs = this.now();
    if (this.state !== 'up') {
      this.state = 'up';
      this._stats.linkUps++;
      this.emit('link', { state: 'up' });
    }

    const ch = frame.channel;
    if (ch >= CHANNEL_MAX) return;

    // §6.3 + §6.5④：`HELLO` 是**序号主导方**的引导帧，必须**绕开 seq 门**处理。
    // 引导期主机会发 stateless PING（seq=0/flags=0，§6.5④），它会占掉"下一个期望 seq"，
    // 而设备的周期性 HELLO 也可能是同一个 seq —— 若让 HELLO 走普通去重逻辑，
    // 它会被当成"重复 seq"丢掉，链路永远收敛不了。故 HELLO 单帧在此直接放行。
    const bootstrap = (ch === CHANNEL.CTRL && frame.payload && frame.payload[0] === OP.HELLO);

    const reliable = !!(CHANNEL_INFO[ch] && CHANNEL_INFO[ch].reliable);
    // ACK 帧不参与 seq 期望值（2026-10-31 修订，见 ackReceivedFrame 说明）
    if (reliable && frame.length === CTRL_HDR_LEN && frame.payload && frame.payload[0] === OP.ACK) {
      const rcAck = this._reliable[ch];
      if (rcAck) rcAck.onAck(rd16(frame.payload, 2));
      return;
    }
    if (reliable && !bootstrap) {
      const rc = this._reliable[ch];
      const expect = this._rxExpected[ch];
      const isDup = (frame.seq !== expect) && seqLE(frame.seq, (expect - 1) & 0xFFFF);
      if (isDup) {
        // §6.1：重复 seq 必须幂等 —— 再回一次 ACK，**不**重复投递
        if (rc) rc.ackReceivedFrame(frame, expect);
        return;
      }
      if (frame.seq !== expect) {
        // 乱序（有空洞）：累计 ACK 停在上一个期望值，不投递；发送方 200 ms 后会重传最老帧
        if (rc) rc.ackReceivedFrame(frame, expect);
        this.emit('gap', { channel: ch, got: frame.seq, expected: expect });
        return;
      }
      this._rxExpected[ch] = (expect + 1) & 0xFFFF;
      if (rc) rc.ackReceivedFrame(frame, this._rxExpected[ch]);
    }

    const asm = this._assemblers[ch];
    const msg = asm.push(frame);
    if (msg === null) return;
    this._stats.rxMessages++;

    // §6.5①：ACK/CREDIT 由通道层消费，不再当数据消息派发
    if (reliable && this._reliable[ch] && this._reliable[ch].handleMessage(msg)) return;

    if (ch === CHANNEL.CTRL) this._onCtrlMessage(msg, frame);
    else this.emit('message', { channel: ch, payload: msg, frame });
  }

  _onCtrlMessage(msg, frame) {
    const c = parseCtrl(msg);
    if (!c) return;
    this.emit('ctrl', c);

    switch (c.opcode) {
      case OP.HELLO: {
        // 设备是序号主导方：收到 HELLO ⇒ 复位本侧序号与拼接态，再回 HELLO_ACK（§6.3）
        this.deviceVersion = c.arg;
        this.caps = (c.body && c.body.length >= 4) ? rd32(c.body, 0) : 0;
        this.resetSequences();
        this.sendHelloAck();
        this.state = 'up';
        this.emit('hello', { version: c.arg, caps: this.caps });
        break;
      }
      case OP.HELLO_ACK:
        this.state = 'up';
        this.emit('helloAck', c);
        break;
      case OP.PING:
        this.sendPong();
        this.emit('ping', c);
        break;
      case OP.PONG:
        this.emit('pong', c);
        break;
      case OP.ACK:
        this.ctrl.onAck(c.arg);
        this.emit('ack', c);
        break;
      case OP.CREDIT:
        this.emit('credit', c);
        break;
      case OP.TELEM:
        break;
      default:
        this.emit('ctrlMessage', { ctrl: c, frame });
        break;
    }
    // 注：ACK / CREDIT 已在 `_onFrame()` 里由通道层（§6.5①）消费，这里**不再**重复消费
  }

  /* ---- 发送 API（state 不为 up 时不做可靠发送，§6.3） ---- */

  /** 复位本侧所有 TX seq / RX 期望 / 拼接态（收 HELLO 时调用） */
  resetSequences() {
    for (let ch = 0; ch < CHANNEL_MAX; ch++) {
      this._rxExpected[ch] = 0;
      this._assemblers[ch].reset();
    }
    this.ctrl.reset();
    this.file.reset();
  }

  /**
   * 回 `HELLO_ACK`（**引导帧，无状态**：`seq = 0`、`flags = 0`、不入可靠窗口）。
   *
   * 依据 §6.5④ 的同一理由（引导期的帧若占窗口，会在设备周期性 HELLO 触发的序号重置
   * 变成悬空未确认帧）：`HELLO` 每 2 s 重发一次，所以本握手天然幂等 ——
   * 丢了就等下一个 HELLO 再回，不需要重传，也不该占用 seq 空间。
   * ⚠️ 因此设备侧必须**绕开 seq 门**识别 `HELLO_ACK`（`webusb_proto.c` 里同款处理）。
   */
  sendHelloAck() {
    const ok = this.sendRaw(CHANNEL.CTRL, ctrlPayload(OP.HELLO_ACK, 0, PROTO_VERSION, null), 0, 0);
    if (ok) this._stats.helloAckSent++;
    return ok;
  }

  /**
   * 心跳 PING：`link_up` 之后是**普通可靠帧**（占 seq、入窗口、可重传）；
   * 引导期（`HELLO` 与 `HELLO_ACK` 之间）按 §6.5④ 走无状态 `seq = 0` / `flags = 0`。
   */
  sendPing() {
    const ok = (this.state === 'up')
      ? this.sendCtrl(OP.PING, { force: true })
      : this.sendRaw(CHANNEL.CTRL, ctrlPayload(OP.PING, 0, 0, null), 0, 0);
    if (ok) this._stats.pingsSent++;
    return ok;
  }

  sendPong() {
    const ok = this.sendCtrl(OP.PONG, { force: true });
    if (ok) this._stats.pongsSent++;
    return ok;
  }

  /**
   * 发一条 CTRL 消息（可靠）。link_down 期间默认拒绝（`force` 可绕过，仅用于 HELLO_ACK/PING）。
   * @returns {boolean} 是否已入窗发出
   */
  sendCtrl(opcode, opts = {}) {
    if (this.state !== 'up' && !opts.force) return false;
    const payload = ctrlPayload(opcode, opts.status || 0, opts.arg || 0, opts.body || null);
    const n = this.ctrl.sendMessage(payload, { ack: opts.ack !== false });
    if (n) this._stats.txMessages++;
    return n > 0;
  }

  /** 发一条 FILE 消息（可靠，§5.3 的 8 字节头 + xid 配对） */
  sendFile(op, opts = {}) {
    if (this.state !== 'up' && !opts.force) return false;
    const payload = filePayload(op, opts.status || 0, opts.xid || 0, opts.offset || 0, opts.body || null);
    const n = this.file.sendMessage(payload, { ack: true });
    if (n) this._stats.txMessages++;
    return n > 0;
  }

  /** 不可靠通道的单帧发送（LOG 回显等；设备→主机为主，主机侧仅诊断用） */
  sendRaw(channel, payload, seq = 0, flags = 0) {
    return this._sendBytes(encodeFrame({ channel, flags, seq, payload }));
  }

  /** 定时器：3 s 无有效帧 ⇒ link_down；同时驱动两个可靠通道的重传 */
  tick(nowMs) {
    const t = (nowMs === undefined) ? this.now() : nowMs;
    let retx = 0;
    if (this.state === 'up' && this.linkDownMs > 0 && (t - this.lastRxMs) >= this.linkDownMs) {
      this.state = 'link_down';
      this._stats.linkDowns++;
      this.resetSequences();
      this.emit('link', { state: 'link_down' });
    }
    // 重连提示：每 2 s 使一次 HELLO 期望（主机侧只提示，不主动发 HELLO —— 设备才是主导方）
    if (this.state !== 'up' && (t - this.lastRxMs) > 0 && this.linkDownMs > 0) {
      const cycles = Math.floor((t - this.lastRxMs) / HELLO_PERIOD_MS);
      if (cycles > this._stats.helloLost) this._stats.helloLost = cycles;
    }
    if (this.state === 'up') {
      retx += this.ctrl.tick(t);
      retx += this.file.tick(t);
    }
    return retx;
  }

  _onChannelDead() {
    this.state = 'link_down';
    this._stats.linkDowns++;
    this.resetSequences();
    this.emit('link', { state: 'link_down', reason: 'retx_exhausted' });
  }
}

/* ============================ 默认导出（便于 `import WU from './proto.js'`） ============================ */

const WU = {
  MAGIC, MAGIC0, MAGIC1, VERSION, HDR_LEN, MAX_PAYLOAD, MAX_FRAME,
  CHANNEL, CHANNEL_NAMES, CHANNEL_MAX, CHANNEL_INFO,
  FLAGS, OP, OP_NAMES, STATUS, STATUS_NAMES, CAP, FILE_OP, FILE_OP_NAMES,
  PROTO_VERSION, CTRL_HDR_LEN, FILE_HDR_LEN, VIDEO_HDR_LEN, TELEM_LEN,
  WINDOW, ACK_TIMEOUT_MS, MAX_RETX, LINK_DOWN_MS, HELLO_PERIOD_MS, PING_PERIOD_MS, TELEM_PERIOD_MS,
  toBytes, hexToBytes, bytesToHex, utf8ToBytes, bytesToUtf8, rd16, rd32, wr16, wr32,
  crc16, crc16OfFrame, encodeFrame, fragmentMessage,
  FrameDecoder, Assembler, ReliableChannel, LinkSession,
  ctrlPayload, parseCtrl, filePayload, parseFile, parseTelem,
};
export default WU;
