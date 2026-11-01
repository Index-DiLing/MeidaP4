// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · web/ 离线自检（node，无外部依赖）
//
// 为什么需要它：`web/index.html` 必须能在 `file://` 下双击即用，而 **Chrome 禁止 `file://`
//   页面加载外部 ES module**（实测 Chrome 155：`Failed to fetch dynamically imported module`）。
//   所以 index.html 里内联了 `proto.js` / `file.js` 的**逐字节等价副本**（经典脚本形态：
//   只去掉 import/export 语法，其余一字不改）。本文件保证这份副本永不漂移：
//
//   1) 从 index.html 抽出 `<script id="wu-proto-inline">` / `<script id="wu-file-inline">`，
//      与 `stripModule(proto.js)` / `stripModule(file.js)` **逐字节比较**；
//   2) 把抽出的内联副本当模块跑 **tools/proto_vectors.json 的全部 29 个向量**，
//      与 `proto.js` 本身的运行结果、以及向量的期望值三者对照；
//   3) 跑 file.js 的编解码 + **完整环回**（LinkSession 对 LinkSession + 内存 FILE 服务端）：
//      xid 配对、分块读写、错误码、超时、迟到响应；
//   4) 跑 probe-report §3.4 那 3 个真机坑的离线回归：**ZLP 合法**、**resync 不中止**、
//      **连接前 drain + 状态不跨会话泄漏**；
//   5) grep 自证 `index.html` 里没有任何外部资源引用（无 CDN / 外链字体 / 图片 / fetch）。
//
// 用法：
//   node web/selfcheck.mjs            # 校验（exit 0 = 全通过）
//   node web/selfcheck.mjs --sync     # 用 proto.js / file.js 重新生成 index.html 的两个内联块
//
// 说明：`web/proto.js` 是**已验证的规范实现，归 ws-webusb**，本文件只读它、绝不修改。

import { readFileSync, writeFileSync, existsSync, mkdirSync } from 'node:fs';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { dirname, join } from 'node:path';
import { tmpdir } from 'node:os';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = join(HERE, '..');
const INDEX = join(HERE, 'index.html');
const PROTO = join(HERE, 'proto.js');
const FILEJS = join(HERE, 'file.js');
const VECTORS = join(REPO, 'tools', 'proto_vectors.json');

const PROTO_BLOCK_ID = 'wu-proto-inline';
const FILE_BLOCK_ID = 'wu-file-inline';

let gPass = 0;
const gFail = [];
function ck(cond, msg) {
  if (cond) { gPass++; return true; }
  gFail.push(msg);
  return false;
}
function section(t) { console.log('\n== ' + t + ' =='); }

/* ==========================================================================
 * 1. 经典脚本镜像：只去掉 import/export 语法
 * ======================================================================== */

/**
 * 把 ESM 源码转成"经典脚本"形态：
 *   - 删除 import 语句（含多行形式）；
 *   - 删除行首的 `export ` 关键字；`export default X;` → 注释；
 *   - 顶部加 `'use strict';`（ESM 本来就是严格模式，经典脚本默认不是 —— 必须显式对齐）；
 *   - 加一行"由 selfcheck.mjs 生成、请勿手改"的横幅。
 * 其余每一个字节都保持不变（这正是等价性的可验证定义）。
 */
