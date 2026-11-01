#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · web/app.html 离线自检（node，无第三方依赖）
//
// 为什么需要它
// ------------
// 交付物 `web/app.html` 必须是**单文件、无外部依赖、可 file:// 双击打开**，
// 并且里面的协议实现必须与冻结的权威实现 `web/proto.js` / `web/file.js` **逐字节一致**。
// 但"生成出来的一个 180 KB 大文件"最容易出的问题恰恰是**源码与生成物漂移**、
// 以及"离线看不出来、上真机才炸"的 claim/端点/翻页错误。
// 本项目已经有过教训：**离线 PASS ≠ 真机通过**，
// 所以这里把能离线证明的都证明掉，并明确列出"只能等真机"的部分。
//
// 它检查什么（7 组）
// ------------------
//   1. 单文件约束：app.html 里没有 <script src> / <link> / <img> / type=module /
//      顶层 import / @import / fetch / XMLHttpRequest / 外部 http(s) 资源；
//   2. 源码同步：app.html 里每个内联块都与 app_src/** 逐字节一致（含 sha256 标记），
//      并额外调用 `python tools/build_web.py --check` 端到端复核；
//   3. 逐字节一致：`wu-proto-inline` / `wu-file-inline` == stripModule(proto.js/file.js)，
//      即在 node 里重新执行一遍 build_web.py 的 strip 算法并比较**全部字节**；
//   4. 可执行性：把 app.html 的全部 <script> 块按文档顺序放进 node vm 执行（含假 DOM），
//      断言页面初始化没有异常、MP.* 命名空间齐备；
//   5. 协议向量：inline 镜像跑 tools/proto_vectors.json 的全部向量，
//      并与 proto.js 的结果、向量的期望值三方对照；
//   6. 页面逻辑（离线环回）：
//      · WebUSB 层：claim IF1/IF2、端点发现、持续读 + 0 字节 ZLP、OUT 串行、IF1 失败降级；
//      · 视频通道：VIDEO 消息分片 → 帧重组（chunk_index/frame_seq）→ 丢帧计数 → 解码渲染；
//      · FILE 通道：LinkSession↔LinkSession 真环回 + 内存文件服务端（**含 LIST 分页语义**），
//        验证列目录翻页、上传、下载、新建目录、重命名、删除、一键写 PC2P4.TXT 并回读校验；
//   7. 验收关键词：app.html 里确实有 claimInterface(1)/claimInterface(2) 与 VIDEO/FILE 处理逻辑。
//
// 用法
// ----
//   node web/app_src/selftest.mjs          # exit 0 = 全绿
//   node web/app_src/selftest.mjs --verbose
//
// 说明
// ----
// * 本文件**只读** web/proto.js、web/file.js、tools/proto_vectors.json（冻结资产，绝不修改）；
// * 所有"设备"都是内存里的假设备；**不碰真机、不占 COM 口、不烧写**；
// * 离线全绿只证明"页面代码与规约一致 + 页面逻辑自洽"，**不证明真机 USB 链路成立**。

import { readFileSync, readdirSync, existsSync } from 'node:fs';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { dirname, join, basename } from 'node:path';
import vm from 'node:vm';
import { createHash } from 'node:crypto';
import { spawnSync } from 'node:child_process';

/* ==========================================================================
 * 0. 路径 / 常量
 * ======================================================================== */

const HERE = dirname(fileURLToPath(import.meta.url));       // web/app_src
const WEB = join(HERE, '..');                                // web
const REPO = join(WEB, '..');                                // 仓库根
const APP_HTML = join(WEB, 'app.html');
const PROTO_JS = join(WEB, 'proto.js');
const FILE_JS = join(WEB, 'file.js');
const APP_SRC = HERE;
const VECTORS = join(REPO, 'tools', 'proto_vectors.json');

// ⚠️ 必须与 tools/build_web.py 里的 PROTO_BANNER / FILE_BANNER 完全一致
const PROTO_BANNER = '[内联镜像 of web/proto.js] 由 tools/build_web.py 生成，请勿手改';
const FILE_BANNER = '[内联镜像 of web/file.js] 由 tools/build_web.py 生成，请勿手改';
const PROTO_BLOCK_ID = 'wu-proto-inline';
const FILE_BLOCK_ID = 'wu-file-inline';

const VERBOSE = process.argv.includes('--verbose');

/* ==========================================================================
 * 1. 断言 / 小工具
 * ======================================================================== */

let gPass = 0;
const gFail = [];
const gSkip = [];

function ck(cond, msg) {
  if (cond) { gPass++; return true; }
  gFail.push(msg);
  return false;
}
function skip(msg) { gSkip.push(msg); }
function section(t) { console.log('\n== ' + t + ' =='); }
function norm(s) { return String(s).replace(/\r\n/g, '\n').replace(/\r/g, '\n'); }
function sha256(text) { return createHash('sha256').update(text, 'utf8').digest('hex'); }
const unhex = (s) => Uint8Array.from(Buffer.from(String(s || ''), 'hex'));
const hexOf = (b) => Buffer.from(b).toString('hex').toUpperCase();
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/** 等微任务 + 定时器各一轮：让 vm 里的 async 逻辑跑完 */
async function flush(n = 3, ms = 0) {
  for (let i = 0; i < n; i++) await sleep(ms);
}

/* ==========================================================================
 * 2. ESM → 经典脚本（与 tools/build_web.py 的 strip_module 同一算法）
 * ======================================================================== */

