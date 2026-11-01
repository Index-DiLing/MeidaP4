// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · 浏览器内文件管理器（FILE 通道，协议 §5.3）
//
// 归 I0（**冻结，不得改**）：payload 前 8 字节头 `{u8 op, u8 status, u16 xid, u32 offset}`
//   + **`xid` 请求/响应配对规则**。本文件只定义 8 字节头**之后**的 body 语义与客户端 API。
//
// ⚠️ body 语义（本文件定义，**尚未与设备侧对齐**）：
//   设备侧 `components/usb_vendor/file_service.c` 目前**不存在**（我 grep 过整个 components/）。
//   docs/protocol.md §5.3 明确"详细文件操作语义由 WS-webusb 在 web/file.js +
//   components/usb_vendor/file_service.c 里定"——所以下面这张表就是 JS 侧的提案，
//   **写设备侧的同事/Lead 必须照它实现，或先报 Lead 改本文件**（两边不一致 = FILE 通道不可用）。
//
//   | op | 名字 | 方向 | 8 字节头里的字段 | body |
//   |---|---|---|---|---|
//   | 0  | LIST_REQ | H→D | xid | `char path[] \0`（UTF-8） |
//   | 1  | LIST_RSP | D→H | xid | `N × { u32 size, u8 is_dir, char name[256] }`（每条 261 B，可跨帧分片） |
//   | 2  | OPEN_REQ | H→D | xid | `u8 mode`（0=只读 1=写/创建/截断） + `char path[] \0` |
//   | 3  | OPEN_RSP | D→H | xid | `u32 handle, u32 size, u8 is_dir, u8 rsv[3]`（12 B） |
//   | 4  | READ_REQ | H→D | xid, **offset** | `u32 handle, u32 length`（8 B；length ≤ 1016） |
//   | 5  | READ_RSP | D→H | xid, **offset** | 文件数据（长度 = 实际读到；status=OK 且 0 字节 ⇒ EOF） |
//   | 6  | WRITE_REQ | H→D | xid, **offset** | `u32 handle` + 数据（数据 ≤ 1012 B） |
//   | 7  | WRITE_RSP | D→H | xid | `u32 written`（4 B） |
//   | 8  | CLOSE_REQ | H→D | xid | `u32 handle`（4 B） |
//   | 9  | CLOSE_RSP | D→H | xid | 空 |
//   | 10 | DELETE | H→D | xid | `char path[] \0` |
//   | 11 | MKDIR | H→D | xid | `char path[] \0` |
//   | 12 | RENAME | H→D | xid | `char src[] \0 char dst[] \0` |
//
//   ⚠️ 10/11/12 **没有独立的 `_RSP` opcode**（§5.3 的 opcode 表就是这么定的）：
//      约定 = 设备**原样回同一个 op**（status 填 §5.1 错误码、xid 原样回），客户端按 xid 配对待之。
//
// 可靠性与流控：本模块**不做**重传/窗口（那是 proto.js 的 `ReliableChannel` 的事）——
//   它只走 `LinkSession.sendFile()`，并把收到的 FILE 消息按 `xid` 配对给发起方。
//   客户端自身负责：每个请求一个超时（默认 2 s，超时即拒绝，不静默吞掉）、xid 回绕、
//   迟到响应丢弃计数、以及"窗口满/链路未 up 时退避重试"。

import {
  CHANNEL,
  FILE_HDR_LEN,
  FILE_OP,
  FILE_OP_NAMES,
  MAX_PAYLOAD,
  STATUS,
  bytesToUtf8,
  filePayload,
  parseFile,
  toBytes,
  utf8ToBytes,
  wr32,
  rd32,
} from './proto.js';

/* ==========================================================================
 * 常量
 * ======================================================================== */

