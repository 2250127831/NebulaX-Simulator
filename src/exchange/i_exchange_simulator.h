#pragma once

// ── 内存交易所接口（本仿真新增，替代原 ExecutionEngine 的网络 sender/codec）──
// 原 NebulaX-Trader 通过 OUCH/TCP 把订单发给外部交易所，成交回报经 fill_th 线程解码。
// 本仿真单机、纯内存通信：订单直接进本进程内的撮合引擎（ExchangeSimulator），
// 成交回报同步回灌 OMS/Risk。ExecutionEngine 只依赖本接口，不感知网络。

#include "core/types.h"
#include "exchange/protocol.h"   // OrderTif (DAY/IOC/FOK)
#include <cstdint>
#include <vector>

// 策略订单在引擎中的 user_id（自成交排除 + 归属判定用）
// 引擎按 taker 的 user_id 排除同属挂单 → 策略单不会吃自己的挂单。
static constexpr uint64_t kStrategyUserId = 0x4E585F55LL;  // "NX_U"

// ── 订单回报结构（协议无关，迁移自 Trader oms/i_order_codec.h）──
// type: 'A' Accepted / 'E' Executed / 'C' Canceled / 'J' Rejected
struct Fill {
    uint8_t  type = 'E';
    uint64_t order_id = 0;      // 内部订单 id
    uint64_t exchange_ref = 0;  // 交易所 ref（'A' 带回，0=未接受）
    uint64_t filled_qty = 0;    // 该笔成交量
    int64_t  fill_price = 0;    // 成交价（分）
};

// ── 盘口快照（协议无关，迁移自 Trader oms/i_order_codec.h）──
struct BookQuote {
    uint64_t symbol_id = 0;
    int64_t  bid = 0;
    uint64_t bid_vol = 0;
    int64_t  ask = 0;
    uint64_t ask_vol = 0;
};

// ── 策略订单提交结果（内存撮合同步返回）──
struct ExchangeSubmitResult {
    bool accepted = false;        // 非拒单（挂簿 或 部分/全部成交）
    bool fully_filled = false;    // 全部成交，无残留
    std::vector<Fill> fills;      // 每笔主动成交（价/量）
};

class IExchangeSimulator {
public:
    virtual ~IExchangeSimulator() = default;

    // 策略订单发往交易所。order 已由 OMS 分配 order_id（引擎 order_ref）。
    // tif: OrderType::MARKET → nxex::OrderTif::IOC；OrderType::LIMIT → nxex::OrderTif::DAY
    virtual void submit(const Order& order, uint64_t order_id,
                        nxex::OrderTif tif, ExchangeSubmitResult& out) = 0;

    // 撤单：按 order_id（引擎 order_ref）撤挂单。symbol_id 用于路由到引擎的 locate 簿。
    virtual void cancel(uint64_t order_id, uint64_t symbol_id, uint64_t user_id) = 0;

    // 盘口快照（供市价→限价转换）。无有效盘口返回 false。
    virtual bool book_quote(uint64_t symbol_id, BookQuote& out) const = 0;
};