export function stripModule(text, banner) {
  const src = String(text).replace(/\r\n/g, '\n');
  const lines = src.split('\n');
  const out = [];
  out.push("'use strict';");
  out.push('// ' + banner);
  let i = 0;
  while (i < lines.length) {
    const l = lines[i];
    if (/^import\s/.test(l)) {
      if (/from\s+['"][^'"]+['"]\s*;?\s*$/.test(l) || /^import\s+['"]/.test(l)) { i++; continue; }
      i++; // 多行 import：吃到 `from '...'` 那一行为止
      while (i < lines.length && !/from\s+['"][^'"]+['"]\s*;?\s*$/.test(lines[i])) i++;
      if (i < lines.length) i++;
      continue;
    }
    if (/^export default\s+(\w+)\s*;?\s*$/.test(l)) {
      const name = l.match(/^export default\s+(\w+)/)[1];
      out.push('// [内联镜像] export default ' + name + ';  ← 经典脚本里无默认导出，忽略');
      i++;
      continue;
    }
    if (/^export\s+/.test(l)) {
      out.push(l.replace(/^export\s+/, ''));
      i++;
      continue;
    }
    out.push(l);
    i++;
  }
  return out.join('\n');
}

function extractBlock(html, id) {
  const open = '<script id="' + id + '">';
  const a = html.indexOf(open);
  if (a < 0) return null;
  const b = html.indexOf('</script>', a + open.length);
  if (b < 0) return null;
  return html.slice(a + open.length, b);
}

function replaceBlock(html, id, content) {
  const open = '<script id="' + id + '">';
  const a = html.indexOf(open);
  if (a < 0) throw new Error('index.html 里找不到 ' + open);
  const b = html.indexOf('</script>', a + open.length);
  if (b < 0) throw new Error('index.html 里 ' + id + ' 的 </script> 找不到');
  return html.slice(0, a + open.length) + '\n' + content.replace(/^\n+|\n+$/g, '') + '\n' + html.slice(b);
}

/* ==========================================================================
 * 2. 向量断言口径（与 C 侧 test/host/test_codec.c 同一批断言）
 * ======================================================================== */

const unhex = (s) => Uint8Array.from(Buffer.from(String(s || ''), 'hex'));
const hex = (b) => Buffer.from(b).toString('hex').toUpperCase();

function runVectors(m, vectors) {
  const bad = [];
  const info = [];
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
      ck2(hex(f) === v.frame_hex.toUpperCase(), v.name + ' frame_hex got=' + hex(f) + ' exp=' + v.frame_hex);
      ck2(m.crc16OfFrame(f.subarray(0, 10), payload) === v.crc16, v.name + ' frame crc');
      const d = new m.FrameDecoder();
      const got = d.push(f);
      ck2(got.length === 1, v.name + ' decode 1 帧');
      if (got.length === 1) {
        ck2(got[0].channel === v.channel && got[0].flags === v.flags && got[0].seq === v.seq &&
            got[0].length === v.length && hex(got[0].payload) === (v.payload_hex || '').toUpperCase(),
          v.name + ' decode 头/payload');
      }
      // 逐字节喂入必须等价
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
        ck2(r.got.length === v.expect_frames.length, v.name + '/' + label + ' 帧数 got=' + r.got.length + ' exp=' + v.expect_frames.length);
        ck2(r.st.crcErrors === v.expect_crc_errors, v.name + '/' + label + ' crcErrors got=' + r.st.crcErrors + ' exp=' + v.expect_crc_errors);
        ck2(r.st.badHeader === v.expect_bad_header, v.name + '/' + label + ' badHeader got=' + r.st.badHeader + ' exp=' + v.expect_bad_header);
        if (v.expect_resync_bytes !== undefined) {
          ck2(r.st.resyncBytes === v.expect_resync_bytes, v.name + '/' + label + ' resyncBytes got=' + r.st.resyncBytes + ' exp=' + v.expect_resync_bytes);
        } else {
          ck2(r.st.resyncBytes >= v.expect_resync_min, v.name + '/' + label + ' resyncBytes got=' + r.st.resyncBytes + ' >= ' + v.expect_resync_min);
        }
        for (let i = 0; i < Math.min(r.got.length, v.expect_frames.length); i++) {
          const e = v.expect_frames[i], g = r.got[i];
          ck2(g.channel === e.channel && g.flags === e.flags && g.seq === e.seq && g.length === e.length &&
              hex(g.payload) === (e.payload_hex || '').toUpperCase(), v.name + '/' + label + ' 帧' + i + ' 字段/payload');
        }
      }
      // 不变量：喂入粒度不得改变任何诊断量
      for (const k of ['frames', 'crcErrors', 'badHeader', 'resyncBytes']) {
        ck2(whole.st[k] === bytewise.st[k], v.name + ' 粒度不变量 ' + k + ' whole=' + whole.st[k] + ' bytewise=' + bytewise.st[k]);
        ck2(whole.st[k] === planned.st[k], v.name + ' 粒度不变量(plan) ' + k + ' whole=' + whole.st[k] + ' plan=' + planned.st[k]);
      }
      info.push('  ' + v.name.padEnd(34) + ' frames=' + whole.st.frames + ' crc=' + whole.st.crcErrors +
                ' bad=' + whole.st.badHeader + ' resync=' + whole.st.resyncBytes);
    }
  }
  return { bad, info };
}

/* ==========================================================================
 * 3. 三个真机坑的离线回归
 * ======================================================================== */

function makeClock(start = 0) {
  let t = start;
  return { now: () => t, adv: (ms) => { t += ms; return t; } };
}

