#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""MeidaP4 · I0 状态机黄金向量生成器（verifier 独立编写，Lead 2026-10 裁决要求）

背景（Lead 裁决原话）：`ReliableChannel`（W=8 / 200 ms / 5 次）与 `LinkSession`
（HELLO / 3 s link_down / zeroLenReads）此前只有"类存在且可构造"的断言。
**不要用真计时器做向量**（要么注入假时间、要么把实现当前行为当真值编码进向量 = 循环论证）。

正解：把状态机抽成**纯转移函数**，时间只从参数进来：

    reliableInit() / reliableStep(state, event, now_ms) -> { state, actions }
    linkInit()     / linkStep(state, event, now_ms)     -> { state, actions }

本生成器按 **docs/protocol.md §6.1 / §6.3 / §6.5** 逐条推导期望值（不引用任何实现），
输出 `test/host/sm_vectors.json`。断言采用**下界/上界 + 子集匹配**，只锁规约写死的事实，
不锁规约没写的实现细节（避免把"实现当前行为"当成真值）。

用法：
    python test/host/gen_sm_vectors.py [--out test/host/sm_vectors.json] [--check]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys

RELIABLE_IFACE = "reliableStep(state, event, nowMs) -> { state, actions }"
LINK_IFACE = "linkStep(state, event, nowMs) -> { state, actions }"


def R(name, notes, events, actions_contain=None, state=None, unordered=True):
    v = {"name": name, "notes": notes, "events": events,
         "expect": {"actions_contain": actions_contain or [], "state": state or []}}
    if unordered:
        v["expect"]["unordered"] = True
    return v


def L(name, notes, events, actions_contain=None, state=None, requires_spec=None, unordered=True):
    v = {"name": name, "notes": notes, "events": events,
         "expect": {"actions_contain": actions_contain or [], "state": state or []}}
    if unordered:
        v["expect"]["unordered"] = True
    if requires_spec:
        v["requires_spec"] = requires_spec
    return v


