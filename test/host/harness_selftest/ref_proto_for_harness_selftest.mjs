// SPDX-License-Identifier: Apache-2.0
//
// ⚠️⚠️ 这不是产品代码，也**不是**被验证对象 ⚠️⚠️
//
// 用途（仅此一个）：在 `web/proto.js`（ws-webusb 产出）尚未落地时，用一份临时的、
// 按 docs/protocol.md 直写的参考实现把 `test/host/run_proto_js.mjs`（测试骨架本身）跑通。
//   * 用它跑出的 PASS **不能**作为 web/proto.js 合规的证据。
//   * tools/verify-proto.ps1 默认**不**使用本文件；只有 `-HarnessSelftest` 才用。

export const MAGIC = 0x504d;              // 字节序为 4D 50
export const HEADER_LEN = 12;
export const MAX_PAYLOAD = 1024;
export const VERSION = 0x01;

export function crc16(bytes) {
  let crc = 0xffff;
  for (const b of bytes) {
    crc ^= (b << 8) & 0xffff;
    for (let i = 0; i < 8; i++) {
      crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
    }
  }
  return crc & 0xffff;
}

export function encodeFrame({ channel, flags = 0, seq = 0, payload = new Uint8Array(0) }) {
  const pl = payload instanceof Uint8Array ? payload : Uint8Array.from(payload);
  if (pl.length > MAX_PAYLOAD) throw new RangeError('payload > 1024');
  const out = new Uint8Array(HEADER_LEN + pl.length);
  out[0] = 0x4d; out[1] = 0x50; out[2] = VERSION; out[3] = channel & 0xff;
  const dv = new DataView(out.buffer);
  dv.setUint16(4, flags & 0xffff, true);
  dv.setUint16(6, pl.length, true);
  dv.setUint16(8, seq & 0xffff, true);
  out.set(pl, HEADER_LEN);
  // ⚠️ CRC 覆盖 hdr[0..10) + payload —— **不含** crc16 字段自身（docs/protocol.md §2）
  const cover = new Uint8Array(10 + pl.length);
  cover.set(out.subarray(0, 10), 0);
  cover.set(pl, 10);
  dv.setUint16(10, crc16(cover), true);
  return out;
}

/** CRC 覆盖 buf[0..10) + buf[12..total) —— 与 docs/protocol.md §2 一致 */
function crcOverFrame(buf, total) {
  const cover = new Uint8Array(10 + (total - HEADER_LEN));
  cover.set(buf.subarray(0, 10), 0);
  cover.set(buf.subarray(HEADER_LEN, total), 10);
  return crc16(cover);
}

export class FrameDecoder {
  #buf = new Uint8Array(HEADER_LEN + MAX_PAYLOAD);
  #len = 0;
  #crcErrors = 0;
  #badHeader = 0;
  #resyncBytes = 0;
  #frames = 0;

  get stats() {
    return { frames: this.#frames, crcErrors: this.#crcErrors, badHeader: this.#badHeader, resyncBytes: this.#resyncBytes };
  }

  #drop(n) {
    const keep = this.#len - n;
    this.#buf.copyWithin(0, n, this.#len);
    this.#len = keep < 0 ? 0 : keep;
  }

  push(bytes, cb) {
    const out = [];
    if (!bytes || bytes.length === 0) return out;
    let off = 0;
    while (off < bytes.length) {
      const space = this.#buf.length - this.#len;
      const take = Math.min(space, bytes.length - off);
      this.#buf.set(bytes.subarray(off, off + take), this.#len);
      this.#len += take;
      off += take;
      for (;;) {
        if (this.#len < HEADER_LEN) break;
        if (this.#buf[0] !== 0x4d || this.#buf[1] !== 0x50) { this.#resyncBytes++; this.#drop(1); continue; }
        const dv = new DataView(this.#buf.buffer);
        const plen = dv.getUint16(6, true);
        if (this.#buf[2] !== VERSION || plen > MAX_PAYLOAD) {
          // 冻结口径（wu_codec.h:148-164）：候选头自身 12 B 归 badHeader 事件、不计字节；
          // 其后的垃圾字节计入 resyncBytes。必须与喂入粒度无关。
          this.#badHeader++;
          let k = 1;
          while (k + 1 < this.#len && !(this.#buf[k] === 0x4d && this.#buf[k + 1] === 0x50)) k++;
          const noMagic = k + 1 >= this.#len;
          const skip = noMagic ? (this.#buf[this.#len - 1] === 0x4d ? this.#len - 1 : this.#len) : k;
          const uncounted = Math.min(skip, HEADER_LEN);
          this.#drop(uncounted);
          this.#resyncBytes += skip - uncounted;
          this.#drop(skip - uncounted);
          if (noMagic) break;
          continue;
        }
        const total = HEADER_LEN + plen;
        if (this.#len < total) break;
        const got = dv.getUint16(10, true);
        const calc = crcOverFrame(this.#buf, total);
        if (got !== calc) { this.#crcErrors++; this.#drop(total); continue; }
        const frame = {
          channel: this.#buf[3],
          flags: dv.getUint16(4, true),
          seq: dv.getUint16(8, true),
          payload: this.#buf.slice(HEADER_LEN, total),
        };
        this.#frames++;
        this.#drop(total);
        out.push(frame);
        if (cb && cb(frame) === false) return out;
      }
      if (this.#len === this.#buf.length) { this.#resyncBytes++; this.#drop(1); }
    }
    return out;
  }
}

/** 行为骨架（窗口/ACK 语义需要真机或额外向量，离线只保证可构造） */
export class ReliableChannel {
  constructor(opts = {}) { this.window = opts.window ?? 8; this.timeoutMs = opts.timeoutMs ?? 200; }
}
export class LinkSession {
  constructor() { this.zeroLenReads = 0; this.linkUp = false; }
  onZeroLengthRead() { this.zeroLenReads++; }
}