/** ① ZLP：0 字节读是合法事件，必须继续读、不得中止，且不影响解码 */
function testZlp(P) {
  const clock = makeClock();
  const host = new P.LinkSession({ now: clock.now });
  const frames = [];
  host.on('message', (m) => frames.push({ ch: m.channel, len: m.payload.length }));
  const payload = P.encodeFrame({ channel: P.CHANNEL.CTRL, flags: P.FLAGS.ACK, seq: 0,
    payload: P.ctrlPayload(P.OP.PONG, 0, 0, null) });
  const telem = new Uint8Array(36);
  const tFrame = P.encodeFrame({ channel: P.CHANNEL.TELEM, flags: 0, seq: 0, payload: telem });

  // 读序列里穿插 0 字节（ZLP）：一次都不许抛、不许中止
  const reads = [new Uint8Array(0), payload, new Uint8Array(0), new Uint8Array(0),
                 tFrame.subarray(0, 20), tFrame.subarray(20), new Uint8Array(0)];
  let zlp = 0, bytes = 0, decoded = 0;
  for (const r of reads) {
    if (r.length === 0) { host.onZeroLenRead(); zlp++; continue; }
    bytes += r.length;
    decoded += host.onBytes(r).length;
  }
  ck(zlp === 4, 'ZLP: 计数应 4，实测 ' + zlp);
  ck(host.zeroLenReads === 4, 'ZLP: session.zeroLenReads 应 4，实测 ' + host.zeroLenReads);
  ck(decoded === 2, 'ZLP: 跨 0 字节读仍应解出 2 帧，实测 ' + decoded);
  ck(bytes === payload.length + tFrame.length, 'ZLP: 字节数不应把 ZLP 算进来');
  ck(host.state === 'up', 'ZLP: 收到有效帧后 state 应为 up，实测 ' + host.state);
  return 'rads=7(zlp=4) bytes=' + bytes + ' decoded=2';
}

/** ② resync：垃圾 + 坏 CRC + 合法帧混在一条流里，必须照常解出、绝不要求重连 */
function testResyncNotFatal(P) {
  const clock = makeClock();
  const host = new P.LinkSession({ now: clock.now });
  const ok = P.encodeFrame({ channel: P.CHANNEL.CTRL, flags: P.FLAGS.ACK, seq: 0, payload: P.ctrlPayload(P.OP.PONG, 0, 0, null) });
  const bad = P.encodeFrame({ channel: P.CHANNEL.CTRL, flags: P.FLAGS.ACK, seq: 1, payload: P.ctrlPayload(P.OP.PING, 0, 0, null) });
  bad[bad.length - 1] ^= 0xFF;                       // 改坏 payload → CRC 必失败
  const stream = new Uint8Array([0x00, 0x11, 0x4D, 0x33, 0x50, 0x7F, 0xAA]).length + ok.length + bad.length + ok.length;
  const all = new Uint8Array(stream);
  let off = 0;
  all.set(new Uint8Array([0x00, 0x11, 0x4D, 0x33, 0x50, 0x7F, 0xAA]), off); off += 7;
  all.set(bad, off); off += bad.length;
  all.set(ok, off); off += ok.length;
  all.set(ok, off);

  const got = host.onBytes(all);
  ck(got.length === 2, 'resync: 应解出 2 帧（坏 CRC 帧被丢），实测 ' + got.length);
  ck(host.decoder.stats.crcErrors === 1, 'resync: crcErrors 应 1，实测 ' + host.decoder.stats.crcErrors);
  ck(host.decoder.stats.resyncBytes === 7, 'resync: resyncBytes 应 7，实测 ' + host.decoder.stats.resyncBytes);
  ck(host.state === 'up', 'resync: 链路应仍为 up（绝不因坏帧重连），实测 ' + host.state);
  // 坏帧之后链路仍可用：再喂一帧照样解出
  const more = host.onBytes(ok);
  ck(more.length === 1, 'resync: 之后仍能继续解帧，实测 ' + more.length);
  return 'frames=2 crcErrors=1 resyncBytes=7 之后仍可解帧';
}

