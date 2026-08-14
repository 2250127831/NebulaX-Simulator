// ── 加权净投票仲裁测试（★ 吸收自 NebulaX-Trader arbitrate.h 语义）──
// 覆盖：同向全投 / 部分同意 / 反方向按权重 / 权重反转 / 阈值观望 /
//       强度加权平均 / primary 定标的 / N=1 / N=0 / primary 越界。

#include "strategy/base/arbitrate.h"

#include <cassert>
#include <cstdio>

int main() {
    // ── N=0：无策略不决策 ──
    {
        ArbDecision d = arbitrate_decide(nullptr, 0, nullptr, 0, 0);
        assert(!d.act);
        std::printf("[N=0] 不决策 ✓\n");
    }

    // ── 同向全投（权重 1.0，threshold 0）：BUY ──
    {
        ArbSignal s[2] = {{OrderSide::BUY, 1, 8000, 10}, {OrderSide::BUY, 1, 5000, 11}};
        int64_t w[2] = {10000, 10000};
        ArbDecision d = arbitrate_decide(s, 2, w, 0, 0);
        assert(d.act && d.dir == OrderSide::BUY);
        // 强度 = 同向加权平均 = (8000+5000)/2 = 6500
        assert(d.strength == 6500);
        assert(d.locate == 1);   // primary 定标的
        assert(d.seq == 10);
        std::printf("[同向全投] dir=BUY strength=6500 ✓\n");
    }

    // ── 部分同意：BUY 1.0 vs SELL 0.6 → net=0.4 > 0 → BUY ──
    {
        ArbSignal s[2] = {{OrderSide::BUY, 1, 8000, 0}, {OrderSide::SELL, 1, 8000, 0}};
        int64_t w[2] = {10000, 6000};
        ArbDecision d = arbitrate_decide(s, 2, w, 0, 0);
        assert(d.act && d.dir == OrderSide::BUY);
        std::printf("[部分同意] dir=BUY (net=+0.4) ✓\n");
    }

    // ── 权重反转：SELL 权重更大 → SELL ──
    {
        ArbSignal s[2] = {{OrderSide::BUY, 1, 8000, 0}, {OrderSide::SELL, 1, 8000, 0}};
        int64_t w[2] = {6000, 10000};
        ArbDecision d = arbitrate_decide(s, 2, w, 0, 0);
        assert(d.act && d.dir == OrderSide::SELL);
        std::printf("[权重反转] dir=SELL (net=-0.4) ✓\n");
    }

    // ── 阈值观望：net 未超阈值 → 不决策 ──
    {
        ArbSignal s[2] = {{OrderSide::BUY, 1, 8000, 0}, {OrderSide::SELL, 1, 8000, 0}};
        int64_t w[2] = {10000, 6000};
        ArbDecision d = arbitrate_decide(s, 2, w, 0, 5000);   // 阈值 0.5
        assert(!d.act);
        std::printf("[阈值观望] net=0.4 < 0.5 不下单 ✓\n");
    }

    // ── 强度加权平均（BUY 满强 + BUY 半强，权重 1:1）──
    {
        ArbSignal s[2] = {{OrderSide::BUY, 2, 10000, 5}, {OrderSide::BUY, 2, 4000, 6}};
        int64_t w[2] = {10000, 10000};
        ArbDecision d = arbitrate_decide(s, 2, w, 1, 0);
        assert(d.act && d.dir == OrderSide::BUY);
        assert(d.strength == 7000);
        assert(d.locate == 2 && d.seq == 6);   // primary=1 定标的/seq
        std::printf("[强度加权] strength=7000, primary 定标的 ✓\n");
    }

    // ── N=1：单策略 ──
    {
        ArbSignal s[1] = {{OrderSide::SELL, 3, 9000, 7}};
        int64_t w[1] = {10000};
        ArbDecision d = arbitrate_decide(s, 1, w, 0, 0);
        assert(d.act && d.dir == OrderSide::SELL && d.strength == 9000 && d.locate == 3);
        std::printf("[N=1] 单策略 SELL ✓\n");
    }

    // ── primary 越界 → 不决策 ──
    {
        ArbSignal s[2] = {{OrderSide::BUY, 1, 8000, 0}, {OrderSide::BUY, 1, 8000, 0}};
        int64_t w[2] = {10000, 10000};
        ArbDecision d = arbitrate_decide(s, 2, w, 5, 0);
        assert(!d.act);
        std::printf("[primary越界] 不决策 ✓\n");
    }

    std::printf("ALL ARBITRATE CHECKS PASSED\n");
    return 0;
}