/** LIST_RSP / OPEN_RSP 里名字字段的固定宽度（协议 §5.3：`char name[256]`） */
export const FILE_NAME_LEN = 256;
/** LIST_RSP 单条目录项的字节数 */
export const FILE_ENTRY_LEN = 4 + 1 + FILE_NAME_LEN; // 261
/** WRITE_REQ 单帧可携带的数据字节数：8(头) + 4(handle) + N ≤ 1024 */
export const MAX_WRITE_DATA = MAX_PAYLOAD - FILE_HDR_LEN - 4; // 1012
/** READ_REQ 单次可请求的字节数（READ_RSP 的 body 直接是数据：8 + N ≤ 1024） */
export const MAX_READ_DATA = MAX_PAYLOAD - FILE_HDR_LEN; // 1016
/** OPEN_REQ 的 mode */
export const OPEN_MODE = { READ: 0, WRITE: 1 };
/** xid 请求默认超时 */
export const DEFAULT_XID_TIMEOUT_MS = 2000;
/** 可靠窗口满时的退避重试上限 */
export const DEFAULT_SEND_RETRY_MS = 2000;

/** 状态码名字（§5.1） */
export const STATUS_NAME = {
  0x00: 'OK', 0x01: 'EINVAL', 0x02: 'ENOENT', 0x03: 'EIO',
  0x04: 'ENOSPC', 0x05: 'EBUSY', 0x06: 'ENOTSUP', 0x7F: 'EUNKNOWN',
};

