// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · I0 线协议离线一致性测试（node 侧，verifier 独立编写）
//
// 被验证对象（**只读**）：web/proto.js（WS-webusb 产出，规约 docs/protocol.md §7）
// 黄金向量来源    ：tools/proto_vectors.json（test/host/gen_vectors.py 独立生成）
//
// 断言与 test/host/test_codec.c **一一对应**：crc16 命中 / encodeFrame 逐字节 == frame_hex /
// 解码往返一致 / 逐字节喂入一致 / 鲁棒性向量（resync、坏 CRC、length=1025、ZLP）
//
// 用法：node test/host/run_proto_js.mjs [vectors.json] [web/proto.js]
// 退出码：0 = 全部通过；1 = 有断言失败；2 = 工具/输入错误（proto.js 缺失或无法载入）

import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

let gPass = 0;
let gFail = 0;
let gSkip = 0;
const failures = [];

function ck(cond, ctx, msg) {
  if (cond) {
    gPass++;
    return true;
  }
  gFail++;
  const line = `  [FAIL] ${ctx} : ${msg}`;
  console.log(line);
  failures.push(line.trim());
  return false;
}

const hex = (u8) => Buffer.from(u8).toString('hex');
const fromHex = (s) => Uint8Array.from(Buffer.from(s, 'hex'));
const eqBytes = (a, b) => a.length === b.length && Buffer.compare(Buffer.from(a), Buffer.from(b)) === 0;

async function loadProtoJs(rel) {
  const abs = path.resolve(rel);
  const src = readFileSync(abs, 'utf8');
  const errs = [];
  try {
    return await import(pathToFileURL(abs).href);
  } catch (e) {
    errs.push(`import(): ${e.message}`);
  }
  try {
    // 绕开 package.json 的 type 解析：直接以 ESM 载入源码
    const url = 'data:text/javascript;base64,' + Buffer.from(src, 'utf8').toString('base64');
    return await import(url);
  } catch (e) {
    errs.push(`data: URL: ${e.message}`);
  }
  try {
    const require = createRequire(import.meta.url);
    const m = require(abs);
    return { ...m, __cjs: true };
  } catch (e) {
    errs.push(`require() CJS: ${e.message}`);
  }
  throw new Error(errs.join('\n  '));
}

/** 兼容多种帧对象形状（docs/protocol.md §7 未规定帧对象字段名） */
function normFrame(f) {
  if (!f || typeof f !== 'object') return null;
  const channel = f.channel ?? f.ch ?? 0;
  const flags = f.flags ?? 0;
  const seq = f.seq ?? 0;
  const payload = f.payload ?? f.data ?? f.bytes ?? new Uint8Array(0);
  const u8 = payload instanceof Uint8Array ? payload : Uint8Array.from(payload);
  return { channel, flags, seq, length: u8.length, payload: u8 };
}

function collector(dec) {
  const out = [];
  const push = (bytes) => {
    // docs/protocol.md §7 的形态是 push(bytes) 直接吐出帧；这里对"回调式"实现也做兼容，
    // 但**只在返回值拿不到帧时**才采用回调结果，避免同一次 push 被记两遍。
    let cbFrames = null;
    const cb = (fr) => { (cbFrames ??= []).push(fr); return true; };
    let r;
    try {
      r = dec.push(bytes, cb);
    } catch (e) {
      if (dec.push.length >= 2) { cbFrames = null; r = dec.push(bytes); }
      else throw e;
    }
    if (Array.isArray(r)) { for (const f of r) out.push(f); }
    else if (r && typeof r === 'object' && r.channel !== undefined) out.push(r);
    else if (cbFrames) for (const f of cbFrames) out.push(f);
  };
  return { out, push };
}

function decodeAll(dec, bytes, feed) {
  const col = collector(dec);
  for (const chunk of feed(bytes)) col.push(chunk);
  return col.out.map(normFrame).filter(Boolean);
}

