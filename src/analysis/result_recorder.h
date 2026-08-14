#pragma once

// ── 结果记录器（★ 本仿真新增）──
// 记录段快照（净值/回撤/持仓）、策略订单终态、策略成交明细（trades.csv），
// 输出 CSV + 汇总（含绩效指标：夏普/索提诺/卡玛/胜率/盈亏比，从净值序列派生）。
// 供事后分析 + 用户 Python 深度分析。

#include "oms/order_manager.h"
#include "execution/execution_engine.h"   // TradeRecord

#include <cstdint>
#include <string>
#include <vector>

class ResultRecorder {
public:
    struct SegmentSnapshot {
        size_t segment;
        int64_t equity;         // 盯市净值（tick×股）
        int64_t drawdown;       // 当前回撤（tick×股）
        int64_t realized_pnl;   // 已实现盈亏（tick×股）
        int64_t net_position;   // 净持仓（已成交净量，全标的）
        uint64_t engine_trades; // 交易所累计成交笔数（含市场内部）
        size_t strategy_resting;// 策略当前挂单数
        uint64_t strategy_trades;   // 策略累计成交笔数
        uint64_t strategy_filled;   // 策略累计成交量
        int64_t strategy_pnl;       // 策略累计已实现盈亏
    };

    void add_snapshot(const SegmentSnapshot& s) { snapshots_.push_back(s); }
    void add_order_final(uint64_t id, const OrderManager::Entry& e) {
        orders_.emplace_back(id, e);
    }
    void add_trade(const TradeRecord& t) { trades_.push_back(t); }

    // 写 equity.csv / trades.csv / orders.csv / summary.txt 到 out_dir
    bool write(const std::string& out_dir,
               size_t messages, size_t order_events, size_t segments,
               uint64_t engine_trades) const;

    const std::vector<SegmentSnapshot>& snapshots() const { return snapshots_; }
    const std::vector<TradeRecord>& trades() const { return trades_; }

private:
    std::vector<SegmentSnapshot> snapshots_;
    std::vector<std::pair<uint64_t, OrderManager::Entry>> orders_;
    std::vector<TradeRecord> trades_;
};