/** ③ drain + 会话隔离：排空积压后再测量必须是"对齐"的；新会话不得继承旧会话任何状态 */
function testDrainAndIsolation(P) {
  const clock = makeClock();
  const mkFrame = (seq, ch) => P.encodeFrame({
    channel: ch, flags: ch === P.CHANNEL.CTRL ? P.FLAGS.ACK : 0, seq,
    payload: ch === P.CHANNEL.TELEM ? new Uint8Array(36).fill(seq & 0xFF) : P.ctrlPayload(P.OP.PONG, 0, 0, null),
  });
  // 旧会话：故意留下"半帧"残片（批 0 缺陷 1 的形态）
  const old = new P.LinkSession({ now: clock.now });
  old.onBytes(mkFrame(1, P.CHANNEL.TELEM));
  const partial = mkFrame(2, P.CHANNEL.TELEM);
  old.onBytes(partial.subarray(0, 20));
  ck(old.decoder.diagnostics.truncated > 0, 'isolation: 旧会话应处于半帧状态（diagnostics.truncated>0）');

  // 新会话（= 页面上"重新连接/新一轮测量"）：所有状态必须从零开始
  const fresh = new P.LinkSession({ now: clock.now });
  const s = fresh.stats;
  ck(s.zeroLenReads === 0 && s.crcErrors === 0 && s.resyncBytes === 0 && s.frames === 0 && s.badHeader === 0,
    'isolation: 新会话计数器必须全 0，实测 ' + JSON.stringify({ z: s.zeroLenReads, c: s.crcErrors, r: s.resyncBytes, f: s.frames, b: s.badHeader }));
  ck(fresh.state === 'waiting_hello', 'isolation: 新会话状态应为 waiting_hello，实测 ' + fresh.state);

  // drain：把积压（3 整帧 + 1 个 0 字节 ZLP）全部读掉
  let zlp = 0;
  const backlog = [mkFrame(10, P.CHANNEL.TELEM), new Uint8Array(0), mkFrame(11, P.CHANNEL.TELEM), mkFrame(12, P.CHANNEL.TELEM)];
  let idx = 0;
  const read = () => {
    if (idx >= backlog.length) return null;
    return backlog[idx++];
  };
  let drained = 0;
  while (true) {
    const c = read();
    if (c === null) break;
    if (c.length === 0) { fresh.onZeroLenRead(); zlp++; continue; }
    drained += fresh.onBytes(c).length;
  }
  ck(drained === 3, 'drain: 应排空 3 帧，实测 ' + drained);
  ck(zlp === 1 && fresh.zeroLenReads === 1, 'drain: ZLP 应被计数且不中止');

  // 排空后"测量"：新到的第一帧必须被完整、无错位地解出
  const target = mkFrame(13, P.CHANNEL.TELEM);
  const got = fresh.onBytes(target);
  ck(got.length === 1, 'drain: 排空后第一帧应完整解出，实测 ' + got.length);
  if (got.length === 1) {
    ck(got[0].payload.length === 36 && got[0].payload[0] === 13, 'drain: 排空后第一帧 payload 必须精确对齐（无整体位移）');
  }
  return '新会话计数全 0；drain 排空 3 帧 + 1 ZLP；测量首帧对齐';
}

/* ==========================================================================
 * 4. file.js：编解码 + 内存 FILE 服务端环回
 * ======================================================================== */