function* planChunks(bytes, v) {
  if (Array.isArray(v.chunk_plan)) {
    let off = 0;
    for (const n of v.chunk_plan) {
      if (n === 0) yield new Uint8Array(0);
      else { yield bytes.subarray(off, off + n); off += n; }
    }
    return;
  }
  if (!v.chunk_size || v.chunk_size <= 0) {
    yield bytes;
    return;
  }
  const step = v.chunk_size;
  let pushed = 0;
  for (let i = 0; i < bytes.length; i += step) {
    yield bytes.subarray(i, Math.min(i + step, bytes.length));
    pushed++;
    if (v.zero_push_every > 0 && pushed % v.zero_push_every === 0) yield new Uint8Array(0);
  }
}

function statsOf(dec) {
  const s = dec.stats ?? {};
  return {
    frames: s.frames ?? (Array.isArray(s) ? undefined : undefined),
    crc_errors: s.crcErrors ?? s.crc_errors ?? null,
    bad_header: s.badHeader ?? s.bad_header ?? null,
    resync_bytes: s.resyncBytes ?? s.resync_bytes ?? null,
  };
}

function testCrcVector(v, crc16) {
  const ctx = v.name;
  const got = crc16(fromHex(v.payload_hex)) & 0xFFFF;
  ck(got === v.crc16, ctx, `crc16 不符：got=0x${got.toString(16)} exp=0x${v.crc16.toString(16)}`);
}

function testFrameVector(v, api) {
  const { crc16, encodeFrame, FrameDecoder } = api;
  const ctx = v.name;
  const payload = fromHex(v.payload_hex);
  const frame = fromHex(v.frame_hex);

  ck(payload.length === v.length, ctx, `payload_hex 长度 ${payload.length} != length ${v.length}`);
  ck(frame.length === 12 + v.length, ctx, `frame_hex 长度 ${frame.length} != 12+${v.length}`);

  // crc16（独立复算字段）
  const crcOver = new Uint8Array(10 + payload.length);
  crcOver.set(frame.subarray(0, 10), 0);
  crcOver.set(payload, 10);
  ck((crc16(crcOver) & 0xFFFF) === v.crc16, ctx, 'crc16(hdr10+payload) 不符');

  // encodeFrame 逐字节
  let enc;
  try {
    enc = encodeFrame({ channel: v.channel, flags: v.flags, seq: v.seq, payload });
  } catch (e) {
    ck(false, ctx, `encodeFrame 抛异常: ${e.message}`);
    return;
  }
  if (!ck(enc instanceof Uint8Array || Buffer.isBuffer(enc), ctx, 'encodeFrame 未返回 Uint8Array')) return;
  ck(enc.length === frame.length, ctx, `encodeFrame 长度 ${enc.length} != ${frame.length}`);
  ck(eqBytes(enc, frame), ctx, `encodeFrame 字节流不符（got ${hex(enc).slice(0, 32)}... exp ${hex(frame).slice(0, 32)}...）`);

  // decode(encode(x)) == x：整块 + 逐字节
  for (const [mode, feed] of [
    ['整块', (b) => [b]],
    ['逐字节', function* (b) { for (let i = 0; i < b.length; i++) yield b.subarray(i, i + 1); }],
  ]) {
    let frames;
    try {
      frames = decodeAll(new FrameDecoder(), enc, feed);
    } catch (e) {
      ck(false, ctx, `${mode}解码抛异常: ${e.message}`);
      continue;
    }
    if (!ck(frames.length === 1, ctx, `${mode}喂入应解出 1 帧，实测 ${frames.length}`)) continue;
    const f = frames[0];
    ck(f.channel === v.channel, ctx, `${mode} channel=${f.channel} exp ${v.channel}`);
    ck(f.flags === v.flags, ctx, `${mode} flags=0x${f.flags.toString(16)} exp 0x${v.flags.toString(16)}`);
    ck(f.seq === v.seq, ctx, `${mode} seq=${f.seq} exp ${v.seq}`);
    ck(f.length === v.length, ctx, `${mode} length=${f.length} exp ${v.length}`);
    ck(eqBytes(f.payload, payload), ctx, `${mode} payload 不符`);
  }
}

const statRows = [];

