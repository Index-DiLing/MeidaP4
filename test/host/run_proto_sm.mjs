// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · I0 状态机离线断言（verifier；Lead 2026-10 裁决要求）
//
// 被验证对象：web/proto.js 的**纯转移函数**入口（由 ws-webui 提供）
//   reliableInit() / reliableStep(state, event, nowMs) -> { state, actions }
//   linkInit()     / linkStep(state, event, nowMs)     -> { state, actions }
//
// 向量：test/host/sm_vectors.json（按 docs/protocol.md §6 独立推导，不引用实现）
//
// 设计要点（避免循环论证）：
//   * 时间**只**从 nowMs 参数进来 —— 本 runner 从不调用 setTimeout / sleep / Date.now；
//     这样"状态机语义"是可离线判定的。
//   * 断言只锁规约写死的事实：用 actions **子集匹配** + state 的 eq/min/max 边界，
//     不锁规约没写的实现细节。
//   * 计时器真实接线（精度、浏览器节流、后台标签页）**不做向量** —— 见 docs/verification 的 D 清单。
//
// 退出码：0 = 全部通过；1 = 有断言失败；2 = 输入/工具错误；3 = **接口未就绪**（proto.js 还没提供 step()）

import { readFileSync } from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import { createRequire } from 'node:module';

let gPass = 0, gFail = 0, gSkip = 0;

function ck(cond, ctx, msg) {
  if (cond) { gPass++; return true; }
  gFail++;
  console.log(`  [FAIL] ${ctx} : ${msg}`);
  return false;
}

async function loadProtoJs(rel) {
  const abs = path.resolve(rel);
  const src = readFileSync(abs, 'utf8');
  const errs = [];
  try { return await import(pathToFileURL(abs).href); } catch (e) { errs.push(`import(): ${e.message}`); }
  try {
    const url = 'data:text/javascript;base64,' + Buffer.from(src, 'utf8').toString('base64');
    return await import(url);
  } catch (e) { errs.push(`data: URL: ${e.message}`); }
  try { return { ...createRequire(import.meta.url)(abs) }; } catch (e) { errs.push(`require(): ${e.message}`); }
  throw new Error(errs.join('\n  '));
}

function get(obj, dotted) {
  let cur = obj;
  for (const k of dotted.split('.')) {
    if (cur === null || cur === undefined) return undefined;
    cur = cur[k];
  }
  return cur;
}

/** state 断言：eq / min / max。派生视图缺失时用约定的原始字段兜底 */
function stateValue(state, path_) {
  if (path_ === 'outstanding_len') {
    if (state.outstanding_len !== undefined) return state.outstanding_len;
    if (Array.isArray(state.outstanding)) return state.outstanding.length;
  }
  if (path_ === 'outstanding_oldest_seq') {
    if (state.outstanding_oldest_seq !== undefined) return state.outstanding_oldest_seq;
    if (Array.isArray(state.outstanding) && state.outstanding.length) return state.outstanding[0].seq;
  }
  const direct = get(state, path_);
  if (direct !== undefined) return direct;
  // `xxx_max` 只是"单调不减的累计值"的别名（如 deliver_count_max -> deliver_count）
  if (path_.endsWith('_max')) return get(state, path_.slice(0, -4));
  return undefined;
}

function matchAction(actual, pattern) {
  if (!actual || typeof actual !== 'object') return false;
  if (pattern.act !== undefined && actual.act !== pattern.act && actual.type !== pattern.act) return false;
  for (const [k, v] of Object.entries(pattern)) {
    if (k === 'act') continue;
    if (actual[k] !== v) return false;
  }
  return true;
}