/** FILE 通道错误（带 `name` / `status`，便于 UI 分类显示） */
export class FileError extends Error {
  constructor(name, message, status) {
    super(message);
    this.name = name;
    this.status = (status === undefined) ? null : status;
    this.statusName = (status === undefined) ? null : (STATUS_NAME[status] || ('0x' + status.toString(16)));
  }
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function pathBytes(path) {
  const b = utf8ToBytes(String(path === undefined ? '/' : path));
  if (b.indexOf(0) >= 0) throw new FileError('EINVAL', '路径里不允许出现 NUL');
  const out = new Uint8Array(b.length + 1); // 末尾补 NUL
  out.set(b, 0);
  return out;
}

/** 从 NUL 结尾的字节串里读出一个字符串（没有 NUL 时读到末尾） */
function cstr(bytes, off) {
  const b = toBytes(bytes);
  let end = off;
  while (end < b.length && b[end] !== 0) end++;
  return bytesToUtf8(b.subarray(off, end));
}

function concatBytes(list) {
  let n = 0;
  for (const x of list) n += x.length;
  const out = new Uint8Array(n);
  let off = 0;
  for (const x of list) { out.set(x, off); off += x.length; }
  return out;
}

/* ==========================================================================
 * 纯编解码（可离线单测；与设备侧必须逐字节一致）
 * ======================================================================== */

/** 编码一条 FILE 消息的 payload（8 字节头 + body）。等价于 proto.js 的 `filePayload`，此处只做语义包装。 */
export function encodeFileMessage(op, { status = 0, xid = 0, offset = 0, body = null } = {}) {
  return filePayload(op, status, xid, offset, body);
}

/**
 * 解析一条 FILE 消息的 payload → `{op, opName, status, statusName, xid, offset, body, malformed}`
 * 永不抛异常（长度不足返回 `{malformed:true}`），因为它是接收入口。
 */
export function decodeFileMessage(payload) {
  const p = toBytes(payload);
  if (p.length < FILE_HDR_LEN) {
    return { malformed: true, length: p.length, op: null, opName: null, status: null, xid: null, offset: null, body: new Uint8Array(0) };
  }
  const m = parseFile(p);
  return {
    malformed: false,
    op: m.op,
    opName: FILE_OP_NAMES[m.op] || ('OP' + m.op),
    status: m.status,
    statusName: STATUS_NAME[m.status] || ('0x' + m.status.toString(16)),
    xid: m.xid,
    offset: m.offset,
    body: m.body,
  };
}

/** LIST_RSP：把目录项数组编码成 body（每项 261 B）。 */
export function encodeListEntries(entries) {
  const list = entries || [];
  const out = new Uint8Array(list.length * FILE_ENTRY_LEN);
  list.forEach((e, i) => {
    const off = i * FILE_ENTRY_LEN;
    wr32(out, off, (e.size || 0) >>> 0);
    out[off + 4] = e.isDir ? 1 : 0;
    const name = utf8ToBytes(String(e.name === undefined ? '' : e.name));
    const n = Math.min(name.length, FILE_NAME_LEN - 1); // 必须留一个 NUL
    out.set(name.subarray(0, n), off + 5);
    out[off + 5 + n] = 0;
  });
  return out;
}

/**
 * LIST_RSP：解析 body。
 * 尾部不足一条（261 B）的残片**丢弃并计数**，绝不越界读。
 * @returns {{entries:Array, truncatedTail:number}}
 */
export function decodeListEntries(body) {
  const b = toBytes(body);
  const full = Math.floor(b.length / FILE_ENTRY_LEN);
  const entries = [];
  for (let i = 0; i < full; i++) {
    const off = i * FILE_ENTRY_LEN;
    entries.push({
      size: rd32(b, off),
      isDir: b[off + 4] !== 0,
      name: cstr(b, off + 5),
    });
  }
  return { entries, truncatedTail: b.length - full * FILE_ENTRY_LEN };
}

/** OPEN_RSP：`u32 handle, u32 size, u8 is_dir, u8 rsv[3]` */
export function encodeOpenRsp({ handle = 0, size = 0, isDir = false } = {}) {
  const out = new Uint8Array(12);
  wr32(out, 0, handle >>> 0);
  wr32(out, 4, size >>> 0);
  out[8] = isDir ? 1 : 0;
  return out;
}

export function decodeOpenRsp(body) {
  const b = toBytes(body);
  if (b.length < 9) throw new FileError('EIO', 'OPEN_RSP body 长度 ' + b.length + ' < 9（需要 handle/size/is_dir）');
  return { handle: rd32(b, 0), size: rd32(b, 4), isDir: b[8] !== 0 };
}

/** READ_REQ body：`u32 handle, u32 length` */
export function encodeReadReq({ handle = 0, length = 0 } = {}) {
  const out = new Uint8Array(8);
  wr32(out, 0, handle >>> 0);
  wr32(out, 4, length >>> 0);
  return out;
}

export function decodeReadReq(body) {
  const b = toBytes(body);
  if (b.length < 8) throw new FileError('EINVAL', 'READ_REQ body 长度 ' + b.length + ' < 8');
  return { handle: rd32(b, 0), length: rd32(b, 4) };
}

/** WRITE_REQ body：`u32 handle` + 数据 */
export function encodeWriteReq({ handle = 0 } = {}, data) {
  const d = toBytes(data);
  if (d.length > MAX_WRITE_DATA) {
    throw new FileError('EINVAL', 'WRITE 数据 ' + d.length + ' B 超过单帧上限 ' + MAX_WRITE_DATA);
  }
  const out = new Uint8Array(4 + d.length);
  wr32(out, 0, handle >>> 0);
  out.set(d, 4);
  return out;
}

export function decodeWriteReq(body) {
  const b = toBytes(body);
  if (b.length < 4) throw new FileError('EINVAL', 'WRITE_REQ body 长度 ' + b.length + ' < 4');
  return { handle: rd32(b, 0), data: b.subarray(4) };
}

/** WRITE_RSP body：`u32 written` */
export function encodeWriteRsp({ written = 0 } = {}) {
  const out = new Uint8Array(4);
  wr32(out, 0, written >>> 0);
  return out;
}

export function decodeWriteRsp(body) {
  const b = toBytes(body);
  if (b.length < 4) return { written: 0, malformed: true };
  return { written: rd32(b, 0), malformed: false };
}

/** 各种"带处理句柄"的请求：`u32 handle` */
export function encodeHandle(handle) {
  const out = new Uint8Array(4);
  wr32(out, 0, handle >>> 0);
  return out;
}

/** RENAME body：`src \0 dst \0` */
export function encodeRenameBody(from, to) {
  return concatBytes([pathBytes(from), pathBytes(to)]);
}

/* ==========================================================================
 * FileClient —— 走 FILE 通道的请求/响应客户端（xid 配对）
 * ======================================================================== */

/**
 * @example
 *   const fc = new FileClient({ session });                       // session = proto.js LinkSession
 *   session.on('message', ({channel, payload}) => {
 *     if (channel === CHANNEL.FILE) fc.onMessage(payload);
 *   });
 *   const {entries} = await fc.list('/');
 */
export class FileClient {
  /**
   * @param {{session:object, now?:function():number, xidStart?:number, timeoutMs?:number,
   *          maxInFlight?:number, onEvent?:function(string,object)}} opts
   */
  constructor(opts = {}) {
    this.session = opts.session || null;
    this.now = opts.now || (() => Date.now());
    this.timeoutMs = (opts.timeoutMs === undefined) ? DEFAULT_XID_TIMEOUT_MS : opts.timeoutMs;
    this.maxInFlight = (opts.maxInFlight === undefined) ? 4 : opts.maxInFlight;
    this.onEvent = opts.onEvent || null;
    this._xid = (opts.xidStart === undefined) ? 1 : (opts.xidStart & 0xFFFF);
    this._pending = new Map();       // xid -> {resolve, reject, op, at, timer, retry}
    this.stats = {
      requests: 0, responses: 0, timeouts: 0, unmatchedResponses: 0,
      malformed: 0, sendBlocked: 0, retries: 0, bytesWritten: 0, bytesRead: 0,
    };
  }

