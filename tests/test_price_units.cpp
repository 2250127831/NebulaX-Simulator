// ── 价格单位 + PnL 数值正确性（★ 本仿真新增）──
// 验证全链路统一 1/10000（tick）后：
//   1. RiskManager：buy@P 后 sell@P' 的 realized_pnl = (P'-P)×qty（tick×股）
//   2. equity = 初始 + 已实现 + 浮盈（mark 口径一致）
//   3. OrderManager：多次成交加权均价正确（ticks）
// 用构造的精确整数断言，杜绝"分/tick"口径混用导致的数值错乱。

#include "risk/risk_manager.h"
#include "oms/order_manager.h"
#include "core/types.h"

#include <cassert>
#include <cstdio>

int main() {
    // ── 1. RiskManager PnL（tick 口径）──
    {
        RiskManager rm;
        rm.set_initial_equity(10000000000LL);   // 1e10 ticks = 100 万元

        Order buy{};
        buy.symbol_id = 1; buy.side = OrderSide::BUY;
        buy.price = 100000; buy.quantity = 100;   // 10 元
        rm.on_fill(buy, 100);

        Order sell{};
        sell.symbol_id = 1; sell.side = OrderSide::SELL;
        sell.price = 100500; sell.quantity = 100;  // 10.05 元
        rm.on_fill(sell, 100);

        const int64_t expect_pnl = (100500 - 100000) * 100;   // 50000 tick×股
        assert(rm.realized_pnl() == expect_pnl);
        assert(rm.position(1) == 0);                          // 平仓后持仓 0

        std::printf("[1] realized_pnl = %lld == %lld ✓ (tick×股)\n",
                    (long long)rm.realized_pnl(), (long long)expect_pnl);
    }

    // ── 2. equity 含浮盈（mark 口径一致）──
    {
        RiskManager rm;
        rm.set_initial_equity(10000000000LL);
        Order buy{};
        buy.symbol_id = 2; buy.side = OrderSide::BUY;
        buy.price = 200000; buy.quantity = 50;   // 20 元 × 50 股
        rm.on_fill(buy, 50);
        rm.mark(2, 210000);                      // 现价 21 元
        const int64_t expect_equity = 10000000000LL + (210000 - 200000) * 50;  // 浮盈 +50000
        assert(rm.equity() == expect_equity);
        std::printf("[2] equity = %lld == %lld ✓ (初始+浮盈)\n",
                    (long long)rm.equity(), (long long)expect_equity);
    }

    // ── 3. OrderManager 加权均价（ticks）──
    {
        OrderManager om;
        om.set_next_id(1000);
        Order o{};
        o.symbol_id = 3; o.side = OrderSide::BUY;
        o.price = 300000; o.quantity = 100; o.timestamp = 1;
        const uint64_t id = om.new_order(o);
        om.on_accept(id);
        om.on_fill(id, 40, 300000);    // 半成交@30 元
        om.on_fill(id, 60, 300500);    // 余量@30.05 元
        const OrderManager::Entry* e = om.entry(id);
        assert(e->filled == 100);
        assert(e->remaining == 0);
        // avg = (40×300000 + 60×300500)/100 = 300300
        assert(e->avg_fill_price == 300300);
        assert(e->status == OrderStatus::FILLED);
        std::printf("[3] avg_fill_price = %lld == 300300 ✓ (ticks)\n",
                    (long long)e->avg_fill_price);
    }

    std::printf("ALL PRICE-UNIT CHECKS PASSED\n");
    return 0;
}