function runVectors(kind, vectors, stepFn, initFn, api) {
  for (const v of vectors) {
    const ctx = v.name;
    let state, actions = [];
    try {
      state = initFn();
      for (const ev of v.events) {
        const { at, ev: name, ...rest } = ev;
        const r = stepFn(state, { ev: name, ...rest }, at);
        if (!r || typeof r !== 'object' || !r.state) {
          ck(false, ctx, `step() 必须返回 { state, actions }（事件 ${name}@${at} 实测 ${JSON.stringify(r)}）`);
          state = null; break;
        }
        state = r.state;
        if (Array.isArray(r.actions)) actions.push(...r.actions);
      }
    } catch (e) {
      ck(false, ctx, `转移函数抛异常: ${e.message}`);
      continue;
    }
    if (!state) continue;

    if (v.requires_spec) {
      gSkip++;
      console.log(`  [SKIP] ${ctx} : 规约依据缺失 -> ${v.requires_spec}`);
      continue;
    }

    for (const pat of v.expect.actions_contain ?? []) {
      ck(actions.some((a) => matchAction(a, pat)), ctx,
         `动作序列里缺少 ${JSON.stringify(pat)}（实测 ${actions.length} 个动作：${JSON.stringify(actions).slice(0, 240)}）`);
    }
    for (const s of v.expect.state ?? []) {
      const got = stateValue(state, s.path);
      if (s.eq !== undefined) {
        ck(got === s.eq, ctx, `state.${s.path} = ${JSON.stringify(got)}（期望 ${JSON.stringify(s.eq)}）`);
      }
      if (s.min !== undefined) {
        ck(typeof got === 'number' && got >= s.min, ctx, `state.${s.path} = ${JSON.stringify(got)}（期望 >= ${s.min}）`);
      }
      if (s.max !== undefined) {
        ck(typeof got === 'number' && got <= s.max, ctx, `state.${s.path} = ${JSON.stringify(got)}（期望 <= ${s.max}）`);
      }
    }
    console.log(`  [ok] ${ctx}`);
  }
}

async function main() {
  const vectorsPath = process.argv[2] ?? 'test/host/sm_vectors.json';
  const protoPath = process.argv[3] ?? 'web/proto.js';

  console.log('==============================================================================');
  console.log('MeidaP4 · I0 状态机离线断言（纯转移函数 + 参数化时间）');
  console.log(`  被验证 : ${protoPath}`);
  console.log(`  向量   : ${vectorsPath}`);
  console.log('  规约   : docs/protocol.md §6.1 / §6.3 / §6.5');
  console.log('==============================================================================');

  const doc = JSON.parse(readFileSync(path.resolve(vectorsPath), 'utf8'));
  let api;
  try {
    api = await loadProtoJs(protoPath);
  } catch (e) {
    console.log(`[run_proto_sm] FATAL 无法载入 ${protoPath}：\n  ${e.message}`);
    return 2;
  }

  const have = (f) => typeof api[f] === 'function';
  const missing = ['reliableInit', 'reliableStep', 'linkInit', 'linkStep'].filter((f) => !have(f));
  if (missing.length) {
    console.log(`[run_proto_sm] 接口未就绪：${protoPath} 缺少 ${missing.join(' / ')}`);
    console.log('  需要 ws-webui 按 Lead 裁决把状态机抽成纯转移函数：');
    console.log(`    ${doc.interface.reliableStep}`);
    console.log(`    ${doc.interface.linkStep}`);
    console.log('  时间只从 nowMs 参数进来（内部不得读时钟 / 起 timer / 用 setTimeout）。');
    console.log('  状态字段与事件/动作词表见 test/host/sm_vectors.json 的 interface 段。');
    console.log('  本步骤计 SKIP（不计 FAIL）—— 但 B 清单里的"状态机语义"此时**未被验证**。');
    return 3;
  }

  runVectors('reliable', doc.reliable, api.reliableStep, api.reliableInit, api);
  runVectors('link', doc.link, api.linkStep, api.linkInit, api);

  console.log('------------------------------------------------------------------------------');
  console.log(`断言汇总: ${gPass + gFail} 条，通过 ${gPass}，失败 ${gFail}，跳过 ${gSkip}`);
  console.log(`判定    : ${gFail === 0 ? 'PASS（状态机语义与 docs/protocol.md §6 一致）' : 'FAIL（见上面逐条 FAIL）'}`);
  console.log('边界    : 时间被参数化 ⇒ **状态机语义**可离线判定；');
  console.log('          **计时器真实接线**（精度、浏览器节流、后台标签页）离线不可判定，见 docs/verification 的 D 清单。');
  console.log('==============================================================================');
  return gFail === 0 ? 0 : 1;
}

process.exit(await main());