  _emit(type, info) {
    if (this.onEvent) {
      try { this.onEvent(type, info || {}); } catch (e) { /* 上层回调不得影响协议 */ }
    }
  }

  get pending() { return this._pending.size; }

  _nextXid() {
    // xid 0 保留给"无事务"的帧（§5.3：请求/响应配对），真实事务从 1 开始
    for (let i = 0; i < 0xFFFF; i++) {
      const x = this._xid;
      this._xid = (this._xid + 1) & 0xFFFF;
      if (this._xid === 0) this._xid = 1;
      if (!this._pending.has(x)) return x;
    }
    throw new FileError('EBUSY', 'xid 空间耗尽（有 ' + this._pending.size + ' 个请求未完成）');
  }

  /**
   * 发一个请求并等它的响应（按 xid 配对）。
   * @returns {Promise<object>} 解析后的 FILE 消息（status 非 0 时**不抛**，由调用方判断）
   */
  async request(op, { status = 0, offset = 0, body = null, timeoutMs = this.timeoutMs } = {}) {
    if (!this.session || typeof this.session.sendFile !== 'function') {
      throw new FileError('ENOTSUP', 'FileClient 未绑定 LinkSession（session.sendFile 缺失）');
    }
    const xid = this._nextXid();
    const p = new Promise((resolve, reject) => {
      const entry = { resolve, reject, op, at: this.now(), timer: null };
      entry.timer = setTimeout(() => {
        this._pending.delete(xid);
        this.stats.timeouts++;
        this._emit('timeout', { xid, op, opName: FILE_OP_NAMES[op] });
        reject(new FileError('ETIMEDOUT',
          FILE_OP_NAMES[op] + ' xid=' + xid + ' 超过 ' + timeoutMs + ' ms 无响应' +
          '（可靠通道仍在 200 ms 重传；若一直没有响应，检查设备侧 FILE 服务是否实现）'));
      }, timeoutMs);
      this._pending.set(xid, entry);
    });

    // 窗口满 / 链路未 up：退避重试（绝不静默丢请求）
    const t0 = this.now();
    for (;;) {
      let sent = false;
      try {
        sent = this.session.sendFile(op, { status, xid, offset, body }) === true;
      } catch (e) {
        sent = false;
      }
      if (sent) {
        this.stats.requests++;
        this._emit('request', { xid, op, opName: FILE_OP_NAMES[op], offset, bodyLength: body ? toBytes(body).length : 0 });
        return p;
      }
      this.stats.sendBlocked++;
      if (this.now() - t0 >= DEFAULT_SEND_RETRY_MS) {
        const entry = this._pending.get(xid);
        if (entry) { clearTimeout(entry.timer); this._pending.delete(xid); }
        throw new FileError('EBUSY',
          FILE_OP_NAMES[op] + ' 无法发出：链路未 up 或可靠窗口已满（等了 ' + DEFAULT_SEND_RETRY_MS + ' ms）');
      }
      this._emit('send_blocked', { xid, op });
      this.stats.retries++;
      await sleep(25);
    }
  }