function makeMemServer(FILEOP, STATUS) {
  // 内存文件系统：'/' + 路径 -> {isDir, data:Uint8Array}
  const fs = new Map([['/', { isDir: true, data: null, children: new Map() }]]);
  const norm = (p) => (String(p).startsWith('/') ? String(p) : '/' + String(p));
  const dirOf = (p) => p.slice(0, p.lastIndexOf('/')) || '/';
  const baseOf = (p) => p.slice(p.lastIndexOf('/') + 1);
  const handles = new Map();
  let nextHandle = 1;
  /** 从 NUL 结尾的字节串读出字符串（设备侧也该这么做——file.js 的路径都是 NUL 结尾的） */
  const cstr = (b) => {
    const n = b.indexOf(0);
    return new TextDecoder().decode(b.subarray(0, n < 0 ? b.length : n));
  };

  function ensureDir(path) {
    const p = norm(path);
    if (fs.has(p)) return fs.get(p).isDir;
    const parent = ensureDir(dirOf(p));
    if (!parent) return false;
    if (!fs.get(dirOf(p)).children) fs.get(dirOf(p)).children = new Map();
    fs.get(dirOf(p)).children.set(baseOf(p), p);
    fs.set(p, { isDir: true, data: null, children: new Map() });
    return true;
  }

  function list(path) {
    const p = norm(path);
    const e = fs.get(p);
    if (!e || !e.isDir) return { status: STATUS.ENOENT, entries: [] };
    const entries = [{ name: '.', size: 0, isDir: true }, { name: '..', size: 0, isDir: true }];
    for (const [, child] of (e.children || new Map())) {
      const c = fs.get(child);
      if (!c) continue; // 陈旧链接（已被 rename/delete）跳过
      entries.push({ name: baseOf(child), size: c.isDir ? 0 : c.data.length, isDir: c.isDir });
    }
    return { status: STATUS.OK, entries };
  }

  return {
    fs,
    /** 处理一条 FILE 请求 → 返回要回给主机的 {op, status, xid, offset, body}（或 null = 不回） */
    handle(req) {
      const op = req.op;
      switch (op) {
        case FILEOP.LIST_REQ: {
          const r = list(cstr(req.body) || '/');
          return { op: FILEOP.LIST_RSP, status: r.status, xid: req.xid, offset: 0, body: r.entries.length ? this.encodeList(r.entries) : null, entries: r.entries };
        }
        case FILEOP.OPEN_REQ: {
          const mode = req.body[0];
          const path = norm(cstr(req.body.subarray(1)));
          let e = fs.get(path);
          if (!e && mode === 1) { // 写模式：创建（父目录必须存在）
            if (!fs.has(dirOf(path)) || !fs.get(dirOf(path)).isDir) return { op: FILEOP.OPEN_RSP, status: STATUS.ENOENT, xid: req.xid, body: null };
            if (!fs.get(dirOf(path)).children) fs.get(dirOf(path)).children = new Map();
            fs.get(dirOf(path)).children.set(baseOf(path), path);
            e = { isDir: false, data: new Uint8Array(0) };
            fs.set(path, e);
          }
          if (!e) return { op: FILEOP.OPEN_RSP, status: STATUS.ENOENT, xid: req.xid, body: null };
          const h = nextHandle++;
          handles.set(h, { path, mode, e });
          const body = new Uint8Array(12);
          const dv = new DataView(body.buffer);
          dv.setUint32(0, h, true); dv.setUint32(4, e.isDir ? 0 : e.data.length, true); body[8] = e.isDir ? 1 : 0;
          return { op: FILEOP.OPEN_RSP, status: STATUS.OK, xid: req.xid, body };
        }
        case FILEOP.READ_REQ: {
          const dv = new DataView(req.body.buffer, req.body.byteOffset, req.body.byteLength);
          const h = dv.getUint32(0, true), len = dv.getUint32(4, true);
          const hh = handles.get(h);
          if (!hh) return { op: FILEOP.READ_RSP, status: STATUS.EINVAL, xid: req.xid, offset: req.offset, body: null };
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
          const e = fs.get(path);
          if (!e) return { op: FILEOP.DELETE, status: STATUS.ENOENT, xid: req.xid, body: null };
          fs.delete(path);
          if (fs.get(dirOf(path)) && fs.get(dirOf(path)).children) fs.get(dirOf(path)).children.delete(baseOf(path));
          return { op: FILEOP.DELETE, status: STATUS.OK, xid: req.xid, body: null };
        }
        case FILEOP.MKDIR: {
          const path = norm(cstr(req.body));
          const ok = ensureDir(path);
          return { op: FILEOP.MKDIR, status: ok ? STATUS.OK : STATUS.ENOENT, xid: req.xid, body: null };
        }
        case FILEOP.RENAME: {
          const nul = req.body.indexOf(0);
          const from = norm(cstr(req.body.subarray(0, nul)));
          const to = norm(cstr(req.body.subarray(nul + 1)));
          const e = fs.get(from);
          if (!e) return { op: FILEOP.RENAME, status: STATUS.ENOENT, xid: req.xid, body: null };
          fs.delete(from); fs.set(to, e);
          // 更新两边的父目录索引（真实设备侧也必须这么做，否则 list 会看不到改名后的文件）
          const pf = fs.get(dirOf(from));
          if (pf && pf.children) pf.children.delete(baseOf(from));
          ensureDir(dirOf(to));
          const pt = fs.get(dirOf(to));
          if (pt) { if (!pt.children) pt.children = new Map(); pt.children.set(baseOf(to), to); }
          return { op: FILEOP.RENAME, status: STATUS.OK, xid: req.xid, body: null };
        }
        default:
          return null;
      }
    },
    encodeList(entries) {
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
    },
  };
}

