#pragma once

// ── ExchangeSimulator（★ 本仿真核心适配器：交易所 + 广播通道）──
// 语义：委托先进交易所撮合，撮合结果广播给交易系统（订阅方）。
//
//   - 历史委托（市场侧）  on_market_order：委托进 MatchingEngine 撮合 →
//                         归属路由（被动成交回报 OMS / 市场成交广播策略簿）→ 广播委托事件+成交事件
//   - 策略订单（策略侧）  submit/cancel：直接进 MatchingEngine，成交回报 OMS，残留挂簿注册
//   - 广播              OrderBookConsumer（策略市场簿，剔自单）：收到委托事件全量 + 成交 EXECUTE
//   - 权威簿            MatchingEngine：历史市场单 + 策略单，撮合产生全部成交
//
// 时序不变量：单线程，一条委托"撮合 → 广播 → 订阅方应用"全部完成才处理下一条。

#include "exchange/i_exchange_simulator.h"
#include "exchange/itch_parser.h"        // ItchEvent（交易所侧解析器，历史委托输入）
#include "exchange/matching_engine.h"    // MatchingEngine（权威簿）
#include "exchange/order_pool.h"
#include "exchange/order_map.h"
#include "exchange/protocol.h"           // BinaryResponse / RSP_* / OrderTif
#include "core/market_event.h"           // MarketEvent（广播给策略市场簿）
#include "market/book/order_book_consumer.h"  // 策略市场簿（订阅方）

#include <cstdint>
#include <functional>
#include <unordered_set>

class ExecutionEngine;   // 前向声明（被动成交回报回调）

class ExchangeSimulator : public IExchangeSimulator {
public:
    explicit ExchangeSimulator(size_t pool_slots = 1u << 20);

    // ── IExchangeSimulator（策略侧入口）──
    void submit(const Order& order, uint64_t order_id, nxex::OrderTif tif,
                ExchangeSubmitResult& out) override;
    void cancel(uint64_t order_id, uint64_t symbol_id, uint64_t user_id) override;
    bool book_quote(uint64_t symbol_id, BookQuote& out) const override;

    // ── 市场侧：历史委托进交易所（先撮合，后广播）──
    void on_market_order(const nxex::ItchEvent& ev);

    // ── 广播/回报目标绑定（组装时设置）──
    void set_market_view(OrderBookConsumer* view) { view_ = view; }
    void set_execution(ExecutionEngine* ex) { execution_ = ex; }
    // 广播订阅回调（策略 worker 等）：每条广播 MarketEvent 都转发（订阅方，单线程直调）
    void set_market_sink(std::function<void(const MarketEvent&)> sink) { sink_ = std::move(sink); }

    // ── 只读 ──
    const nxex::OrderBook* book(uint64_t locate) const { return engine_.book_for(locate); }
    bool is_strategy_order(uint64_t ref) const { return strategy_refs_.count(ref) > 0; }
    size_t strategy_resting() const { return strategy_refs_.size(); }
    uint64_t trade_count() const { return trades_; }
    uint64_t clock() const { return clock_; }

private:
    // 广播一条 MarketEvent 给策略市场簿
    void broadcast(const MarketEvent& ev);
    // ItchEvent（引擎输入）→ MarketEvent（交易系统广播）
    MarketEvent to_market_event(const nxex::ItchEvent& ev) const;
    static MarketEvent make_execute(uint64_t locate, uint64_t order_ref, uint64_t qty);
    // 策略挂单被历史委托被动成交：回报 OMS + 全成则注销注册表
    void on_strategy_maker_fill(uint64_t locate, uint64_t maker_ref, uint64_t qty, uint64_t price);
    // 若挂单已从权威簿消失（全成/被撤），从策略订单注册表移除
    void unregister_if_gone(uint64_t locate, uint64_t ref);

    nxex::OrderPool pool_;                // 交易所权威簿挂单池（必须先于 engine_）
    nxex::OrderMap  index_;               // 交易所权威簿挂单索引
    mutable nxex::MatchingEngine engine_; // 权威簿（历史市场单 + 策略单）；mutable 供 const 查询惰性建簿
    OrderBookConsumer* view_ = nullptr;    // 策略市场簿（订阅方）
    ExecutionEngine* execution_ = nullptr; // 策略 OMS/风控（成交回报）
    std::function<void(const MarketEvent&)> sink_;  // 广播订阅回调（策略 worker）
    std::unordered_set<uint64_t> strategy_refs_;  // 策略挂单注册表（归属判定）
    uint64_t trades_ = 0;                  // 引擎总成交笔数
    uint64_t clock_ = 0;                   // 广播单调时间戳（事件序号）
};