  /** 收到一条 FILE 通道消息（由宿主从 LinkSession 的 'message' 事件转交）。 */
  onMessage(payload) {
    const msg = decodeFileMessage(payload);
    if (msg.malformed) {
      this.stats.malformed++;
      this._emit('malformed', { length: msg.length });
      return null;
    }
    const entry = this._pending.get(msg.xid);
    if (!entry) {
      // 迟到响应 / 设备主动推送 / xid 不匹配：只计数，绝不错配给别的请求
      this.stats.unmatchedResponses++;
      this._emit('unmatched', { xid: msg.xid, op: msg.op, opName: msg.opName });
      return msg;
    }
    clearTimeout(entry.timer);
    this._pending.delete(msg.xid);
    this.stats.responses++;
    this._emit('response', { xid: msg.xid, op: msg.op, opName: msg.opName, status: msg.status, offset: msg.offset, bodyLength: msg.body.length });
    entry.resolve(msg);
    return msg;
  }

  /** 取消所有未完成请求（断开/重连时必须调用，避免旧会话的 Promise 泄漏到新会话） */
  abortAll(reason) {
    for (const [xid, e] of this._pending) {
      clearTimeout(e.timer);
      e.reject(new FileError('EABORT', '请求被取消（' + (reason || 'abort') + '） xid=' + xid));
    }
    this._pending.clear();
  }

  /** 复位 xid 与统计（收到 HELLO、链路重连后调用：xid 空间是每会话的） */
  reset(reason) {
    this.abortAll(reason || 'reset');
    this._xid = 1;
    this._emit('reset', { reason: reason || 'reset' });
  }

  _check(msg, expectOps) {
    if (msg.status !== STATUS.OK) {
      throw new FileError('ESTATUS',
        (msg.opName || 'OP' + msg.op) + ' 返回 ' + msg.statusName + ' (0x' + msg.status.toString(16) + ')',
        msg.status);
    }
    if (expectOps && expectOps.indexOf(msg.op) < 0) {
      throw new FileError('EPROTO',
        '响应 op=' + msg.op + '（' + msg.opName + '）不是期望的 ' +
        expectOps.map((o) => FILE_OP_NAMES[o]).join('/'));
    }
    return msg;
  }

  /* ---------------- 面向 UI 的高层 API（status 非 0 一律抛 FileError） ---------------- */

  /** 列目录 → `{entries:[{name,size,isDir}], truncatedTail}` */
  async list(path = '/') {
    const msg = await this.request(FILE_OP.LIST_REQ, { body: pathBytes(path) });
    this._check(msg, [FILE_OP.LIST_RSP]);
    const r = decodeListEntries(msg.body);
    if (r.truncatedTail) this._emit('list_truncated', { tail: r.truncatedTail });
    return r;
  }

  /** 打开文件 → `{handle, size, isDir}` */
  async open(path, mode = OPEN_MODE.READ) {
    const body = concatBytes([Uint8Array.of(mode & 0xFF), pathBytes(path)]);
    const msg = await this.request(FILE_OP.OPEN_REQ, { body });
    this._check(msg, [FILE_OP.OPEN_RSP]);
    return decodeOpenRsp(msg.body);
  }

  /** 读一段（offset 在 8 字节头里，handle/length 在 body 里） */
  async read(handle, offset, length = MAX_READ_DATA) {
    const n = Math.min(length, MAX_READ_DATA);
    const msg = await this.request(FILE_OP.READ_REQ, { offset: offset >>> 0, body: encodeReadReq({ handle, length: n }) });
    this._check(msg, [FILE_OP.READ_RSP]);
    this.stats.bytesRead += msg.body.length;
    return msg.body;
  }