async function testFileClient(P, F) {
  const clock = makeClock();
  const link = { toDev: null, toHost: null };
  const host = new P.LinkSession({ send: (b) => link.toDev(b), now: clock.now });
  const dev = new P.LinkSession({ send: (b) => link.toHost(b), now: clock.now });
  link.toDev = (b) => { dev.onBytes(b); return true; };
  link.toHost = (b) => { host.onBytes(b); return true; };

  const server = makeMemServer(P.FILE_OP, P.STATUS);
  const fc = new F.FileClient({ session: host, now: clock.now, timeoutMs: 500 });
  host.on('message', (m) => { if (m.channel === P.CHANNEL.FILE) fc.onMessage(m.payload); });
  dev.on('message', (m) => {
    if (m.channel !== P.CHANNEL.FILE) return;
    const req = F.decodeFileMessage(m.payload);
    const rsp = server.handle(req);
    if (rsp) dev.sendFile(rsp.op, { status: rsp.status || 0, xid: req.xid, offset: rsp.offset || 0, body: rsp.body || null });
  });

  // 握手：设备发 HELLO（带 caps），主机回 HELLO_ACK，两侧都 up
  const caps = new Uint8Array(4);
  new DataView(caps.buffer).setUint32(0, P.CAP.FILE | P.CAP.LOG | P.CAP.TELEM, true);
  dev.sendCtrl(P.OP.HELLO, { arg: P.PROTO_VERSION, body: caps, force: true });
  ck(host.state === 'up' && dev.state === 'up', 'file: HELLO/HELLO_ACK 握手后两侧都应 up（host=' + host.state + ' dev=' + dev.state + '）');

  // 纯编解码往返
  const enc = F.encodeFileMessage(P.FILE_OP.READ_REQ, { xid: 7, offset: 4096, body: F.encodeReadReq({ handle: 3, length: 512 }) });
  const dec = F.decodeFileMessage(enc);
  ck(dec.op === P.FILE_OP.READ_REQ && dec.xid === 7 && dec.offset === 4096 && dec.body.length === 8, 'file: 8 字节头往返（op/xid/offset/body）');
  const ents = [{ name: 'a.txt', size: 5, isDir: false }, { name: 'dir', size: 0, isDir: true }];
  const le = F.decodeListEntries(F.encodeListEntries(ents));
  ck(le.entries.length === 2 && le.entries[0].name === 'a.txt' && le.entries[0].size === 5 && !le.entries[1].isDir === false, 'file: LIST_RSP 目录项编解码');
  ck(F.decodeListEntries(F.encodeListEntries(ents).subarray(0, 261 + 3)).truncatedTail === 3, 'file: LIST_RSP 尾部残片必须被识别（不越界读）');

  // mkdir → list → write → read → rename → delete
  await fc.mkdir('/docs');
  const l0 = await fc.list('/');
  ck(l0.entries.some((e) => e.name === 'docs' && e.isDir), 'file: mkdir 后 list 应出现 docs/');

  const data = new Uint8Array(3000);
  for (let i = 0; i < data.length; i++) data[i] = (i * 7) & 0xFF;
  const written = await fc.writeFile('/docs/big.bin', data);
  ck(written === data.length, 'file: writeFile 应写 ' + data.length + ' B，实测 ' + written);
  ck(written > F.MAX_WRITE_DATA, 'file: 该用例必须跨多个 WRITE 分块（> ' + F.MAX_WRITE_DATA + ' B）');

  const got = await fc.readFile('/docs/big.bin');
  ck(got.bytes.length === data.length, 'file: readFile 应读回 ' + data.length + ' B，实测 ' + got.bytes.length);
  let same = got.bytes.length === data.length;
  for (let i = 0; same && i < data.length; i++) if (got.bytes[i] !== data[i]) same = false;
  ck(same, 'file: 读回内容必须逐字节相同');

  await fc.rename('/docs/big.bin', '/docs/renamed.bin');
  const l1 = await fc.list('/docs');
  ck(l1.entries.some((e) => e.name === 'renamed.bin') && !l1.entries.some((e) => e.name === 'big.bin'), 'file: rename 生效');

  await fc.remove('/docs/renamed.bin');
  const l2 = await fc.list('/docs');
  ck(!l2.entries.some((e) => e.name === 'renamed.bin'), 'file: delete 生效');

  // 错误码：打开不存在的文件必须抛 ENOENT（不能静默成功）
  let enoent = null;
  try { await fc.open('/nope.txt'); } catch (e) { enoent = e; }
  ck(enoent && enoent.name === 'ESTATUS' && enoent.status === P.STATUS.ENOENT, 'file: 打开不存在文件应抛 ENOENT，实测 ' + (enoent && enoent.message));

  // 超时：设备故意不回 → 必须按超时拒绝，而不是永远挂着
  dev.removeAllListeners ? dev.removeAllListeners('message') : null;
  const savedHandlers = dev._handlers.get('message').slice();
  dev._handlers.set('message', []);
  let timedOut = null;
  const t0 = Date.now();
  try { await fc.list('/'); } catch (e) { timedOut = e; }
  ck(timedOut && timedOut.name === 'ETIMEDOUT', 'file: 无响应应抛 ETIMEDOUT，实测 ' + (timedOut && timedOut.name));
  ck(Date.now() - t0 < 1500, 'file: 超时应在 500 ms 量级返回（实测 ' + (Date.now() - t0) + ' ms）');
  ck(fc.stats.timeouts >= 1, 'file: 超时必须计入 stats.timeouts');
  dev._handlers.set('message', savedHandlers);

  // xid 配对：迟到的响应不得错配给后来的请求
  const before = fc.stats.unmatchedResponses;
  fc.onMessage(F.encodeFileMessage(P.FILE_OP.LIST_RSP, { status: 0, xid: 0xBEEF, body: new Uint8Array(0) }));
  ck(fc.stats.unmatchedResponses === before + 1, 'file: 未知 xid 的响应必须计入 unmatchedResponses，不得错配');

  return 'mkdir/list/write(3000B 分块)/read/rename/delete/xid 配对/ENOENT/超时/迟到响应 全部通过';
}