function testStreamVector(v, api) {
  const { FrameDecoder } = api;
  const ctx = v.name;
  const bytes = fromHex(v.stream_hex);
  let frames;
  let dec;
  try {
    dec = new FrameDecoder();
    frames = decodeAll(dec, bytes, (b) => planChunks(b, v));
  } catch (e) {
    ck(false, ctx, `解码抛异常: ${e.message}`);
    return;
  }

  const exp = v.expect_frames ?? [];
  ck(frames.length === exp.length, ctx, `解出 ${frames.length} 帧（期望 ${exp.length}）`);
  if (frames.length === exp.length) {
    for (let i = 0; i < exp.length; i++) {
      const e = exp[i], f = frames[i];
      ck(f.channel === e.channel, ctx, `帧${i} channel=${f.channel} exp ${e.channel}`);
      ck(f.flags === e.flags, ctx, `帧${i} flags=0x${f.flags.toString(16)} exp 0x${e.flags.toString(16)}`);
      ck(f.seq === e.seq, ctx, `帧${i} seq=${f.seq} exp ${e.seq}`);
      ck(f.length === e.length, ctx, `帧${i} length=${f.length} exp ${e.length}`);
      ck(eqBytes(f.payload, fromHex(e.payload_hex)), ctx, `帧${i} payload 不符`);
    }
  }

  const st = statsOf(dec);
  if (st.crc_errors === null) {
    gSkip++;
    console.log(`  [SKIP] ${ctx} : stats 无 crcErrors 字段（docs §7 要求提供），无法断言`);
  } else {
    ck(st.crc_errors === v.expect_crc_errors, ctx, `stats.crcErrors=${st.crc_errors} exp ${v.expect_crc_errors}`);
  }
  if (st.resync_bytes === null) {
    gSkip++;
    console.log(`  [SKIP] ${ctx} : stats 无 resyncBytes 字段（docs §7 要求提供）`);
  } else {
    ck(st.resync_bytes === v.expect_resync_bytes, ctx,
       `stats.resyncBytes=${st.resync_bytes} exp ${v.expect_resync_bytes}`);
  }
  if (st.bad_header === null) {
    gSkip++;   // 字段不在 docs §7 的强制清单里，只做"存在则检查"
  } else {
    ck(st.bad_header === v.expect_bad_header, ctx, `stats.badHeader=${st.bad_header} exp ${v.expect_bad_header}`);
  }
  if (st.frames !== undefined && st.frames !== null) {
    ck(st.frames === exp.length, ctx, `stats.frames=${st.frames} exp ${exp.length}`);
  }
  if (v.invariant_group) {
    statRows.push({ name: v.name, group: v.invariant_group, ...st });
  }
  console.log(`  [info] ${ctx}: 解出=${frames.length} crcErrors=${st.crc_errors} badHeader=${st.bad_header} resyncBytes=${st.resync_bytes}`);
}

/** 同一字节流的不同喂入粒度必须给出相同诊断计数（见 wu_codec.c:20-21 的同名声明） */
function testGranularityInvariants() {
  const ctx = '不变量/喂入粒度';
  for (let i = 0; i < statRows.length; i++) {
    for (let j = i + 1; j < statRows.length; j++) {
      const a = statRows[i], b = statRows[j];
      if (a.group !== b.group) continue;
      for (const k of ['crc_errors', 'bad_header', 'resync_bytes', 'frames']) {
        if (a[k] === null || a[k] === undefined || b[k] === null || b[k] === undefined) continue;
        ck(a[k] === b[k], ctx, `[${a.group}] ${k} 依赖喂入粒度：${a.name}=${a[k]} vs ${b.name}=${b[k]}`);
      }
    }
  }
}