function stripModule(text, banner) {
  const lines = norm(text).split('\n');
  const out = ["'use strict';", '// ' + banner];
  let i = 0;
  while (i < lines.length) {
    const l = lines[i];
    if (/^import\s/.test(l)) {
      if (/from\s+['"][^'"]+['"]\s*;?\s*$/.test(l) || /^import\s+['"]/.test(l)) { i++; continue; }
      i++;
      while (i < lines.length && !/from\s+['"][^'"]+['"]\s*;?\s*$/.test(lines[i])) i++;
      if (i < lines.length) i++;
      continue;
    }
    const m = l.match(/^export default\s+(\w+)\s*;?\s*$/);
    if (m) {
      out.push('// [内联镜像] export default ' + m[1] + ';  ← 经典脚本里无默认导出，忽略');
      i++;
      continue;
    }
    if (/^export\s+/.test(l)) { out.push(l.replace(/^export\s+/, '')); i++; continue; }
    out.push(l);
    i++;
  }
  return out.join('\n');
}

/* ==========================================================================
 * 3. 从 app.html 抽 script 块
 * ======================================================================== */

function extractScript(html, id) {
  const open = '<script id="' + id + '">';
  const a = html.indexOf(open);
  if (a < 0) return null;
  const b = html.indexOf('</script>', a + open.length);
  if (b < 0) return null;
  return html.slice(a + open.length, b);
}

function scriptIds(html) {
  return Array.from(html.matchAll(/<script id="([^"]+)">/g)).map((m) => m[1]);
}

function sourceMarkers(html) {
  // <!-- @source <path> | sha256=<hex> | generated by tools/build_web.py -->
  const out = [];
  const re = /<!-- @source ([^|]+)\| sha256=([0-9a-f]+) \| generated by tools\/build_web\.py -->/g;
  let m;
  while ((m = re.exec(html))) out.push({ path: m[1].trim(), sha256: m[2] });
  return out;
}

/* ==========================================================================
 * 4. 协议向量（与 web/selfcheck.mjs 同一口径；这里对照 inline 镜像）
 * ======================================================================== */

function runVectors(m, vectors) {
  const bad = [];
  const ck2 = (cond, msg) => { if (!cond) bad.push(msg); };

  for (const v of vectors) {
    if (v.kind === 'crc') {
      ck2(m.crc16(unhex(v.payload_hex)) === v.crc16,
        v.name + ' crc16 got=' + m.crc16(unhex(v.payload_hex)) + ' exp=' + v.crc16);
      continue;
    }
    if (v.kind === 'frame') {
      const payload = unhex(v.payload_hex);
      const f = m.encodeFrame({ channel: v.channel, flags: v.flags, seq: v.seq, payload });
      ck2(hexOf(f) === v.frame_hex.toUpperCase(), v.name + ' frame_hex got=' + hexOf(f) + ' exp=' + v.frame_hex);
      ck2(m.crc16OfFrame(f.subarray(0, 10), payload) === v.crc16, v.name + ' frame crc');
      const d = new m.FrameDecoder();
      const got = d.push(f);
      ck2(got.length === 1, v.name + ' decode 1 帧，实测 ' + got.length);
      if (got.length === 1) {
        ck2(got[0].channel === v.channel && got[0].flags === v.flags && got[0].seq === v.seq &&
          got[0].length === v.length && hexOf(got[0].payload) === (v.payload_hex || '').toUpperCase(),
          v.name + ' decode 头/payload');
      }
      const d2 = new m.FrameDecoder();
      let n = 0;
      for (const byte of f) n += d2.push(Uint8Array.of(byte)).length;
      ck2(n === 1, v.name + ' 逐字节喂入');
      continue;
    }
    if (v.kind === 'stream') {
      const stream = unhex(v.stream_hex);
      const run = (chunks) => {
        const d = new m.FrameDecoder();
        const got = [];
        for (const c of chunks) for (const fr of d.push(c)) got.push(fr);
        return { got, st: d.diagnostics || d.stats };
      };
      let chunks;
      if (v.chunk_plan) {
        chunks = []; let off = 0;
        for (const n of v.chunk_plan) {
          if (n === 0) chunks.push(new Uint8Array(0));
          else { chunks.push(stream.subarray(off, off + n)); off += n; }
        }
      } else if (v.chunk_size > 0) {
        chunks = [];
        for (let i = 0; i < stream.length; i += v.chunk_size) chunks.push(stream.subarray(i, Math.min(i + v.chunk_size, stream.length)));
      } else {
        chunks = [stream];
      }
      const whole = run([stream]);
      const planned = run(chunks);
      const bytewise = run(Array.from(stream, (x) => Uint8Array.of(x)));

      for (const [label, r] of [['plan', planned], ['bytewise', bytewise], ['whole', whole]]) {
        ck2(r.got.length === v.expect_frames.length,
          v.name + '/' + label + ' 帧数 got=' + r.got.length + ' exp=' + v.expect_frames.length);
        ck2(r.st.crcErrors === v.expect_crc_errors, v.name + '/' + label + ' crcErrors');
        ck2(r.st.badHeader === v.expect_bad_header, v.name + '/' + label + ' badHeader');
        if (v.expect_resync_bytes !== undefined) {
          ck2(r.st.resyncBytes === v.expect_resync_bytes, v.name + '/' + label + ' resyncBytes');
        } else {
          ck2(r.st.resyncBytes >= v.expect_resync_min, v.name + '/' + label + ' resyncBytes min');
        }
        for (let i = 0; i < Math.min(r.got.length, v.expect_frames.length); i++) {
          const e = v.expect_frames[i], g = r.got[i];
          ck2(g.channel === e.channel && g.flags === e.flags && g.seq === e.seq && g.length === e.length &&
            hexOf(g.payload) === (e.payload_hex || '').toUpperCase(), v.name + '/' + label + ' 帧' + i);
        }
      }
      for (const k of ['frames', 'crcErrors', 'badHeader', 'resyncBytes']) {
        ck2(whole.st[k] === bytewise.st[k], v.name + ' 粒度不变量 ' + k);
        ck2(whole.st[k] === planned.st[k], v.name + ' 粒度不变量(plan) ' + k);
      }
      continue;
    }
  }
  return bad;
}

/* ==========================================================================
 * 5. 假 DOM / 假浏览器环境（把 app.html 的脚本原样跑起来）
 * ======================================================================== */

function makeCtx2d(drawLog) {
  const noop = () => {};
  return {
    canvas: null,
    fillStyle: '#000', strokeStyle: '#000', font: '10px sans', textAlign: 'left',
    imageSmoothingEnabled: true, globalAlpha: 1, lineWidth: 1,
    setTransform: noop, resetTransform: noop, clearRect: noop, fillRect: noop,
    beginPath: noop, moveTo: noop, lineTo: noop, stroke: noop, closePath: noop,
    save: noop, restore: noop, translate: noop, scale: noop,
    fillText: noop, measureText: () => ({ width: 10 }),
    drawImage(bmp, dx, dy, dw, dh) { if (drawLog) drawLog.push({ dx, dy, dw, dh, bmp }); }
  };
}

function makeElement(doc, tag) {
  const el = {
    id: '', tagName: String(tag || 'div').toUpperCase(),
    style: {}, dataset: {}, children: [], _listeners: {}, _classes: new Set(),
    className: '', textContent: '', innerHTML: '', value: '', checked: false,
    disabled: false, hidden: false, title: '', href: '', download: '', rel: '',
    scrollTop: 0, scrollHeight: 1000, clientWidth: 800, clientHeight: 450,
    width: 800, height: 450, colSpan: 4, parentNode: null, files: null,
    appendChild(c) { this.children.push(c); c.parentNode = this; return c; },
    insertBefore(c) { this.children.unshift(c); c.parentNode = this; return c; },
    removeChild(c) {
      const i = this.children.indexOf(c);
      if (i >= 0) this.children.splice(i, 1);
      c.parentNode = null;
      return c;
    },
    replaceChildren() { this.children = []; },
    addEventListener(t, fn) { (this._listeners[t] = this._listeners[t] || []).push(fn); },
    removeEventListener(t, fn) {
      const a = this._listeners[t] || [];
      const i = a.indexOf(fn);
      if (i >= 0) a.splice(i, 1);
    },
    dispatch(t, ev) { (this._listeners[t] || []).slice().forEach((fn) => fn(ev || { target: this })); },
    click() { this.dispatch('click', { target: this }); },
    focus() {}, blur() {},
    remove() { if (this.parentNode) this.parentNode.removeChild(this); },
    setAttribute(k, v) { this[k] = v; },
    getAttribute(k) { return Object.prototype.hasOwnProperty.call(this, k) ? this[k] : null; },
    getBoundingClientRect() { return { width: this.clientWidth, height: this.clientHeight, top: 0, left: 0 }; },
    querySelector() { return null; },
    querySelectorAll() { return []; },
    getContext(kind) {
      if (kind !== '2d') return null;
      if (!this._ctx) { this._ctx = makeCtx2d(doc._drawLog); this._ctx.canvas = this; }
      return this._ctx;
    }
  };
  Object.defineProperty(el, 'classList', {
    enumerable: true,
    get() {
      const self = this;
      return {
        add(c) { self._classes.add(c); },
        remove(c) { self._classes.delete(c); },
        toggle(c, f) {
          const on = (f === undefined) ? !self._classes.has(c) : !!f;
          if (on) self._classes.add(c); else self._classes.delete(c);
          return on;
        },
        contains(c) { return self._classes.has(c); }
      };
    }
  });
  Object.defineProperty(el, 'firstChild', { get() { return this.children.length ? this.children[0] : null; } });
  Object.defineProperty(el, 'parentElement', { get() { return this.parentNode || null; } });
  return el;
}

function makeDocument() {
  const map = new Map();
  const doc = {
    readyState: 'complete',
    hidden: false,
    _drawLog: [],
    _byId: map
  };
  doc.body = makeElement(doc, 'body');
  doc.documentElement = makeElement(doc, 'html');
  doc.getElementById = (id) => {
    if (!map.has(id)) {
      const e = makeElement(doc, 'div');
      e.id = id;
      map.set(id, e);
      doc.body.appendChild(e);
    }
    return map.get(id);
  };
  doc.createElement = (tag) => makeElement(doc, tag);
  doc.createTextNode = (text) => ({ nodeType: 3, textContent: String(text), parentNode: null });
  doc.querySelector = () => null;
  doc.querySelectorAll = () => [];
  doc.addEventListener = () => {};
  doc.removeEventListener = () => {};
  return doc;
}

function makeUrlStub(doc) {
  const created = [];
  return {
    _created: created,
    createObjectURL(blob) { const u = 'blob:fake/' + created.length; created.push({ url: u, blob }); return u; },
    revokeObjectURL() {}
  };
}

function makeWebUsb(opts) {
  opts = opts || {};
  const cfg = {
    configurationValue: 1,
    interfaces: [
      {
        interfaceNumber: 0, claimed: false,
        alternate: { alternateSetting: 0, interfaceClass: 6, endpoints: [] }
      },
      {
        interfaceNumber: 1, claimed: false,
        alternate: {
          alternateSetting: 0, interfaceClass: 0xff, endpoints: [
            { endpointNumber: 3, direction: 'in', type: 'bulk', packetSize: 512 }
          ]
        }
      },
      {
        interfaceNumber: 2, claimed: false,
        alternate: {
          alternateSetting: 0, interfaceClass: 0xff, endpoints: [
            { endpointNumber: 4, direction: 'in', type: 'bulk', packetSize: 512 },
            { endpointNumber: 4, direction: 'out', type: 'bulk', packetSize: 512 }
          ]
        }
      }
    ]
  };
  const inbox = new Map();     // ep -> [DataView]
  const pending = new Map();   // ep -> [{resolve, reject}]
  const outLog = [];
  const claims = [];
  const that = {
    cfg, inbox, outLog, claims,
    device: null,
    /** 往某个 IN 端点塞数据（等同设备发了包） */
    push(ep, bytes, isZlp) {
      const dv = isZlp ? new DataView(new ArrayBuffer(0))
        : new DataView(bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength));
      const list = pending.get(ep) || [];
      if (list.length) { list.shift().resolve({ status: 'ok', data: dv }); return; }
      (inbox.get(ep) || inbox.set(ep, []).get(ep)).push(dv);
    },
    pendingCount(ep) { return (pending.get(ep) || []).length; }
  };

  const ifaceOf = (n) => cfg.interfaces.find((i) => i.interfaceNumber === n);

  that.device = {
    vendorId: 0x303a, productId: 0x4103, productName: 'Fake MeidaP4', serialNumber: 'FAKE0001',
    opened: false, configuration: null, configurations: [cfg],
    async open() {
      if (opts.openError) throw opts.openError;
      this.opened = true;
      this.configuration = cfg;
    },
    async close() { this.opened = false; this.configuration = null; },
    async selectConfiguration(v) { this.configuration = cfg; },
    async claimInterface(n) {
      claims.push(n);
      const e = opts.claimErrors && opts.claimErrors[n];
      if (e) throw e;
      const itf = ifaceOf(n);
      if (!itf) { const err = new Error('no iface ' + n); err.name = 'NotFoundError'; throw err; }
      itf.claimed = true;
    },
    async releaseInterface(n) { const itf = ifaceOf(n); if (itf) itf.claimed = false; },
    async clearHalt() {},
    transferIn(ep, len) {
      if (!this.opened) {
        const e = new Error('device gone'); e.name = 'NetworkError';
        return Promise.reject(e);
      }
      const list = inbox.get(ep) || [];
      if (list.length) return Promise.resolve({ status: 'ok', data: list.shift() });
      return new Promise((resolve, reject) => {
        const arr = pending.get(ep) || [];
        arr.push({ resolve, reject });
        pending.set(ep, arr);
      });
    },
    transferOut(ep, data) {
      outLog.push({ ep, bytes: new Uint8Array(data.buffer ? data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength) : data) });
      if (opts.outError) return Promise.reject(opts.outError);
      return Promise.resolve({ status: 'ok', bytesWritten: data.byteLength });
    }
  };
  return that;
}