/* ==========================================================================
 * 5. index.html 外部引用自检
 * ======================================================================== */

function scanExternalRefs(html) {
  const findings = [];
  const lines = html.replace(/\r\n/g, '\n').split('\n');
  const patterns = [
    [/https?:\/\//i, '绝对 URL（http/https）'],
    [/\/\/cdn|cdn\.|unpkg|jsdelivr|googleapis|fonts\./i, 'CDN / 外链字体'],
    [/<link\b/i, '<link> 外部资源'],
    [/<img\b/i, '<img> 外部图片'],
    [/<iframe\b/i, '<iframe>'],
    [/@import\b/i, 'CSS @import'],
    [/\burl\(\s*['"]?(?!data:|#)/i, 'CSS url() 非 data:'],
    [/\bfetch\s*\(/i, 'fetch()'],
    [/XMLHttpRequest/i, 'XMLHttpRequest'],
    [/importScripts\s*\(/i, 'importScripts()'],
    [/\bsrc\s*=\s*["'](?!data:|#|\.\/)/i, 'src= 非相对路径'],
    [/\bhref\s*=\s*["'](?!data:|#)/i, 'href= 非 data:/锚点'],
  ];
  lines.forEach((l, i) => {
    for (const [re, what] of patterns) {
      if (re.test(l)) { findings.push('L' + (i + 1) + ' [' + what + '] ' + l.trim().slice(0, 120)); break; }
    }
  });
  return findings;
}

/* ==========================================================================
 * main
 * ======================================================================== */

async function main() {
  const args = process.argv.slice(2);
  const doSync = args.includes('--sync');

  console.log('MeidaP4 · web/ 离线自检');
  console.log('  proto.js (规范实现，只读) : ' + PROTO);
  console.log('  file.js  (FILE 通道客户端): ' + FILEJS);
  console.log('  index.html               : ' + INDEX);
  console.log('  向量                     : ' + VECTORS);

  const protoSrc = readFileSync(PROTO, 'utf8');
  const fileSrc = readFileSync(FILEJS, 'utf8');

  if (doSync) {
    if (!existsSync(INDEX)) {
      console.error('\n[--sync] index.html 不存在，无法同步。');
      process.exit(2);
    }
    let html = readFileSync(INDEX, 'utf8');
    html = replaceBlock(html, PROTO_BLOCK_ID, stripModule(protoSrc, '[内联镜像 of web/proto.js] 由 node web/selfcheck.mjs --sync 生成，请勿手改'));
    html = replaceBlock(html, FILE_BLOCK_ID, stripModule(fileSrc, '[内联镜像 of web/file.js] 由 node web/selfcheck.mjs --sync 生成，请勿手改'));
    writeFileSync(INDEX, html);
    console.log('\n[--sync] 已用 proto.js / file.js 重写 index.html 的两个内联块。');
    return;
  }

  const P = await import(pathToFileURL(PROTO).href);
  const F = await import(pathToFileURL(FILEJS).href);
  const vectors = JSON.parse(readFileSync(VECTORS, 'utf8'));

  /* --- 1. 规范实现自身的向量断言 --- */
  section('1) 规范实现 proto.js 对 tools/proto_vectors.json（' + vectors.length + ' 个向量）');
  const r1 = runVectors(P, vectors);
  for (const l of r1.info) console.log(l);
  ck(r1.bad.length === 0, 'proto.js 向量断言失败 ' + r1.bad.length + ' 条：\n      ' + r1.bad.slice(0, 6).join('\n      '));
  console.log('  proto.js: ' + (r1.bad.length ? 'FAIL ' + r1.bad.length : 'PASS'));

  /* --- 2. index.html 内联镜像等价性 + 内联镜像自己也要过全部向量 --- */
  section('2) index.html 内联镜像 ≡ proto.js / file.js（逐字节）+ 内联镜像跑全部向量');
  if (!existsSync(INDEX)) {
    ck(false, 'index.html 不存在');
  } else {
    const html = readFileSync(INDEX, 'utf8');
    const wantProto = stripModule(protoSrc, '[内联镜像 of web/proto.js] 由 node web/selfcheck.mjs --sync 生成，请勿手改').trim();
    const wantFile = stripModule(fileSrc, '[内联镜像 of web/file.js] 由 node web/selfcheck.mjs --sync 生成，请勿手改').trim();
    const gotProto = (extractBlock(html, PROTO_BLOCK_ID) || '').trim();
    const gotFile = (extractBlock(html, FILE_BLOCK_ID) || '').trim();
    ck(gotProto.length > 0, 'index.html 缺少 <script id="' + PROTO_BLOCK_ID + '">');
    ck(gotFile.length > 0, 'index.html 缺少 <script id="' + FILE_BLOCK_ID + '">');
    ck(gotProto === wantProto, 'proto.js 内联镜像与 proto.js 不逐字节一致（请跑 node web/selfcheck.mjs --sync）');
    ck(gotFile === wantFile, 'file.js 内联镜像与 file.js 不逐字节一致（请跑 node web/selfcheck.mjs --sync）');
    console.log('  内联块长度: proto=' + gotProto.length + ' file=' + gotFile.length +
                '（proto.js=' + protoSrc.length + ' file.js=' + fileSrc.length + '）');

    if (gotProto) {
      const tmp = join(tmpdir(), 'meida-web-selfcheck-' + process.pid);
      mkdirSync(tmp, { recursive: true });
      const names = Object.keys(P).filter((n) => n !== 'default');
      const modPath = join(tmp, 'inline-proto.mjs');
      writeFileSync(modPath, gotProto + '\n\nexport { ' + names.join(', ') + ' };\n');
      let inlineMod = null;
      try { inlineMod = await import(pathToFileURL(modPath).href); }
      catch (e) { ck(false, '内联镜像无法作为模块加载：' + (e && e.message)); }
      if (inlineMod) {
        const r2 = runVectors(inlineMod, vectors);
        ck(r2.bad.length === 0, '内联镜像向量断言失败 ' + r2.bad.length + ' 条：\n      ' + r2.bad.slice(0, 6).join('\n      '));
        // 与规范实现的关键常量/行为必须一致
        ck(inlineMod.MAGIC === P.MAGIC && inlineMod.MAX_PAYLOAD === P.MAX_PAYLOAD && inlineMod.HDR_LEN === P.HDR_LEN,
          '内联镜像常量与 proto.js 不一致');
        console.log('  内联镜像: ' + (r2.bad.length ? 'FAIL ' + r2.bad.length : 'PASS') + '（与 proto.js 同一批向量）');
      }
    }
  }

  /* --- 3. 三个真机坑的离线回归 --- */
  section('3) probe-report §3.4 三个真机坑的离线回归');
  console.log('  ① ZLP（0 字节 transferIn 合法）      : ' + testZlp(P));
  console.log('  ② resync（坏帧绝不中止/重连）        : ' + testResyncNotFatal(P));
  console.log('  ③ drain + 会话隔离（不跨会话泄漏）   : ' + testDrainAndIsolation(P));

  /* --- 4. file.js --- */
  section('4) file.js：编解码 + 环回（LinkSession 对 LinkSession + 内存 FILE 服务端）');
  console.log('  ' + (await testFileClient(P, F)));

  /* --- 5. index.html 外部引用 --- */
  section('5) index.html 外部资源引用自证（无 CDN / 外链字体 / 图片 / fetch）');
  if (existsSync(INDEX)) {
    const html = readFileSync(INDEX, 'utf8');
    const findings = scanExternalRefs(html);
    for (const f of findings) console.log('  [FOUND] ' + f);
    ck(findings.length === 0, 'index.html 存在 ' + findings.length + ' 处外部引用');
    if (!findings.length) console.log('  未发现任何外部引用（' + html.split('\n').length + ' 行被扫描）');
  }

  console.log('\n' + '-'.repeat(78));
  console.log('自检汇总: 通过 ' + gPass + '，失败 ' + gFail.length);
  if (gFail.length) { gFail.forEach((m, i) => console.log('  [' + (i + 1) + '] ' + m)); }
  console.log('判定: ' + (gFail.length === 0 ? 'PASS' : 'FAIL'));
  console.log('边界: 本自检只证明 web/ 侧代码与 docs/protocol.md + tools/proto_vectors.json 一致，');
  console.log('      **不证明真机 USB 链路成立**（设备未连接）。离线 PASS != 真机通过。');
  process.exit(gFail.length === 0 ? 0 : 1);
}

main().catch((e) => {
  console.error('\n[selfcheck] FATAL: ' + (e && e.stack || e));
  process.exit(2);
});