function testZlpNoop(api) {
  const { FrameDecoder } = api;
  const ctx = 'zlp/noop';
  try {
    const dec = new FrameDecoder();
    const col = collector(dec);
    for (let i = 0; i < 5; i++) col.push(new Uint8Array(0));
    ck(col.out.length === 0, ctx, '0 字节 push 不得产出帧');
    const st = statsOf(dec);
    if (st.crc_errors !== null) ck(st.crc_errors === 0, ctx, `0 字节 push 后 crcErrors=${st.crc_errors}`);
    if (st.resync_bytes !== null) ck(st.resync_bytes === 0, ctx, `0 字节 push 后 resyncBytes=${st.resync_bytes}`);
    // 空 push 穿插真实帧
    const enc = api.encodeFrame({ channel: 0, flags: 0x0002, seq: 7, payload: Uint8Array.from([0x78]) });
    const dec2 = new FrameDecoder();
    const col2 = collector(dec2);
    col2.push(new Uint8Array(0));
    for (let i = 0; i < enc.length; i++) { col2.push(enc.subarray(i, i + 1)); col2.push(new Uint8Array(0)); }
    const fr = col2.out.map(normFrame).filter(Boolean);
    ck(fr.length === 1 && fr[0].seq === 7, ctx, `空 push 穿插影响了解码（count=${fr.length}）`);
  } catch (e) {
    ck(false, ctx, `抛异常: ${e.message}`);
  }
}

async function main() {
  const vectorsPath = process.argv[2] ?? 'tools/proto_vectors.json';
  const protoPath = process.argv[3] ?? 'web/proto.js';

  console.log('==============================================================================');
  console.log('MeidaP4 · I0 线协议离线一致性测试（node 侧）');
  console.log(`  被验证 : ${protoPath}`);
  console.log(`  向量   : ${vectorsPath}`);
  console.log('  规约   : docs/protocol.md');
  console.log('==============================================================================');

  const vectors = JSON.parse(readFileSync(path.resolve(vectorsPath), 'utf8'));
  let api;
  try {
    api = await loadProtoJs(protoPath);
  } catch (e) {
    console.log(`[run_proto_js] FATAL 无法载入 ${protoPath}：\n  ${e.message}`);
    return 2;
  }

  console.log('[run_proto_js] 契约面检查（docs/protocol.md §7 必须提供的导出）');
  ck(api.MAGIC === 0x504d, 'exports', `MAGIC=${api.MAGIC}（期望 0x504D）`);
  ck(typeof api.crc16 === 'function', 'exports', 'crc16 不是函数');
  ck(typeof api.encodeFrame === 'function', 'exports', 'encodeFrame 不是函数');
  ck(typeof api.FrameDecoder === 'function', 'exports', 'FrameDecoder 类缺失（docs §7 强制）');
  ck(typeof api.ReliableChannel === 'function', 'exports', 'ReliableChannel 类缺失（docs §7 强制）');
  ck(typeof api.LinkSession === 'function', 'exports', 'LinkSession 类缺失（docs §7 强制）');
  if (gFail) {
    console.log('[run_proto_js] 导出面不满足 docs/protocol.md §7，后续向量断言无意义 -> 中止');
    return 1;
  }
  console.log('  [ok] MAGIC / crc16 / encodeFrame / FrameDecoder / ReliableChannel / LinkSession 均存在');

  const n = { frame: 0, crc: 0, stream: 0 };
  for (const v of vectors) {
    if (v.kind === 'crc') { n.crc++; testCrcVector(v, api.crc16); }
    else if (v.kind === 'frame') { n.frame++; testFrameVector(v, api); }
    else if (v.kind === 'stream') { n.stream++; testStreamVector(v, api); }
    else ck(false, v.name ?? '(未知)', `未知向量类型 ${v.kind}`);
  }
  testZlpNoop(api);
  testGranularityInvariants();

  console.log('------------------------------------------------------------------------------');
  console.log(`向量统计: frame=${n.frame} crc=${n.crc} stream=${n.stream}`);
  console.log(`断言汇总: ${gPass + gFail} 条，通过 ${gPass}，失败 ${gFail}，跳过 ${gSkip}`);
  console.log(`判定    : ${gFail === 0 ? 'PASS（离线一致性：web/proto.js 与 docs/protocol.md 相符）'
                                    : 'FAIL（web/proto.js 与黄金向量不符，见上面逐条 FAIL）'}`);
  console.log('边界    : 本结果**只**证明 JS 编解码器与 docs/protocol.md 逐字节一致，');
  console.log('          **不能**证明真机 USB 链路成立（WebUSB 权限、Windows device interface 均不在覆盖范围）。');
  console.log('==============================================================================');
  return gFail === 0 ? 0 : 1;
}

process.exit(await main());