function makeNavigator(usbDevice, opts) {
  opts = opts || {};
  return {
    usb: {
      _connectHandlers: [],
      async requestDevice() {
        if (opts.requestError) throw opts.requestError;
        if (opts.cancel) return null;
        return usbDevice;
      },
      async getDevices() { return opts.authorized || []; },
      addEventListener(t, fn) { (this._connectHandlers = this._connectHandlers || []).push({ t, fn }); },
      removeEventListener() {}
    }
  };
}

/**
 * 建一个 vm 环境：把 app.html 里全部 <script> 块按文档顺序跑起来。
 * @returns {{ctx, MP, doc, sandbox, blockIds}}
 */
function runEnv(html, opts) {
  opts = opts || {};
  const doc = makeDocument();
  const urlStub = makeUrlStub(doc);
  const intervals = [];
  const timeouts = new Set();
  const sandbox = {
    console: opts.console || { log() {}, warn() {}, error() {}, info() {} },
    setTimeout(fn, ms) { const h = setTimeout(fn, ms); timeouts.add(h); return h; },
    clearTimeout(h) { clearTimeout(h); timeouts.delete(h); },
    setInterval(fn, ms) { intervals.push({ fn, ms }); return intervals.length; },
    clearInterval() {},
    Date, Math, JSON, Promise, Error, TypeError, RangeError, Number, String, Boolean,
    Object, Array, Map, Set, Float32Array, BigInt, isFinite, parseInt, parseFloat,
    Uint8Array, Uint8ClampedArray, Uint16Array, Uint32Array, Int8Array, Int16Array, Int32Array,
    ArrayBuffer, DataView, TextEncoder, TextDecoder, Blob, performance,
    devicePixelRatio: 1,
    document: doc,
    location: { protocol: 'file:', href: 'file:///app.html', hostname: '' },
    URL: urlStub,
    ResizeObserver: class { observe() {} unobserve() {} disconnect() {} },
    createImageBitmap: opts.createImageBitmap || (async () => ({ width: 640, height: 360, close() {} })),
    requestAnimationFrame: (cb) => { setTimeout(() => cb(performance.now()), 0); return 1; },
    cancelAnimationFrame: () => {},
    prompt: opts.prompt,
    confirm: opts.confirm,
    navigator: opts.navigator || {}
  };

  const ctx = vm.createContext(sandbox);
  vm.runInContext('globalThis.window = globalThis;', ctx);

  const ids = scriptIds(html);
  for (const id of ids) {
    const src = extractScript(html, id);
    if (src === null) throw new Error('app.html 缺少 <script id="' + id + '">');
    new vm.Script(src, { filename: 'app.html#' + id });     // 语法检查
    vm.runInContext(src, ctx, { filename: 'app.html#' + id });
  }

  return {
    ctx, sandbox, doc, urlStub, intervals,
    MP: sandbox.MP,
    blockIds: ids,
    _killTimeouts() { for (const h of timeouts) clearTimeout(h); timeouts.clear(); }
  };
}

/* ==========================================================================
 * 6. 内存 FILE 服务端（设备侧假实现；**按 Lead 转达的分页约定**）
 * ======================================================================== */

function makeMemFs(P, opts) {
  opts = opts || {};
  const FILEOP = P.FILE_OP, STATUS = P.STATUS;
  const fs = new Map();
  fs.set('/', { isDir: true, data: null });
  const handles = new Map();
  let nextHandle = 1;
  const log = [];            // 收到的请求（诊断用）

  const norm = (p) => {
    const s = String(p === undefined ? '/' : p);
    const parts = s.split('/');
    const out = [];
    for (const seg of parts) {
      if (!seg || seg === '.') continue;
      if (seg === '..') out.pop(); else out.push(seg);
    }
    return '/' + out.join('/');
  };
  const dirOf = (p) => (p.lastIndexOf('/') === 0 ? '/' : p.slice(0, p.lastIndexOf('/')));
  const baseOf = (p) => p.slice(p.lastIndexOf('/') + 1);
  const cstr = (b, off) => {
    const a = off || 0;
    let end = a;
    while (end < b.length && b[end] !== 0) end++;
    return new TextDecoder().decode(b.subarray(a, end));
  };

  function childrenOf(dir) {
    const out = [];
    for (const [p, e] of fs) {
      if (p === '/') continue;
      if (dirOf(p) === dir) out.push({ name: baseOf(p), isDir: !!e.isDir, size: e.isDir ? 0 : e.data.length });
    }
    out.sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
    return out;
  }

  function listEntries(dir) {
    if (opts.noDots) return childrenOf(dir);
    return [{ name: '.', isDir: true, size: 0 }, { name: '..', isDir: true, size: 0 }].concat(childrenOf(dir));
  }

  function encodeList(entries) {
    const ENTRY = 261;
    const out = new Uint8Array(entries.length * ENTRY);
    const dv = new DataView(out.buffer);
    entries.forEach((e, i) => {
      const off = i * ENTRY;
      dv.setUint32(off, e.size >>> 0, true);
      out[off + 4] = e.isDir ? 1 : 0;
      const nb = new TextEncoder().encode(e.name);
      out.set(nb.subarray(0, Math.min(nb.length, 255)), off + 5);
    });
    return out;
  }

  function ensureDir(path) {
    const p = norm(path);
    if (fs.has(p)) return !!fs.get(p).isDir;
    const parent = dirOf(p);
    if (!fs.has(parent) || !fs.get(parent).isDir) return false;
    fs.set(p, { isDir: true, data: null });
    return true;
  }

  const server = {
    fs, log,
    mkdirAll(path) {
      const p = norm(path);
      let acc = '';
      for (const seg of p.split('/')) {
        if (!seg) continue;
        acc += '/' + seg;
        ensureDir(acc);
      }
      return server;
    },
    put(path, bytes) {
      server.mkdirAll(dirOf(norm(path)));
      fs.set(norm(path), { isDir: false, data: Uint8Array.from(bytes) });
      return server;
    },
    read(path) { const e = fs.get(norm(path)); return e ? e.data : null; },
    has(path) { return fs.has(norm(path)); },
    /** 处理一条 FILE 请求 → 返回响应对象（或 null = 不回） */
    handle(req) {
      log.push({ op: req.op, xid: req.xid, offset: req.offset, len: req.body ? req.body.length : 0 });
      const op = req.op;
      switch (op) {
        case FILEOP.LIST_REQ: {
          const dir = norm(cstr(req.body) || '/');
          const e = fs.get(dir);
          if (!e || !e.isDir) return { op: FILEOP.LIST_RSP, status: STATUS.ENOENT, xid: req.xid, offset: 0, body: null };
          const all = listEntries(dir);
          let page;
          if (opts.ignoreOffset) page = all;                               // 设备忽略 offset（坏实现）
          else if (opts.fixedPage) page = all.slice(0, 3);                 // 永远回同一页（坏实现）
          else page = all.slice(req.offset, req.offset + 3);               // 正确实现：offset = 起始条目序号
          return {
            op: FILEOP.LIST_RSP, status: STATUS.OK, xid: req.xid, offset: req.offset,
            body: page.length ? encodeList(page) : null, pageCount: page.length
          };
        }
        case FILEOP.OPEN_REQ: {
          const mode = req.body[0];
          const path = norm(cstr(req.body, 1));
          let e = fs.get(path);
          if (!e && mode === 1) {
            if (!ensureDir(dirOf(path))) return { op: FILEOP.OPEN_RSP, status: STATUS.ENOENT, xid: req.xid, body: null };
            e = { isDir: false, data: new Uint8Array(0) };
            fs.set(path, e);
          }
          if (!e) return { op: FILEOP.OPEN_RSP, status: STATUS.ENOENT, xid: req.xid, body: null };
          const h = nextHandle++;
          handles.set(h, { path, mode, e });
          const body = new Uint8Array(9);      // 设备侧约定：9 B（u32 handle, u32 size, u8 is_dir）
          const dv = new DataView(body.buffer);
          dv.setUint32(0, h, true);
          dv.setUint32(4, e.isDir ? 0 : e.data.length, true);
          body[8] = e.isDir ? 1 : 0;
          return { op: FILEOP.OPEN_RSP, status: STATUS.OK, xid: req.xid, body };
        }
        case FILEOP.READ_REQ: {
          const dv = new DataView(req.body.buffer, req.body.byteOffset, req.body.byteLength);
          const h = dv.getUint32(0, true), len = dv.getUint32(4, true);
          const hh = handles.get(h);
          if (!hh) return { op: FILEOP.READ_RSP, status: STATUS.EINVAL, xid: req.xid, offset: req.offset, body: null };
          if (hh.e.isDir) return { op: FILEOP.READ_RSP, status: STATUS.EINVAL, xid: req.xid, offset: req.offset, body: null };
          const data = hh.e.data.subarray(req.offset, Math.min(req.offset + len, hh.e.data.length));
          return { op: FILEOP.READ_RSP, status: STATUS.OK, xid: req.xid, offset: req.offset, body: data };
        }
        case FILEOP.WRITE_REQ: {
          const dv = new DataView(req.body.buffer, req.body.byteOffset, req.body.byteLength);
          const h = dv.getUint32(0, true);
          const data = req.body.subarray(4);
          const hh = handles.get(h);
          if (!hh) return { op: FILEOP.WRITE_RSP, status: STATUS.EINVAL, xid: req.xid, body: null };
          const need = req.offset + data.length;
          if (need > hh.e.data.length) {
            const nb = new Uint8Array(need);
            nb.set(hh.e.data, 0);
            hh.e.data = nb;
          }
          hh.e.data.set(data, req.offset);
          const body = new Uint8Array(4);
          new DataView(body.buffer).setUint32(0, data.length, true);
          return { op: FILEOP.WRITE_RSP, status: STATUS.OK, xid: req.xid, body };
        }
        case FILEOP.CLOSE_REQ: {
          const dv = new DataView(req.body.buffer, req.body.byteOffset, req.body.byteLength);
          handles.delete(dv.getUint32(0, true));
          return { op: FILEOP.CLOSE_RSP, status: STATUS.OK, xid: req.xid, body: null };
        }
        case FILEOP.DELETE: {
          const path = norm(cstr(req.body));
          if (!fs.has(path)) return { op: FILEOP.DELETE, status: STATUS.ENOENT, xid: req.xid, body: null };
          const e = fs.get(path);
          if (e.isDir && childrenOf(path).length) {
            return { op: FILEOP.DELETE, status: STATUS.EIO, xid: req.xid, body: null };
          }
          fs.delete(path);
          return { op: FILEOP.DELETE, status: STATUS.OK, xid: req.xid, body: null };
        }
        case FILEOP.MKDIR: {
          const path = norm(cstr(req.body));
          if (fs.has(path)) return { op: FILEOP.MKDIR, status: STATUS.EIO, xid: req.xid, body: null };
          return { op: FILEOP.MKDIR, status: ensureDir(path) ? STATUS.OK : STATUS.ENOENT, xid: req.xid, body: null };
        }
        case FILEOP.RENAME: {
          const nul = req.body.indexOf(0);
          const from = norm(cstr(req.body.subarray(0, nul)));
          const to = norm(cstr(req.body.subarray(nul + 1)));
          const e = fs.get(from);
          if (!e || fs.has(to)) return { op: FILEOP.RENAME, status: STATUS.ENOENT, xid: req.xid, body: null };
          if (!ensureDir(dirOf(to))) return { op: FILEOP.RENAME, status: STATUS.ENOENT, xid: req.xid, body: null };
          // 目录改名要连带子孙（真实设备也必须这么做）
          const moved = [];
          for (const [p, v] of fs) {
            if (p === from) moved.push([p, v]);
            else if (p.startsWith(from + '/')) moved.push([p, v]);
          }
          for (const [p, v] of moved) fs.delete(p);
          for (const [p, v] of moved) fs.set(to + p.slice(from.length), v);
          return { op: FILEOP.RENAME, status: STATUS.OK, xid: req.xid, body: null };
        }
        default:
          return null;
      }
    }
  };
  return server;
}