def build() -> dict:
    reliable = [
        R("reliable/window_never_exceeds_8",
          "§6.1：发送方最多 8 个未确认帧。连发 10 帧且无 ACK ⇒ 未确认数必须 <= 8"
          "（规约没写第 9 帧是阻塞还是丢弃，所以只断言不变量）",
          [{"at": i, "ev": "tx_send", "seq": i} for i in range(10)],
          state=[{"path": "outstanding_len", "max": 8}]),

        R("reliable/cumulative_ack_releases",
          "§6.1：收到 arg=X ⇒ seq < X 的帧全部释放（累计确认）。发 0..3 后收 arg=2 ⇒ 只剩 2 帧、最老为 seq 2",
          [{"at": 0, "ev": "tx_send", "seq": 0}, {"at": 1, "ev": "tx_send", "seq": 1},
           {"at": 2, "ev": "tx_send", "seq": 2}, {"at": 3, "ev": "tx_send", "seq": 3},
           {"at": 10, "ev": "rx_ack", "arg": 2}],
          state=[{"path": "outstanding_len", "eq": 2}, {"path": "outstanding_oldest_seq", "eq": 2}]),

        R("reliable/no_retx_before_200ms_retx_at_200ms",
          "§6.1：超时 200 ms 未推进窗口 ⇒ 重传窗口内最老一帧并置 RETX。"
          "t=199 不得重传；t=200 必须重传 seq=0 且 retx=true",
          [{"at": 0, "ev": "tx_send", "seq": 0}, {"at": 199, "ev": "tick"},
           {"at": 200, "ev": "tick"}],
          actions_contain=[{"act": "frame", "seq": 0, "retx": True}]),

        R("reliable/five_failures_then_link_down",
          "§6.1：连续 5 次重传失败 ⇒ 清空该通道 TX 队列、置 link_down。"
          "只断言**最终态**（link_down + 队列清空）—— 规约没规定第 5 次失败落在哪个 tick，"
          "所以不对中间 tick 下结论（避免把某种实现 convention 当规约）",
          [{"at": 0, "ev": "tx_send", "seq": 0}] +
          [{"at": t, "ev": "tick"} for t in (200, 400, 600, 800, 1000, 1200)],
          actions_contain=[{"act": "link_down"}],
          state=[{"path": "link_up", "eq": False}, {"path": "outstanding_len", "eq": 0}]),

        R("reliable/duplicate_seq_is_idempotent",
          "§6.1：接收方对重复 seq 必须幂等 —— 直接再回一次 ACK，不得把数据重复投递给上层。"
          "⇒ 必须出现 ack 动作，且不得出现 deliver 动作",
          [{"at": 0, "ev": "rx_data", "seq": 5}, {"at": 1, "ev": "rx_data", "seq": 5}],
          actions_contain=[{"act": "ack"}],
          state=[{"path": "deliver_count_max", "eq": 1}]),
    ]

    link = [
        L("link/hello_every_2s_until_hello_ack",
          "§6.3：设备每 2 s 发 HELLO 直到收到 HELLO_ACK。挂载后 t=0/2000/4000/6000 ⇒"
          "至少 3 次 send_hello（0 时刻是否立刻发规约没写死，用下界 3 / 上界 4）",
          [{"at": 0, "ev": "mount"}] + [{"at": t, "ev": "tick"} for t in (0, 2000, 4000, 6000)],
          actions_contain=[{"act": "send_hello"}],
          state=[{"path": "link_up", "eq": False}, {"path": "send_hello_count", "min": 3, "max": 4}]),

        L("link/hello_ack_converges_and_resets_seqs",
          "§6.3：设备收到 HELLO_ACK ⇒ 所有 TX seq 归 0、RX 期望值归 0、清空发送队列、link_up",
          [{"at": 0, "ev": "mount"}, {"at": 0, "ev": "tick"}, {"at": 10, "ev": "rx_hello_ack"}],
          actions_contain=[{"act": "link_up"}, {"act": "reset_seqs"}],
          state=[{"path": "link_up", "eq": True}, {"path": "tx_seq", "eq": 0},
                 {"path": "rx_expected", "eq": 0}]),

        L("link/valid_frame_postpones_link_down",
          "§6.3：连续 3 s 没收到任何**有效帧** ⇒ link_down。最后一次有效帧在 t=100 ⇒"
          "t=3099 仍 link_up、t=3100 必须 link_down",
          [{"at": 0, "ev": "mount"}, {"at": 0, "ev": "tick"}, {"at": 10, "ev": "rx_hello_ack"},
           {"at": 100, "ev": "rx_frame", "valid": True, "channel": 0, "seq": 0},
           {"at": 3099, "ev": "tick"}, {"at": 3100, "ev": "tick"}],
          actions_contain=[{"act": "link_down"}],
          state=[{"path": "link_up", "eq": False}]),

        L("link/zero_length_reads_do_not_count_as_valid_frames",
          "§4.1 + §6.3：0 字节 IN 读是**合法事件**但**不产生任何帧、不推进任何状态**。"
          "⇒ 每 500 ms 一次 0 字节读、连续 3 s，必须仍然 link_down（把 ZLP 当「链路活动」的实现会永远不掉线）",
          [{"at": 0, "ev": "mount"}, {"at": 0, "ev": "tick"}, {"at": 10, "ev": "rx_hello_ack"},
           {"at": 500, "ev": "rx_zero_read"}, {"at": 1000, "ev": "rx_zero_read"},
           {"at": 1500, "ev": "rx_zero_read"}, {"at": 2000, "ev": "rx_zero_read"},
           {"at": 2500, "ev": "rx_zero_read"}, {"at": 3000, "ev": "rx_zero_read"},
           {"at": 3100, "ev": "tick"}],
          actions_contain=[{"act": "link_down"}],
          state=[{"path": "link_up", "eq": False}, {"path": "zero_len_reads", "min": 6}]),

        L("link/bootstrap_frames_bypass_seq_gate",
          "⚠️ 引导期 seq 门豁免（两侧实现都做了：web/proto.js:784/884 与 webusb_proto.c:27-28/693），"
          "但**现行 docs/protocol.md 没有这一条**（§6.5 只有 ①–⑤）⇒ 本向量的规约依据待补。"
          "可观察结论：引导期设备发 HELLO(seq=0) 之后，主机回 HELLO_ACK(seq=0) 仍必须被接受并收敛到 link_up"
          "（若被 seq 门当重复帧顶掉，链路永远收敛不了）",
          [{"at": 0, "ev": "mount"}, {"at": 0, "ev": "tick"},
           {"at": 5, "ev": "rx_hello", "arg": 1, "seq": 0},
           {"at": 6, "ev": "rx_hello_ack", "seq": 0}],
          actions_contain=[{"act": "link_up"}],
          state=[{"path": "link_up", "eq": True}],
          requires_spec="docs/protocol.md §6.5⑥（**当前文档缺失**；实现里存在，见 webusb_proto.c:27-28）"),
    ]

    names = [v["name"] for v in reliable + link]
    assert len(names) == len(set(names)), "向量名必须唯一"
    return {
        "schema": "meida-p4/proto-sm-vectors/1",
        "generated_by": "test/host/gen_sm_vectors.py（verifier；按 docs/protocol.md §6 推导，不引用实现）",
        "spec": "docs/protocol.md §6.1 / §6.3 / §6.4 / §6.5",
        "time_policy": "时间只从 nowMs 参数进入；转移函数内部不得读时钟 / 起 timer / 用 setTimeout",
        "interface": {
            "reliableInit": "reliableInit() -> state",
            "reliableStep": RELIABLE_IFACE,
            "linkInit": "linkInit() -> state",
            "linkStep": LINK_IFACE,
            "state_schema": {
                "reliable": {
                    "outstanding": "数组，元素至少含 {seq, retxCount, sentAtMs}",
                    "outstanding_len": "未确认帧数（派生视图，runner 会优先读它，缺失时用 outstanding.length）",
                    "outstanding_oldest_seq": "窗口内最老一帧的 seq（派生视图）",
                    "link_up": "bool",
                    "deliver_count_max": "累计向投递上层的数据帧数（幂等验证用）",
                },
                "link": {
                    "link_up": "bool", "tx_seq": "u16", "rx_expected": "u16",
                    "zero_len_reads": "u32", "send_hello_count": "u32",
                },
            },
            "event_vocabulary": {
                "reliable": ["tx_send{seq}", "rx_ack{arg}", "rx_data{seq}", "tick"],
                "link": ["mount", "rx_frame{valid,channel,seq}", "rx_zero_read",
                         "rx_hello{arg,seq}", "rx_hello_ack{seq}", "tick"],
            },
            "action_vocabulary": ["frame{seq,retx}", "ack{arg}", "link_up", "link_down",
                                  "reset_seqs", "send_hello", "deliver"],
        },
        "reliable": reliable,
        "link": link,
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(os.path.dirname(here))
    ap.add_argument("--out", default=os.path.join(here, "sm_vectors.json"))
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    doc = build()
    spec = os.path.join(root, "docs", "protocol.md")
    doc["spec_sha256"] = hashlib.sha256(open(spec, "rb").read()).hexdigest() if os.path.exists(spec) else "(missing)"
    text = json.dumps(doc, ensure_ascii=False, indent=1) + "\n"

    print("[gen_sm_vectors] reliable=%d link=%d 合计=%d"
          % (len(doc["reliable"]), len(doc["link"]), len(doc["reliable"]) + len(doc["link"])))
    print("[gen_sm_vectors] docs/protocol.md sha256 = %s" % doc["spec_sha256"])
    print("[gen_sm_vectors] 产物 sha256 = %s" % hashlib.sha256(text.encode("utf-8")).hexdigest())
    if args.check:
        print("[gen_sm_vectors] --check：未写文件")
        return 0
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("[gen_sm_vectors] 已写入 %s" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
