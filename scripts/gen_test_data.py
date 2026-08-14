#!/usr/bin/env python3
"""生成测试 ITCH 样本（CI 无本地大样本时用）。

生成两个文件：
  test_data/itch_full_sample.bin    —— 构造的完整样本（订单流+成交流一一对应），512 字节。
                                      test_replay_pure --exact 要求它 100% 重演等价 + 0 错误。
  test_data/itch_synth_sample.bin   —— 合成小样本（多 symbol，供 replay_pure 结构验证）。

用法：python3 scripts/gen_test_data.py [out_dir]
"""
import os
import struct
import sys


def frame(body):
    return struct.pack(">H", len(body)) + body


def A(loc, ref, side, sh, px):
    return (b"A" + struct.pack(">H", loc) + struct.pack(">H", 0) + b"\x00" * 6
            + struct.pack(">Q", ref) + (b"B" if side == "B" else b"S")
            + struct.pack(">I", sh) + b"\x00" * 8 + struct.pack(">I", px))


def D(loc, ref):
    return b"D" + struct.pack(">H", loc) + struct.pack(">H", 0) + b"\x00" * 6 + struct.pack(">Q", ref)


def X(loc, ref, sh):
    return (b"X" + struct.pack(">H", loc) + struct.pack(">H", 0) + b"\x00" * 6
            + struct.pack(">Q", ref) + struct.pack(">I", sh))


def E(loc, ref, sh):
    return (b"E" + struct.pack(">H", loc) + struct.pack(">H", 0) + b"\x00" * 6
            + struct.pack(">Q", ref) + struct.pack(">I", sh) + struct.pack(">I", 1))


def P(loc, ref, side, sh, px):
    return (b"P" + struct.pack(">H", loc) + struct.pack(">H", 0) + b"\x00" * 6
            + struct.pack(">Q", ref) + struct.pack(">I", 1)
            + (b"S" if side == "S" else b"B") + struct.pack(">I", sh)
            + b"\x00" * 8 + struct.pack(">I", px))


def build_full_sample():
    """完整样本：每个成交都能在订单流中找到 maker+taker（一一对应）。"""
    # 场景1: 买100@10000 vs 卖100@10000 → 成交100, maker=买ref1
    # 场景2: 买50@10000, 卖20吃20, 卖30@9999吃30 → 全成50, maker=买ref10
    # 场景3: 买40@10000, 撤15, 卖10吃10, 删剩余15 → maker=买ref20
    frames = [
        A(1, 1, "B", 100, 10000), A(1, 2, "S", 100, 10000),
        E(1, 1, 100), P(1, 1, "S", 100, 10000),
        A(1, 10, "B", 50, 10000), A(1, 11, "S", 20, 10000), E(1, 10, 20),
        A(1, 12, "S", 30, 9999), E(1, 10, 30), P(1, 10, "S", 50, 10000),
        A(1, 20, "B", 40, 10000), X(1, 20, 15), A(1, 21, "S", 10, 10000), E(1, 20, 10), D(1, 20),
    ]
    return b"".join(frame(f) for f in frames)


def build_synth_sample():
    """合成小样本：多 symbol 委托流 + 真实成交，供 replay_pure 结构验证（非 exact）。"""
    frames = []
    ref = 1
    for loc in (1, 2, 3, 4, 5):
        base = 10000 + loc * 100
        # 每 symbol：先挂买盘，再挂交叉卖盘吃买盘 → 有真实成交
        buy1, sell1 = ref, ref + 1
        ref += 2
        frames.append(A(loc, buy1, "B", 50, base))          # 买 50 @ base
        frames.append(A(loc, sell1, "S", 40, base - 20))    # 卖 40 @ base-20（交叉，吃买单）
        frames.append(E(loc, buy1, 40))                     # 买盘被吃 40（历史成交）
        frames.append(P(loc, buy1, "S", 40, base - 20))
        frames.append(X(loc, buy1, 5))                      # 部分撤剩余买 10 中的 5
    return b"".join(frame(f) for f in frames)


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "test_data"
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "itch_full_sample.bin"), "wb") as f:
        f.write(build_full_sample())
    with open(os.path.join(out_dir, "itch_synth_sample.bin"), "wb") as f:
        f.write(build_synth_sample())
    print(f"generated {out_dir}/itch_full_sample.bin, {out_dir}/itch_synth_sample.bin")


if __name__ == "__main__":
    main()