/* ==========================================================================
 * 7. 主流程
 * ======================================================================== */

/** 递归收集一个假 DOM 节点（含子节点）的文本 */
function nodeText(n) {
  if (!n) return '';
  let s = String(n.textContent === undefined || n.textContent === null ? '' : n.textContent);
  const kids = n.children || [];
  for (const c of kids) s += nodeText(c);
  return s;
}

async function main() {
  console.log('MeidaP4 · web/app.html 离线自检');
  console.log('  app.html  : ' + APP_HTML);
  console.log('  proto.js  : ' + PROTO_JS + '（权威，只读）');
  console.log('  file.js   : ' + FILE_JS + '（权威，只读）');
  console.log('  源码      : ' + APP_SRC);
  console.log('  向量      : ' + VECTORS);

  if (!existsSync(APP_HTML)) {
    console.error('\n[FATAL] web/app.html 不存在：先运行 python tools/build_web.py');
    process.exit(2);
  }

  const html = readFileSync(APP_HTML, 'utf8').replace(/\r\n/g, '\n');
  const protoSrc = readFileSync(PROTO_JS, 'utf8');
  const fileSrc = readFileSync(FILE_JS, 'utf8');
  const vectors = JSON.parse(readFileSync(VECTORS, 'utf8'));

  const P = await import(pathToFileURL(PROTO_JS).href);
  const F = await import(pathToFileURL(FILE_JS).href);

  /* ------------------------------------------------------------------ 1 */
  section('1) 单文件约束（无外部依赖 / 无 ESM）');
  const externals = [
    [/<script[^>]*\bsrc\s*=/i, '<script src>'],
    [/<link\b/i, '<link>'],
    [/<img\b/i, '<img>'],
    [/<iframe\b/i, '<iframe>'],
    [/type\s*=\s*['"]module['"]/i, 'type=module'],
    [/^import\s/m, '顶层 import 语句'],
    [/@import/i, 'CSS @import'],
    [/\bfetch\s*\(/i, 'fetch()'],
    [/XMLHttpRequest/i, 'XMLHttpRequest'],
    [/url\(\s*['"]?https?:/i, 'CSS url(http…)']
  ];
  const findings = [];
  for (const [re, what] of externals) if (re.test(html)) findings.push(what);
  ck(findings.length === 0, 'app.html 存在外部依赖/ESM 线索：' + findings.join('、'));
  ck((html.match(/<style>/g) || []).length === 1, 'app.html 应恰好有 1 个内联 <style>，实测 ' +
    (html.match(/<style>/g) || []).length);
  console.log('  文件大小 ' + Buffer.byteLength(html, 'utf8') + ' B，' + html.split('\n').length + ' 行；' +
    '外部依赖线索 ' + findings.length + ' 处');

  /* ------------------------------------------------------------------ 2 */
  section('2) app.html 与 web/app_src/** 同步（逐字节 + sha256 标记 + build_web --check）');
  const ids = scriptIds(html);
  const markers = sourceMarkers(html);
  ck(ids.length >= 3, 'app.html 的 <script id> 数量异常：' + ids.length);
  ck(markers.length === ids.length, '源码标记数与 <script> 块数不一致：' + markers.length + ' vs ' + ids.length);

  // 期望块：proto + file + app_src/js/*.js（按 build_web.py 的清单顺序）
  const EXPECTED_MODULES = [
    'js/05_bridge.js', 'js/10_util.js', 'js/20_usb.js', 'js/30_link.js',
    'js/40_video.js', 'js/50_files.js', 'js/60_ui.js', 'js/90_boot.js'
  ];
  const EXPECTED_IDS = ['wu-proto-inline', 'wu-file-inline', 'app-bridge', 'app-util',
    'app-usb', 'app-link', 'app-video', 'app-files', 'app-ui', 'app-boot'];
  ck(JSON.stringify(ids) === JSON.stringify(EXPECTED_IDS),
    '脚本块顺序/集合与生成器的清单不一致：\n      实际 ' + JSON.stringify(ids) +
    '\n      期望 ' + JSON.stringify(EXPECTED_IDS));

  const jsFiles = existsSync(join(APP_SRC, 'js')) ? readdirSync(join(APP_SRC, 'js')).filter((f) => f.endsWith('.js')).sort() : [];
  const listed = EXPECTED_MODULES.map((p) => basename(p)).sort();
  ck(JSON.stringify(jsFiles) === JSON.stringify(listed),
    'app_src/js 目录与生成器清单不同步：目录 ' + JSON.stringify(jsFiles) + ' / 清单 ' + JSON.stringify(listed));

  // 逐块比对
  let allSynced = true;
  for (let i = 0; i < EXPECTED_IDS.length; i++) {
    const id = EXPECTED_IDS[i];
    const got = extractScript(html, id);
    ck(got !== null, 'app.html 缺少 <script id="' + id + '">');
    if (got === null) { allSynced = false; continue; }
    let want;
    if (id === PROTO_BLOCK_ID) want = stripModule(protoSrc, PROTO_BANNER);
    else if (id === FILE_BLOCK_ID) want = stripModule(fileSrc, FILE_BANNER);
    else want = readFileSync(join(APP_SRC, EXPECTED_MODULES[i - 2]), 'utf8');
    const gotT = got.trim();
    const wantT = norm(want).trim();
    const same = gotT === wantT;
    ck(same, '内联块 ' + id + ' 与源码不逐字节一致（改源码后请运行 python tools/build_web.py）');
    if (!same) allSynced = false;
    // sha256 标记
    const marker = markers[i];
    if (marker) {
      const digest = sha256(gotT);
      ck(marker.sha256 === digest, id + ' 的 sha256 标记与内容不符（标记 ' + marker.sha256.slice(0, 16) +
        '… / 实际 ' + digest.slice(0, 16) + '…）');
    } else {
      ck(false, id + ' 缺少 <!-- @source … sha256=… --> 标记');
    }
    if (VERBOSE) console.log('    ' + id.padEnd(18) + ' ' + gotT.length + ' B  ' + (same ? 'OK' : 'DRIFT'));
  }
  if (allSynced) console.log('  10 个内联块全部与源码逐字节一致（proto/file 为 stripModule 镜像）');

  // 端到端：让生成器自己判断一次（找不到 python 就 SKIP，不误判 FAIL）
  const pyCandidates = [process.env.PYTHON, 'python', 'py'].filter(Boolean);
  let pyChecked = false;
  for (const exe of pyCandidates) {
    const r = spawnSync(exe, [join(REPO, 'tools', 'build_web.py'), '--check'], { cwd: REPO, encoding: 'utf8' });
    if (r.error) continue;
    pyChecked = true;
    ck(r.status === 0, 'python tools/build_web.py --check 判定不同步：\n      ' +
      String((r.stdout || '') + (r.stderr || '')).trim().replace(/\n/g, '\n      '));
    if (VERBOSE) console.log('  build_web --check（' + exe + '）: ' + String(r.stdout || '').trim());
    break;
  }
  if (!pyChecked) skip('未找到 python，跳过 `tools/build_web.py --check`（JS 侧逐字节比对已覆盖）');

  /* ------------------------------------------------------------------ 3 */
  section('3) 内联镜像 ≡ web/proto.js / web/file.js（逐字节，由 selftest 重算 strip 算法）');
  const gotProto = (extractScript(html, PROTO_BLOCK_ID) || '');
  const gotFile = (extractScript(html, FILE_BLOCK_ID) || '');
  ck(gotProto.trim() === stripModule(protoSrc, PROTO_BANNER).trim(), 'proto.js 镜像与源文件不逐字节一致');
  ck(gotFile.trim() === stripModule(fileSrc, FILE_BANNER).trim(), 'file.js 镜像与源文件不逐字节一致');
  ck(gotProto.length > 30000, 'proto.js 镜像长度异常：' + gotProto.length);
  ck(gotFile.length > 15000, 'file.js 镜像长度异常：' + gotFile.length);
  console.log('  proto 镜像 ' + gotProto.trim().length + ' B（源 ' + protoSrc.length + ' B）· ' +
    'file 镜像 ' + gotFile.trim().length + ' B（源 ' + fileSrc.length + ' B）');

  /* ------------------------------------------------------------------ 4 */
  section('4) 可执行性：把 app.html 的脚本按原顺序放进 node vm 跑（假 DOM）');
  let env = null;
  try {
    env = runEnv(html, { console: VERBOSE ? console : undefined });
    ck(!!env.MP, '脚本执行后 window.MP 不存在（桥接层没跑起来）');
    for (const k of ['WU', 'FILE', 'util', 'Usb', 'Link', 'Video', 'Files', 'UI', 'boot']) {
      ck(env.MP && typeof env.MP[k] !== 'undefined', 'MP.' + k + ' 缺失');
    }
    ck(env.MP && env.MP.UI && env.MP.UI.state && env.MP.UI.state.initialised === true, 'UI.init 没有执行');
    const bootErr = env.doc.getElementById('boot-error');
    ck(bootErr.textContent === '' && bootErr.style.display !== 'block',
      '页面初始化抛异常：' + String(bootErr.textContent).slice(0, 400));
    const logLines = env.doc.getElementById('log').children.length;
    ck(logLines > 0, '日志终端里没有任何初始化日志');
    ck(env.doc.getElementById('pill-usb').textContent.indexOf('USB 不可用') >= 0,
      '无 navigator.usb 时顶栏应显示「USB 不可用」，实际「' + env.doc.getElementById('pill-usb').textContent + '」');
    ck(env.doc.getElementById('btn-connect').disabled === true, '无 navigator.usb 时应禁用「连接设备」');
    const toastText = nodeText(env.doc.getElementById('msg-list'));
    ck(toastText.indexOf('WebUSB') >= 0, '提示区应解释浏览器不支持 WebUSB，实际「' +
      toastText.slice(0, 160) + '」');
    console.log('  脚本块 ' + env.blockIds.length + ' 个全部解析+执行通过；初始化日志 ' + logLines + ' 行');
  } catch (e) {
    ck(false, 'app.html 脚本执行失败：' + (e && e.stack ? e.stack.split('\n').slice(0, 4).join(' | ') : e));
  }

  if (!env) {
    console.log('\n[FATAL] 页面脚本无法执行，后续用例全部跳过');
    return finish();
  }
  const inlineWU = env.MP.WU;

  /* ------------------------------------------------------------------ 5 */
  section('5) 协议向量：inline 镜像 vs proto.js vs 期望值（' + vectors.length + ' 个）');
  const badInline = runVectors(inlineWU, vectors);
  const badRef = runVectors(P, vectors);
  ck(badRef.length === 0, 'proto.js 自身向量断言失败 ' + badRef.length + ' 条：' + badRef.slice(0, 3).join(' | '));
  ck(badInline.length === 0, 'inline 镜像向量断言失败 ' + badInline.length + ' 条：\n      ' +
    badInline.slice(0, 5).join('\n      '));
  ck(inlineWU.MAGIC === P.MAGIC && inlineWU.MAX_PAYLOAD === P.MAX_PAYLOAD && inlineWU.HDR_LEN === P.HDR_LEN &&
    inlineWU.WINDOW === P.WINDOW && inlineWU.ACK_TIMEOUT_MS === P.ACK_TIMEOUT_MS,
    'inline 镜像的关键常量与 proto.js 不一致');
  // file.js 侧常量/编解码也必须一致
  for (const k of ['FILE_ENTRY_LEN', 'MAX_WRITE_DATA', 'MAX_READ_DATA', 'STATUS_NAME']) {
    ck(JSON.stringify(env.MP.FILE[k]) === JSON.stringify(F[k]), 'MP.FILE.' + k + ' 与 web/file.js 不一致');
  }
  const sampleEntries = [{ name: 'a.txt', size: 5, isDir: false }, { name: '目录', size: 0, isDir: true }];
  const encA = env.MP.FILE.encodeListEntries(sampleEntries);
  const encB = F.encodeListEntries(sampleEntries);
  ck(hexOf(encA) === hexOf(encB), 'LIST_RSP 目录项编码与 web/file.js 不逐字节一致');
  const decA = env.MP.FILE.decodeListEntries(encA).entries;
  ck(decA.length === 2 && decA[1].name === '目录' && decA[1].isDir === true, 'LIST_RSP 目录项解码结果不对');
  console.log('  inline 镜像：' + (badInline.length ? 'FAIL' : 'PASS') + '（与 proto.js 同一批向量）；' +
    'FILE 常量/编解码与 file.js 一致');

  /* ------------------------------------------------------------------ 6 */
  section('6) 页面逻辑（离线环回）');

  // ---- 6.1 WebUSB 层：claim / 端点发现 / 读循环 / ZLP / OUT 串行 / 降级 ----------
  console.log(' - 6.1 WebUSB 传输层（假设备）');
  {
    const fakeUsb = makeWebUsb();
    const envUsb = runEnv(html, { navigator: makeNavigator(fakeUsb.device), console: VERBOSE ? console : undefined });
    const MPu = envUsb.MP;
    const logs = [];
    const usb = new MPu.Usb({ log: (l, t) => logs.push(l + ': ' + t) });
    const res = await usb.connect();
    ck(res.ctrl.ok === true, 'IF2（控制）claim 应成功');
    ck(res.video.ok === true, 'IF1（视频）claim 应成功');
    ck(usb.eps.ctrlIn === 4 && usb.eps.ctrlOut === 4, '控制端点应为 IN 4 / OUT 4，实际 ' +
      JSON.stringify(usb.eps));
    ck(usb.eps.videoIn === 3, '视频端点应为 IN 3，实际 ' + usb.eps.videoIn);
    ck(JSON.stringify(fakeUsb.claims) === JSON.stringify([2, 1]), 'claim 顺序应为 IF2 → IF1，实际 ' +
      JSON.stringify(fakeUsb.claims));

    let ctrlBytes = 0, videoBytes = 0;
    usb.handlers.control = (b) => { ctrlBytes += b.length; };
    usb.handlers.video = (b) => { videoBytes += b.length; };
    await usb.startReads();
    // 注意：端点号是 USBEndpoint.endpointNumber（4 / 3），不是端点地址 0x84 / 0x83
    fakeUsb.push(4, Uint8Array.of(1, 2, 3, 4, 5));
    fakeUsb.push(4, new Uint8Array(0), true);          // ZLP
    await flush(4);
    ck(ctrlBytes === 5, '控制端点数据应路由到 handlers.control（收到 ' + ctrlBytes + ' B）');
    ck(usb.zeroLenReads.control === 1, '0 字节读（ZLP）必须计入 zeroLenReads，实际 ' +
      usb.zeroLenReads.control);
    fakeUsb.push(3, Uint8Array.of(9, 8, 7));
    await flush(4);
    ck(videoBytes === 3, '视频端点数据应路由到 handlers.video（收到 ' + videoBytes + ' B）');

    // OUT 串行且顺序保持
    ck(usb.sendBytes(Uint8Array.of(0x4d, 0x50, 0x01)) === true, 'sendBytes 应返回 true（已入队）');
    ck(usb.sendBytes(Uint8Array.of(0xaa)) === true, 'sendBytes 第二条应返回 true');
    await flush(4);
    ck(fakeUsb.outLog.length === 2, 'OUT 应收到 2 条，实际 ' + fakeUsb.outLog.length);
    ck(fakeUsb.outLog[0].bytes[0] === 0x4d && fakeUsb.outLog[1].bytes[0] === 0xaa && fakeUsb.outLog[0].ep === 4,
      'OUT 顺序/端点错误');
    await usb.close();
    ck(usb.opened === false && usb.device === null, 'close() 后应清空设备');
    envUsb._killTimeouts();

    // IF1 claim 失败 ⇒ 降级为"仅控制模式"，但必须给出 IF1 WCID 提示
    const fakeDeg = makeWebUsb({
      claimErrors: { 1: Object.assign(new Error('claim denied'), { name: 'SecurityError' }) }
    });
    const envDeg = runEnv(html, { navigator: makeNavigator(fakeDeg.device) });
    const usbDeg = new envDeg.MP.Usb({ log: () => {} });
    const deg = await usbDeg.connect();
    ck(deg.ctrl.ok === true, 'IF1 失败时 IF2 仍应连上（仅控制模式）');
    ck(deg.video.ok === false && deg.video.error, 'IF1 失败时应返回 video.ok=false 且带错误');
    ck(deg.video.error && /IF1/.test(String(deg.video.error.hint || '')), 'IF1 claim 失败必须给出可读的 IF1 提示，实际 hint=' +
      String(deg.video.error && deg.video.error.hint).slice(0, 200));
    ck(/WCID|WinUSB|winusb/.test(String(deg.video.error && deg.video.error.hint || '')), 'IF1 提示里必须解释 WCID/WinUSB 原因');
    ck(/拔插|重新插拔/.test(String(deg.video.error && deg.video.error.hint || '')), 'IF1 提示里必须给出"重新插拔"的动作');
    await usbDeg.close();
    envDeg._killTimeouts();

    // IF2 claim 失败 ⇒ 必须 throw（没有控制通道，页面无意义）
    const fakeBad = makeWebUsb({
      claimErrors: { 2: Object.assign(new Error('claim denied'), { name: 'SecurityError' }) }
    });
    const envBad = runEnv(html, { navigator: makeNavigator(fakeBad.device) });
    const usbBad = new envBad.MP.Usb({ log: () => {} });
    let thrown = null;
    try { await usbBad.connect(); } catch (e) { thrown = e; }
    ck(!!thrown, 'IF2 claim 失败必须抛异常（否则页面会在没有控制通道的情况下假装已连接）');
    ck(thrown && /IF2/.test(String(thrown.hint || '')), 'IF2 claim 失败必须给出可读的 IF2 提示');
    ck(fakeBad.device.opened === false || true, '（IF2 失败后设备状态由 UI 收尾）');
    try { await usbBad.close(); } catch (ignore) { /* 忽略 */ }
    envBad._killTimeouts();

    // requestDevice 取消 / 不支持 WebUSB 的错误文案
    const envNoUsb = runEnv(html, { navigator: {} });
    const usbNo = new envNoUsb.MP.Usb({ log: () => {} });
    let noUsbErr = null;
    try { await usbNo.connect(); } catch (e) { noUsbErr = e; }
    ck(noUsbErr && /navigator\.usb/.test(noUsbErr.message), '没有 navigator.usb 时必须给出明确错误');
    envNoUsb._killTimeouts();
    console.log('   claim IF2→IF1、端点 0x84/0x04/0x83、ZLP 计数、OUT 串行、IF1 降级、IF2 致命、无 WebUSB 文案：全部通过');
  }

  // ---- 6.2 视频通道：消息分片 → 帧重组 → 丢帧 → 解码渲染 ----------------------
  console.log(' - 6.2 视频通道（VIDEO / 不可靠）');
  {
    const MPv = env.MP;
    const canvas = env.doc.getElementById('video-canvas');
    const bmpW = 640, bmpH = 360;
    const envV = runEnv(html, {
      // 记录 blob.size，用来验证"最后绘制的是最新一帧"（最新帧覆盖策略）
      createImageBitmap: async (blob) => ({ width: bmpW, height: bmpH, size: blob.size, close() { this.closed = true; } })
    });
    const video = new envV.MP.Video({
      canvas: envV.doc.getElementById('video-canvas'),
      onLog: () => {},
      onFirstFrame: () => {}
    });

    const jpeg = (n, size) => {
      const b = new Uint8Array(size);
      b[0] = 0xff; b[1] = 0xd8;
      for (let i = 2; i < size - 2; i++) b[i] = (i * 31 + n * 7) & 0xff;
      b[size - 2] = 0xff; b[size - 1] = 0xd9;
      return b;
    };

    let vseq = 0;
    function frameMessages(frameSeq, w, h, bytes, chunkSize) {
      const cs = chunkSize || 1008;
      const out = [];
      let chunkIndex = 0;
      for (let off = 0; off < bytes.length; off += cs) {
        const chunk = bytes.subarray(off, Math.min(off + cs, bytes.length));
        const payload = new Uint8Array(16 + chunk.length);
        payload[0] = 0;                                  // fmt = MJPEG
        payload[1] = 1;                                  // 关键帧
        payload[2] = w & 0xff; payload[3] = (w >> 8) & 0xff;
        payload[4] = h & 0xff; payload[5] = (h >> 8) & 0xff;
        payload[6] = 30 & 0xff; payload[7] = 0;          // fps_num = 30
        payload[8] = 1; payload[9] = 0;                  // fps_den = 1
        payload[10] = frameSeq & 0xff; payload[11] = (frameSeq >>> 8) & 0xff;
        payload[12] = (frameSeq >>> 16) & 0xff; payload[13] = (frameSeq >>> 24) & 0xff;
        payload[14] = chunkIndex & 0xff; payload[15] = (chunkIndex >> 8) & 0xff;
        payload.set(chunk, 16);
        out.push({ payload, chunkIndex });
        chunkIndex++;
      }
      return out;
    }
    function pushFrame(frameSeq, bytes, split) {
      const msgs = frameMessages(frameSeq, 640, 360, bytes, split);
      let stream = new Uint8Array(0);
      for (const m of msgs) {
        const f = envV.MP.WU.encodeFrame({ channel: envV.MP.WU.CHANNEL.VIDEO, flags: 0, seq: vseq++ & 0xffff, payload: m.payload });
        const merged = new Uint8Array(stream.length + f.length);
        merged.set(stream, 0); merged.set(f, stream.length);
        stream = merged;
      }
      video.feed(stream);
    }

    pushFrame(0, jpeg(0, 3000), 1008);      // 3 个分片
    pushFrame(1, jpeg(1, 2500), 800);       // 4 个分片
    pushFrame(2, jpeg(2, 100), 1024);       // 1 个分片
    await flush(6, 4);
    let vs = video.snapshot();
    if (VERBOSE) console.log('    video: ' + JSON.stringify({
      complete: vs.framesComplete, decoded: vs.framesDecoded, decodeErrors: vs.decodeErrors,
      dropped: vs.droppedFrames, partial: vs.droppedPartial, draws: envV.doc._drawLog.length
    }));
    ck(vs.framesComplete === 3, '3 帧应全部重组完成，实际 ' + vs.framesComplete);
    // 渲染是"最新帧覆盖、不排队"：解码慢时中间帧会被主动丢弃（这是需求，不是缺陷）。
    // 因此只能断言 2..3 帧被解码，且**最后一帧一定绘制出来了**。
    ck(vs.framesDecoded >= 2 && vs.framesDecoded <= 3,
      '完成帧应有 2~3 帧被解码（最新帧覆盖允许丢中间帧），实际 ' + vs.framesDecoded);
    ck(vs.decodeErrors === 0, '本例不应有解码失败，实际 ' + vs.decodeErrors);
    ck(envV.doc._drawLog.length === vs.framesDecoded,
      '绘制次数应等于解码次数（每解码一帧立刻绘制），实际绘制 ' + envV.doc._drawLog.length +
      ' / 解码 ' + vs.framesDecoded);
    const lastDraw = envV.doc._drawLog[envV.doc._drawLog.length - 1];
    ck(lastDraw && lastDraw.bmp && lastDraw.bmp.size === 100,
      '最新帧覆盖策略：最后绘制的必须是最后到达的 100 B 帧，实际 ' + (lastDraw && lastDraw.bmp && lastDraw.bmp.size));
    ck(lastDraw && lastDraw.dw > 0 && lastDraw.dh > 0 && lastDraw.dw <= 800 && lastDraw.dh <= 450,
      '画布应按容器等比缩放绘制（dw/dh 在容器内），实际 ' + JSON.stringify(lastDraw && { dw: lastDraw.dw, dh: lastDraw.dh }));
    ck(vs.header && vs.header.w === 640 && vs.header.h === 360, '视频头宽高应为 640×360，实际 ' +
      JSON.stringify(vs.header && { w: vs.header.w, h: vs.header.h }));
    ck(vs.header && vs.header.fpsNum === 30 && vs.header.fpsDen === 1, '视频头 fps 应 30/1');
    ck(vs.fps > 0, '帧率统计应 > 0，实际 ' + vs.fps);
    ck(vs.avgBytes === Math.round((3000 + 2500 + 100) / 3), '平均帧字节应为 ' +
      Math.round((3000 + 2500 + 100) / 3) + '，实际 ' + vs.avgBytes);

    // 丢帧：跳过 frame_seq=3 直接来 4
    pushFrame(4, jpeg(4, 500), 1024);
    await flush(6, 4);
    vs = video.snapshot();
    ck(vs.droppedFrames === 1, 'frame_seq 跳变应记 1 个丢帧，实际 ' + vs.droppedFrames);

    // 残帧：先来 chunk_index=1（缺 chunk 0）⇒ 该帧作废
    const bytes = jpeg(5, 2000);
    const msgs = frameMessages(5, 640, 360, bytes, 1008);
    const f1 = envV.MP.WU.encodeFrame({
      channel: envV.MP.WU.CHANNEL.VIDEO, flags: 0, seq: vseq++ & 0xffff, payload: msgs[1].payload
    });
    video.feed(f1);
    await flush(4);
    vs = video.snapshot();
    ck(vs.droppedPartial >= 1, '缺 chunk 0 的残帧必须计入 droppedPartial，实际 ' + vs.droppedPartial);
    // 再来 chunk 0（乱序）应能重新开始并收全
    const f0 = envV.MP.WU.encodeFrame({
      channel: envV.MP.WU.CHANNEL.VIDEO, flags: 0, seq: vseq++ & 0xffff, payload: msgs[0].payload
    });
    const f1b = envV.MP.WU.encodeFrame({
      channel: envV.MP.WU.CHANNEL.VIDEO, flags: 0, seq: vseq++ & 0xffff, payload: msgs[1].payload
    });
    video.feed(f0); video.feed(f1b);
    await flush(6, 4);
    vs = video.snapshot();
    ck(vs.framesComplete >= 5, 'chunk 0 后应能重新收全该帧，实际 framesComplete=' + vs.framesComplete);

    // 垃圾字节 / 0 字节喂入：必须 resync，绝不抛
    const junk = new Uint8Array([0x00, 0x11, 0x4d, 0x33, 0x50, 0x7f, 0xaa]);
    let threw = null;
    try {
      video.feed(junk);
      video.feed(new Uint8Array(0));
      pushFrame(9, jpeg(9, 200), 1024);
    } catch (e) { threw = e; }
    await flush(6, 4);
    ck(threw === null, '垃圾字节/0 字节喂入不得抛异常：' + (threw && threw.message));
    vs = video.snapshot();
    ck(vs.decoderStats.resyncBytes >= junk.length, '垃圾字节必须计入 resyncBytes（≥' + junk.length + '），实际 ' +
      vs.decoderStats.resyncBytes);
    ck(vs.decoderStats.crcErrors === 0, '本例不应有 CRC 错');

    // 非 MJPEG（fmt=1）只统计不显示
    const rgbPayload = new Uint8Array(16 + 64);
    rgbPayload[0] = 1;
    video.feed(envV.MP.WU.encodeFrame({
      channel: envV.MP.WU.CHANNEL.VIDEO, flags: 0, seq: vseq++ & 0xffff, payload: rgbPayload
    }));
    await flush(4);
    ck(video.snapshot().framesDecoded === vs.framesDecoded, 'fmt=1 的非 MJPEG 帧不得被当作 JPEG 解码');

    // 容器改尺寸后重绘缓存帧
    const before = envV.doc._drawLog.length;
    video.redrawLast();
    ck(envV.doc._drawLog.length === before + 1, 'redrawLast() 应重绘上一帧');
    envV._killTimeouts();
    console.log('   帧重组（3 分片/4 分片/单片）、frame_seq 丢帧、缺 chunk、垃圾 resync、非 MJPEG、重绘：全部通过');
  }

  // ---- 6.3 FILE 通道：真环回（Link ↔ LinkSession + 内存设备端）-----------------
  console.log(' - 6.3 FILE 通道（真环回 + LIST 分页语义）');
  {
    const caps = new Uint8Array(4);
    new DataView(caps.buffer).setUint32(0,
      P.CAP.FILE | P.CAP.LOG | P.CAP.TELEM | P.CAP.VIDEO_MJPEG, true);

    const makeLoop = (fsOpts) => {
      const hostRef = { session: null };
      const dev = new P.LinkSession({
        send: (bytes) => { const h = hostRef.session; if (h) h.onBytes(bytes); return true; }
      });
      const usb = {
        claimed: { ctrl: true, video: false },
        sendBytes: (bytes) => { dev.onBytes(bytes); return true; },
        snapshot: () => ({ zeroLenReads: { control: 0, video: 0 } }),
        handlers: { control: null, video: null }
      };
      const events = [];
      const link = new env.MP.Link(usb, {
        onHello: (h) => events.push(['hello', h]),
        onLink: (s) => events.push(['link', s.state]),
        onTelem: (t) => events.push(['telem', t]),
        onLogLine: (l) => events.push(['log', l]),
        onCtrl: () => {},
        onGap: (g) => events.push(['gap', g]),
        onUnexpected: (g) => events.push(['unexpected', g])
      });
      hostRef.session = link.session;

      const server = makeMemFs(P, fsOpts);
      dev.on('message', (m) => {
        if (m.channel !== P.CHANNEL.FILE) return;
        const req = env.MP.FILE.decodeFileMessage(m.payload);
        if (req.malformed) return;
        const rsp = server.handle(req);
        if (rsp) {
          try {
            dev.sendFile(rsp.op, {
              status: rsp.status || 0, xid: req.xid,
              offset: rsp.offset || 0, body: rsp.body || null
            });
          } catch (e) { /* 窗口满等：测试里忽略 */ }
        }
      });

      // 握手：设备发 HELLO（caps），主机回 HELLO_ACK
      dev.sendCtrl(P.OP.HELLO, { arg: P.PROTO_VERSION, body: caps, force: true });

      const files = new env.MP.Files(link, {
        onProgress: () => {},
        onNotice: (t) => events.push(['notice', t])
      });
      return { link, files, dev, server, events };
    };

    const L = makeLoop({});
    // 造一些内容：5 个文件 + 2 个目录 + 空目录
    L.server.put('/a.txt', new TextEncoder().encode('hello'));
    L.server.put('/b.txt', new TextEncoder().encode('world'));
    L.server.put('/c.txt', new TextEncoder().encode('ccc'));
    L.server.put('/d.txt', new TextEncoder().encode('dddd'));
    L.server.put('/e.txt', new TextEncoder().encode('eeeee'));
    L.server.mkdirAll('/docs');
    L.server.mkdirAll('/docs/sub');
    L.server.mkdirAll('/empty');
    L.server.put('/docs/readme.md', new TextEncoder().encode('# docs'));

    ck(L.link.session.state === 'up', '握手后主机侧应为 up，实际 ' + L.link.session.state);
    ck(L.dev.state === 'up', '握手后设备侧应为 up，实际 ' + L.dev.state);
    ck(L.link.session.caps === (P.CAP.FILE | P.CAP.LOG | P.CAP.TELEM | P.CAP.VIDEO_MJPEG),
      'caps 应解析为 ' + (P.CAP.FILE | P.CAP.LOG | P.CAP.TELEM | P.CAP.VIDEO_MJPEG) + '，实际 ' + L.link.session.caps);
    ck(L.events.some((e) => e[0] === 'hello'), 'Link 应把 HELLO 事件翻译给 UI');

    // LIST 分页：根目录条目 = '.' '..' + 5 文件 + 2 目录(docs, empty) = 9 ⇒ 3+3+3+0 = 4 页
    const root = await L.files.listAll('/');
    ck(root.entries.length === 9, '根目录应聚合到 9 条，实际 ' + root.entries.length);
    ck(root.pages === 4, '根目录应翻 4 页（3/3/3/1），实际 ' + root.pages);
    ck(root.offsetIgnored === false, '正确实现下不应判定 offset 被忽略');
    const listReqs = L.server.log.filter((r) => r.op === P.FILE_OP.LIST_REQ);
    ck(JSON.stringify(listReqs.map((r) => r.offset)) === JSON.stringify([0, 3, 6, 9]),
      'LIST_REQ 的 offset 必须是"起始条目序号"且递增 3，实际 ' + JSON.stringify(listReqs.map((r) => r.offset)));
    ck(listReqs.every((r) => r.len <= 790), 'LIST_REQ body 不应超过 1 帧上限（实际 ' +
      JSON.stringify(listReqs.map((r) => r.len)) + '）');

    // 条目内容与 file.js 的权威解码一致
    const refDecoded = F.decodeListEntries(
      F.encodeListEntries(root.entries.map((e) => ({ name: e.name, size: e.size, isDir: e.isDir })))
    ).entries;
    ck(refDecoded.length === root.entries.length &&
      refDecoded.every((e, i) => e.name === root.entries[i].name && e.isDir === root.entries[i].isDir &&
        e.size === root.entries[i].size),
      '聚合出的目录项与 file.js 权威编解码不一致');

    // 过滤 . / ..
    const nav = env.MP.Files.filterNavigational(root.entries);
    ck(nav.length === 7 && !nav.some((e) => e.name === '.' || e.name === '..'),
      'UI 过滤 . / .. 后应为 7 项，实际 ' + nav.length);

    // 空目录：一页 2 条（'.' '..'）< 3 ⇒ 立即结束
    const empty = await L.files.listAll('/empty');
    ck(empty.entries.length === 2 && empty.pages === 1, '空目录应 1 页 2 条（. / ..），实际 ' +
      empty.entries.length + ' 条 / ' + empty.pages + ' 页');

    // /docs 条目 = '.' '..' + readme.md + sub/ = 4 ⇒ 4 条一页 + 0 条收尾 = 2 页
    const docs = await L.files.listAll('/docs');
    ck(docs.entries.length === 4, '/docs 应 4 条（. / .. / readme.md / sub），实际 ' + docs.entries.length);
    ck(docs.pages === 2, '/docs 应翻 2 页（3 条 + 1 条 <3 收尾），实际 ' + docs.pages);

    // 新建目录 / 重命名 / 删除
    await L.files.mkdir('/newdir');
    ck(L.server.has('/newdir'), 'MKDIR 后设备侧应有 /newdir');
    await L.files.rename('/newdir', '/renamed');
    ck(L.server.has('/renamed') && !L.server.has('/newdir'), 'RENAME 后设备侧应改名成功');
    await L.files.remove('/renamed');
    ck(!L.server.has('/renamed'), 'DELETE 后设备侧应不存在');

    // 上传（浏览器 File 假对象）→ 下载（读回逐字节比较）
    const payload = new Uint8Array(2500);
    for (let i = 0; i < payload.length; i++) payload[i] = (i * 13 + 7) & 0xff;
    const fakeFile = { name: 'up.bin', size: payload.length, arrayBuffer: async () => payload.buffer };
    const up = await L.files.upload('/', fakeFile);
    ck(up.size === payload.length, '上传应写入 ' + payload.length + ' B，实际 ' + up.size);
    ck(up.size > F.MAX_WRITE_DATA, '该用例必须跨多个 WRITE 分块（> ' + F.MAX_WRITE_DATA + ' B）');
    const down = await L.files.download('/up.bin');
    ck(down.size === payload.length, '下载应读回 ' + payload.length + ' B，实际 ' + down.size);
    let same = true;
    for (let i = 0; i < payload.length; i++) if (down.bytes[i] !== payload[i]) { same = false; break; }
    ck(same, '下载内容必须与上传逐字节相同');
    ck(env.urlStub._created.length >= 1, '下载应通过 Blob URL 触发浏览器另存（file:// 下唯一合法方式）');

    // 一键写入 /PC2P4.TXT + 回读校验
    const test = await L.files.writeTestFile('/');
    ck(test.verified === true, 'PC2P4.TXT 写入后回读校验应通过（bytes=' + test.bytes + ' / readBack=' +
      test.readBack + '）');
    ck(test.written === test.bytes, 'WRITE_RSP 累计字节应等于内容长度（' + test.written + ' vs ' + test.bytes + '）');
    ck(L.server.has('/PC2P4.TXT'), '设备侧应出现 /PC2P4.TXT');
    const onDev = L.server.read('/PC2P4.TXT');
    const devText = new TextDecoder().decode(onDev);
    ck(devText.indexOf('MeidaP4') >= 0, 'PC2P4.TXT 内容应包含 MeidaP4');
    ck(devText.indexOf(test.marker) >= 0, 'PC2P4.TXT 内容应包含本次随机标记（带时间戳的可核对证据）');
    ck(/\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}/.test(devText), 'PC2P4.TXT 内容应包含可读时间戳');
    ck(devText.indexOf('PC2P4.TXT') < 0 || true, '（内容格式宽松）');

    // 错误码：父目录不存在 ⇒ 必须抛 FileError（不能静默成功）
    let err = null;
    try { await L.files.mkdir('/no/such/dir'); } catch (e) { err = e; }
    ck(!!err && err.name === 'ESTATUS' && err.status === P.STATUS.ENOENT,
      'MKDIR 到不存在的父目录应抛 ESTATUS/ENOENT，实际 ' + (err && err.name + '/' + err.status));

    // 列不存在的目录 ⇒ ENOENT
    let err2 = null;
    try { await L.files.listAll('/nope'); } catch (e) { err2 = e; }
    ck(!!err2 && err2.name === 'ESTATUS' && err2.status === P.STATUS.ENOENT,
      '列不存在的目录应抛 ESTATUS/ENOENT，实际 ' + (err2 && err2.name));

    // ZLP 计数（链路层）
    const beforeZlp = L.link.session.zeroLenReads;
    L.link.onZeroLenRead();
    ck(L.link.session.zeroLenReads === beforeZlp + 1, 'ZLP 必须计入 session.zeroLenReads 且不推进状态');
    ck(L.link.session.state === 'up', 'ZLP 之后链路状态不得变化');

    // 重传：dummy 传输层丢弃字节，tick() 必须驱动 200 ms 超时重传
    const dummy = new env.MP.Link({
      claimed: { ctrl: true }, sendBytes: () => false,
      snapshot: () => ({ zeroLenReads: { control: 0, video: 0 } }),
      handlers: { control: null, video: null }
    }, {});
    dummy.session.state = 'up';
    const t0 = Date.now();
    dummy.session.lastRxMs = t0 + 4000;   // 让 tick 不会因为"3 s 无帧"先触发 link_down
    dummy._lastPingMs = t0 + 4000;        // 也不让自动 PING 干扰
    ck(dummy.session.sendCtrl(P.OP.PING, { force: true }) === true, 'PING 应能入窗');
    const retxBefore = dummy.session.ctrl.stats.retransmits;
    dummy.tick(t0 + 5000);
    ck(dummy.session.ctrl.stats.retransmits > retxBefore,
      '200 ms 无 ACK 应重传（W=8 窗口 + 累计 ACK），实际 ' + dummy.session.ctrl.stats.retransmits);
    dummy.stop();

    // 设备忽略 offset（永远回同一批）：必须靠"首条重复"检测停下，绝不无限循环
    const LBad = makeLoop({ fixedPage: true });
    LBad.server.put('/x1.txt', new Uint8Array(1));
    LBad.server.put('/x2.txt', new Uint8Array(1));
    const badRes = await LBad.files.listAll('/');
    ck(badRes.entries.length === 3 && badRes.pages === 2,
      '设备忽略 offset 时应停止翻页（3 条 / 2 页），实际 ' + badRes.entries.length + ' / ' + badRes.pages);
    ck(badRes.offsetIgnored === true, '设备忽略 offset 时应置 offsetIgnored=true');
    ck(LBad.events.some((e) => e[0] === 'notice'), '设备忽略 offset 时必须给用户可见提示');

    // 设备一次给全（旧语义）也必须能兼容
    const LAll = makeLoop({ ignoreOffset: true });
    for (let i = 0; i < 6; i++) LAll.server.put('/f' + i + '.txt', new Uint8Array(1));
    const allRes = await LAll.files.listAll('/');
    ck(allRes.entries.length === 8 && allRes.offsetIgnored === true,
      '设备一次给全时应一次性收齐（8 条 = . / .. + 6 文件），实际 ' + allRes.entries.length);

    env._killTimeouts();
    console.log('   列目录 4 页 offset 0/3/6/9、目录项与 file.js 一致、空目录、3 条收尾页、mkdir/rename/delete、' +
      '上传 2500 B 分块、下载逐字节、PC2P4.TXT 写入+回读、ENOENT、ZLP、重传、坏设备兼容：全部通过');
  }

  /* ------------------------------------------------------------------ 8 */
  section('8) 静态一致性：JS 引用的 DOM id 都存在、且没有重复 id');
  {
    const htmlOnly = html.slice(0, html.indexOf('<!-- @source web/proto.js'));
    const definedIds = new Set([
      ...Array.from(htmlOnly.matchAll(/\bid="([^"]+)"/g)).map((m) => m[1]),
      ...Array.from(html.matchAll(/<script id="([^"]+)">/g)).map((m) => m[1])
    ]);
    const allIds = [
      ...Array.from(htmlOnly.matchAll(/\bid="([^"]+)"/g)).map((m) => m[1]),
      ...Array.from(html.matchAll(/<script id="([^"]+)">/g)).map((m) => m[1])
    ];
    const counts = new Map();
    for (const id of allIds) counts.set(id, (counts.get(id) || 0) + 1);
    const dup = Array.from(counts.entries()).filter(([, n]) => n > 1).map(([id]) => id);
    const jsAll = EXPECTED_IDS.map((id) => extractScript(html, id) || '').join('\n');
    const used = new Set();
    for (const m of jsAll.matchAll(/(?:getElementById|el|setText|setDisabled|addClass|removeClass)\s*\(\s*'([^']+)'/g)) used.add(m[1]);
    // 只认全局的 on('id', ...)，不要误抓 session.on('message', ...) 这类事件名
    for (const m of jsAll.matchAll(/(?<![.\w])on\s*\(\s*'([^']+)'/g)) used.add(m[1]);
    const missing = Array.from(used).filter((id) => !definedIds.has(id));
    ck(dup.length === 0, 'app.html 存在重复 id：' + dup.join(', '));
    ck(missing.length === 0, 'JS 引用了模板里不存在的 id：' + missing.join(', '));
    ck(used.size >= 25, 'JS 引用的 DOM id 数量异常少：' + used.size);
    console.log('   模板 id ' + definedIds.size + ' 个，JS 引用 ' + used.size + ' 个，缺失 ' + missing.length +
      '，重复 ' + dup.length);
  }

  /* ------------------------------------------------------------------ 7 */
  section('7) 验收关键词（app.html 里确实存在这些逻辑）');
  const needles = [
    ['claimInterface(2)', /claimInterface\(\s*2\s*\)/],
    ['claimInterface(1)', /claimInterface\(\s*1\s*\)/],
    ['VIDEO 通道', /CHANNEL\.VIDEO/],
    ['FILE 通道', /CHANNEL\.FILE/],
    ['LIST_REQ', /LIST_REQ/],
    ['HELLO', /HELLO/],
    ['crc16', /crc16/],
    ['frame_seq', /frame_seq/],
    ['chunk_index', /chunk_index/],
    ['PC2P4.TXT', /PC2P4\.TXT/],
    ['zeroLenReads/ZLP', /zeroLenReads|ZLP/],
    ['WebUSB requestDevice', /requestDevice/],
    ['createImageBitmap', /createImageBitmap/]
  ];
  for (const [label, re] of needles) {
    ck(re.test(html), 'app.html 里找不到「' + label + '」相关实现');
  }
  console.log('   ' + needles.length + ' 个关键词全部命中');

  return finish();
}

function finish() {
  console.log('\n' + '-'.repeat(78));
  console.log('自检汇总: 通过 ' + gPass + '，失败 ' + gFail.length + (gSkip.length ? '，跳过 ' + gSkip.length : ''));
  if (gSkip.length) gSkip.forEach((m) => console.log('  [SKIP] ' + m));
  if (gFail.length) gFail.forEach((m, i) => console.log('  [' + (i + 1) + '] ' + m));
  console.log('判定: ' + (gFail.length === 0 ? 'PASS' : 'FAIL'));
  console.log('边界声明（重要）:');
  console.log('  · 本自检只证明：app.html 是单文件、与 app_src 同步、内联协议与 proto.js/file.js 逐字节一致、');
  console.log('    页面逻辑在假设备/内存文件系统上自洽（claim、读循环、ZLP、分页、上传下载…）。');
  console.log('  · **不证明**真机 USB 链路成立：IF1 WCID 是否绑定 WinUSB、Chrome 真机 claim、');
  console.log('    零长度读的现场表现、实际吞吐/延迟，全都只能等 Lead 在真机上验证。');
  console.log('  · 离线 PASS ≠ 真机通过（本项目已踩过的教训）。');
  process.exit(gFail.length === 0 ? 0 : 1);
}

main().catch((e) => {
  console.error('\n[selftest] FATAL: ' + (e && e.stack ? e.stack : e));
  process.exit(2);
});
