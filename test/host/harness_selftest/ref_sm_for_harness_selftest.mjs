// SPDX-License-Identifier: Apache-2.0
//
// ⚠️⚠️ 这不是产品代码，也**不是**被验证对象 ⚠️⚠️
//
// 用途（仅此一个）：在 `web/proto.js` 还没提供 `reliableStep` / `linkStep`（Lead 2026-10 裁决要求）
// 之前，用一份按 **docs/protocol.md §6.1/§6.3/§6.5** 直写的参考转移函数，把
// `test/host/run_proto_sm.mjs` + `test/host/sm_vectors.json` 这套**测试骨架本身**跑通，
// 确认我的向量期望值没有写错。
//
//   * 用它跑出的 PASS **不能**作为 web/proto.js 合规的证据。
//   * tools/verify-proto.ps1 默认**不**使用本文件（本文件只被 -HarnessSelftest 使用）。
//
// 时间只从 nowMs 参数进来 —— 本文件不读时钟、不起 timer（与产品实现的要求一致）。

export const WINDOW = 8;
export const ACK_TIMEOUT_MS = 200;
export const MAX_RETX = 5;
export const LINK_DOWN_MS = 3000;
export const HELLO_PERIOD_MS = 2000;

/* ---------------- 可靠通道（§6.1） ---------------- */

export function reliableInit() {
  return {
    outstanding: [],      // [{ seq, retxCount, sentAtMs }]
    link_up: true,
    retx_failures: 0,
    rx_seen: [],
    deliver_count: 0,
    last_advance_ms: 0,
  };
}

export function reliableStep(state, event, nowMs) {
  const s = { ...state, outstanding: (state.outstanding || []).map((o) => ({ ...o })) };
  const actions = [];

  switch (event.ev) {
    case 'tx_send':
      if (s.outstanding.length >= WINDOW) {
        actions.push({ act: 'blocked', seq: event.seq });   // 窗口满：规约没写策略，这里选择阻塞
        break;
      }
      s.outstanding.push({ seq: event.seq, retxCount: 0, sentAtMs: nowMs });
      actions.push({ act: 'frame', seq: event.seq, retx: false });
      break;

    case 'rx_ack':
      // §6.1 累计确认：arg = 下一个期望的 seq ⇒ 释放 seq < arg
      s.outstanding = s.outstanding.filter((o) => o.seq >= event.arg);
      s.retx_failures = 0;
      s.last_advance_ms = nowMs;
      break;

    case 'rx_data': {
      // §6.1 重复 seq 幂等：再回一次 ACK，不重复投递
      if (!s.rx_seen.includes(event.seq)) {
        s.rx_seen = [...s.rx_seen, event.seq];
        s.deliver_count++;
        actions.push({ act: 'deliver', seq: event.seq });
      }
      actions.push({ act: 'ack', arg: event.seq + 1 });
      break;
    }

    case 'tick': {
      if (!s.outstanding.length || !s.link_up) break;
      const oldest = s.outstanding[0];
      if (nowMs - oldest.sentAtMs < ACK_TIMEOUT_MS) break;
      // §6.1：连续 5 次重传失败 ⇒ 清空 TX 队列、link_down
      if (oldest.retxCount >= MAX_RETX) {
        s.outstanding = [];
        s.link_up = false;
        s.retx_failures++;
        actions.push({ act: 'link_down', channel: event.channel ?? 0 });
        actions.push({ act: 'reset_wait_hello' });
        break;
      }
      oldest.retxCount++;
      oldest.sentAtMs = nowMs;
      actions.push({ act: 'frame', seq: oldest.seq, retx: true });
      if (oldest.retxCount >= MAX_RETX) {
        s.outstanding = [];
        s.link_up = false;
        actions.push({ act: 'link_down', channel: event.channel ?? 0 });
      }
      break;
    }

    default:
      break;
  }
  return { state: s, actions };
}

/* ---------------- 链路会话（§6.3 + §4.1 + §6.5④） ---------------- */

export function linkInit() {
  return {
    link_up: false,
    mounted: false,
    tx_seq: 0,
    rx_expected: 0,
    zero_len_reads: 0,
    send_hello_count: 0,
    last_valid_ms: -1,
    next_hello_ms: 0,
    bootstrap: true,
  };
}

export function linkStep(state, event, nowMs) {
  const s = { ...state };
  const actions = [];

  switch (event.ev) {
    case 'mount':
      s.mounted = true;
      s.link_up = false;
      s.bootstrap = true;
      s.last_valid_ms = -1;         // 挂载本身不算"收到有效帧"
      s.next_hello_ms = nowMs;
      break;

    case 'tick':
      if (s.mounted && !s.link_up && nowMs >= s.next_hello_ms) {
        actions.push({ act: 'send_hello', arg: 1 });
        s.send_hello_count++;
        s.next_hello_ms = nowMs + HELLO_PERIOD_MS;     // §6.3 每 2 s 一次
      }
      if (s.link_up && s.last_valid_ms >= 0 && nowMs - s.last_valid_ms >= LINK_DOWN_MS) {
        s.link_up = false;
        s.bootstrap = true;
        s.next_hello_ms = nowMs;                        // 重新进入"每 2 s 发 HELLO"
        s.last_valid_ms = -1;
        actions.push({ act: 'link_down' });
      }
      break;

    case 'rx_hello':
      // §6.5④：引导帧**绕开 seq 门**（不推进 rx_expected）
      s.last_valid_ms = nowMs;
      s.rx_expected = s.rx_expected;                    // 显式：不推进
      actions.push({ act: 'ack', arg: 0 });
      break;

    case 'rx_hello_ack':
      // §6.3：收到 HELLO_ACK ⇒ 所有 TX seq 归 0、RX 期望值归 0、清空队列、link_up
      s.last_valid_ms = nowMs;
      s.link_up = true;
      s.bootstrap = false;
      s.tx_seq = 0;
      s.rx_expected = 0;
      actions.push({ act: 'link_up' });
      actions.push({ act: 'reset_seqs' });
      break;

    case 'rx_frame':
      if (event.valid) {
        s.last_valid_ms = nowMs;
        if (!s.bootstrap && typeof event.seq === 'number') {
          s.rx_expected = (event.seq + 1) & 0xFFFF;     // §6.3 运行期的 seq 门
        }
      }
      break;

    case 'rx_zero_read':
      // §4.1：0 字节读合法，但**不产生帧、不推进任何状态**（尤其不刷新 last_valid_ms）
      s.zero_len_reads++;
      break;

    default:
      break;
  }
  return { state: s, actions };
}