  /** 写一段（≤ 1012 B） */
  async write(handle, offset, data) {
    const d = toBytes(data);
    if (d.length > MAX_WRITE_DATA) throw new FileError('EINVAL', 'write 单次数据超过 ' + MAX_WRITE_DATA + ' B');
    const msg = await this.request(FILE_OP.WRITE_REQ, { offset: offset >>> 0, body: encodeWriteReq({ handle }, d) });
    this._check(msg, [FILE_OP.WRITE_RSP]);
    const r = decodeWriteRsp(msg.body);
    this.stats.bytesWritten += r.written;
    return r.written;
  }

  async close(handle) {
    const msg = await this.request(FILE_OP.CLOSE_REQ, { body: encodeHandle(handle) });
    this._check(msg, [FILE_OP.CLOSE_RSP]);
    return true;
  }

  /** 删除（设备原样回 op=10，按 xid 配对） */
  async remove(path) {
    const msg = await this.request(FILE_OP.DELETE, { body: pathBytes(path) });
    return this._check(msg, [FILE_OP.DELETE, 0x80 | FILE_OP.DELETE]);
  }

  async mkdir(path) {
    const msg = await this.request(FILE_OP.MKDIR, { body: pathBytes(path) });
    return this._check(msg, [FILE_OP.MKDIR, 0x80 | FILE_OP.MKDIR]);
  }

  async rename(from, to) {
    const msg = await this.request(FILE_OP.RENAME, { body: encodeRenameBody(from, to) });
    return this._check(msg, [FILE_OP.RENAME, 0x80 | FILE_OP.RENAME]);
  }

  /**
   * 整文件下载：OPEN → 反复 READ（offset 递增，EOF = OK 且 0 字节）→ CLOSE。
   * @param {{onProgress?:function({loaded,total}), chunkSize?:number}} [opts]
   * @returns {Promise<{name:string, bytes:Uint8Array, size:number}>}
   */
  async readFile(path, opts = {}) {
    const chunkSize = Math.min(opts.chunkSize || MAX_READ_DATA, MAX_READ_DATA);
    const info = await this.open(path, OPEN_MODE.READ);
    if (info.isDir) {
      await this.close(info.handle).catch(() => {});
      throw new FileError('EINVAL', path + ' 是目录，不能按文件下载');
    }
    const parts = [];
    let loaded = 0;
    try {
      for (;;) {
        const chunk = await this.read(info.handle, loaded, chunkSize);
        if (chunk.length === 0) break;
        parts.push(chunk);
        loaded += chunk.length;
        if (opts.onProgress) opts.onProgress({ loaded, total: info.size });
        if (info.size && loaded >= info.size) break;
        if (chunk.length < chunkSize) break; // 短包 = 结束
      }
    } finally {
      await this.close(info.handle).catch(() => {});
    }
    return { name: path, bytes: concatBytes(parts), size: loaded };
  }

  /**
   * 整文件上传：OPEN(mode=1) → 分块 WRITE（offset 递增）→ CLOSE。
   * 分块 ≤ 1012 B（单帧上限）；每块一个 xid，串行等待响应（语法简单且对窗口友好）。
   */
  async writeFile(path, bytes, opts = {}) {
    const data = toBytes(bytes);
    const info = await this.open(path, OPEN_MODE.WRITE);
    let written = 0;
    try {
      while (written < data.length) {
        const chunk = data.subarray(written, Math.min(written + MAX_WRITE_DATA, data.length));
        const n = await this.write(info.handle, written, chunk);
        if (n <= 0) throw new FileError('EIO', 'WRITE 在 offset=' + written + ' 写入 0 字节');
        written += n;
        if (opts.onProgress) opts.onProgress({ written, total: data.length });
      }
    } finally {
      await this.close(info.handle).catch(() => {});
    }
    return written;
  }
}

/** 通道号常量再导出，避免宿主重复 import proto.js（FILE = 2） */
export const FILE_CHANNEL = CHANNEL.FILE;

export default FileClient;
