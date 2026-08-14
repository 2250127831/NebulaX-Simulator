#pragma once

#include "strategy/base/signal.h"
#include "oms/order_manager.h"
#include "risk/risk_manager.h"
#include "exchange/i_exchange_simulator.h"

#include <cstdint>
#include <functional>
#include <cstdio>
#include <map>
#include <mutex>

// ── 策略成交记录（★ 本仿真新增，供结果分析 trades.csv）──
// 每次策略成交触发回调，供 ResultRecorder 记逐笔明细。
// 主动 = 策略作为 taker 吃市场挂单；被动 = 策略挂单被市场吃掉。
struct TradeRecord {
    uint64_t order_id;        // 策略订单 id
    uint64_t symbol_id;       // 标的
    OrderSide side;           // 买卖
    uint64_t qty;             // 该笔成交量
    int64_t price;            // 成交价（tick）
    int64_t mid;              // 成交时盘口中间价（tick），滑点 = price - mid 方向感知
    bool passive;             // true=被动成交（挂单被吃），false=主动成交
    uint64_t segment;         // 触发段号
    uint64_t seq;             // 触发事件序号
};

// ── 执行引擎（本仓库改造：网络 sender/codec → 内存交易所 IExchangeSimulator）──
// 把策略信号转成订单，走 风控校验 → OMS 登记 → 发往内存交易所撮合 → 成交回报 的完整链路。
//
// 信号 → 数量：经典策略不直接给下单量，数量由资金管理层换算。
//   quantity = base_qty × strength / kStrengthScale
//   即满强度下单 base_qty，半强度下 base_qty 的一半；NONE 不下单。
//
// 发送模式（本仿真单机纯内存）：
//   submit_signal 在锁内调 exchange_->submit(...) 直接撮合，同步拿成交回报；
//   被动成交（策略挂单被后续历史委托吃掉）由 ExchangeSimulator 回调 on_order_report 驱动。
//
// 线程安全：单线程仿真，但保留 mtx_ 串行化 OMS/Risk 访问（与原设计一致）。
class ExecutionEngine {
public:
    ExecutionEngine(OrderManager& om, RiskManager& rm)
        : order_manager_(om), risk_manager_(rm) {}

    // 配置
    void set_base_qty(uint64_t qty) { base_qty_ = qty; }
    void set_exchange(IExchangeSimulator* ex) { exchange_ = ex; }
    // 成交记录回调（结果分析 trades.csv）
    void set_trade_recorder(std::function<void(const TradeRecord&)> cb) { trade_cb_ = std::move(cb); }
    // 成交时中间价（按 symbol，SimWorker 喂，供滑点计算）；segment/seq 触发上下文
    void set_mid(uint64_t symbol_id, int64_t mid) { if (mid >= 0) mid_map_[symbol_id] = mid; }
    void set_segment_ctx(uint64_t segment, uint64_t seq) { segment_ = segment; seq_ = seq; }

    // 提交一个策略信号，返回订单 id。无信号(side NONE)/无价格 → 0(不下单)。
    // type: 订单类型(默认 MARKET; 限价单 type=LIMIT 且 price>0, 市价→限价转换用)。
    // 风控拒绝 → 订单登记为 REJECTED，返回其 id。
    // 撮合拒绝 → REJECTED。成功 → SUBMITTED + 成交驱动 FILLED/PARTIAL_FILL。
    uint64_t submit_signal(const Signal& sig, uint64_t strategy_id,
                           OrderType type = OrderType::MARKET) {
        if (sig.side == OrderSide::NONE || sig.price < 0) return 0;
        if (type == OrderType::LIMIT && sig.price <= 0) return 0;   // 限价单必须带价

        // 数量：满强度 = base_qty，无强度 = 0(不下单)
        uint64_t qty = base_qty_ * (uint64_t)sig.strength
                     / (uint64_t)Signal::kStrengthScale;
        if (qty == 0) return 0;

        Order order{};
        order.strategy_id = strategy_id;
        order.symbol_id   = sig.locate;
        order.side        = sig.side;
        order.type        = type;
        order.price       = sig.price;
        order.quantity    = qty;
        order.timestamp   = sig.timestamp;

        uint64_t id;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            id = order_manager_.new_order(order);         // PENDING
            if (!risk_manager_.check_order(order)) {
                order_manager_.on_reject(id);             // 风控拦截
                return id;
            }
            if (!exchange_) { order_manager_.on_reject(id); return id; }
            // 内存撮合：市价单 → IOC（全吃或作废），限价单 → DAY（挂簿）
            nxex::OrderTif tif = (type == OrderType::LIMIT)
                                     ? nxex::OrderTif::DAY : nxex::OrderTif::IOC;
            ExchangeSubmitResult r;
            exchange_->submit(order, id, tif, r);         // 撮合在锁内，同步回报
            if (!r.accepted) {
                order_manager_.on_reject(id);             // 拒单（无流动性/参数非法）
            } else {
                order_manager_.on_accept(id);             // PENDING → SUBMITTED
                for (auto& f : r.fills) {                 // 主动成交 → OMS/Risk
                    order_manager_.on_fill(id, f.filled_qty, f.fill_price);
                    // Risk 用真实成交价（市价单 order.price=0/信号价，用它会算错 avg_cost/PnL）
                    Order risk_order = order;
                    risk_order.price = f.fill_price;
                    risk_manager_.on_fill(risk_order, f.filled_qty);
                    const int64_t mid = mid_of(order.symbol_id);
                    record_trade(id, order.symbol_id, order.side, f.filled_qty,
                                 f.fill_price, mid, /*passive=*/false);
                }
            }
            flatten_on_drawdown();
        }
        return id;
    }

    // 成交回报（被动成交/撤单确认等，ExchangeSimulator 回调）。
    void on_order_fill(uint64_t order_id, uint64_t filled_qty, int64_t fill_price) {
        std::lock_guard<std::mutex> lk(mtx_);
        const Order* o = order_manager_.order(order_id);
        if (!o) return;
        order_manager_.on_fill(order_id, filled_qty, fill_price);
        Order risk_order = *o;
        risk_order.price = fill_price;               // 用真实成交价
        risk_manager_.on_fill(risk_order, filled_qty);
        record_trade(order_id, o->symbol_id, o->side, filled_qty, fill_price,
                     mid_of(o->symbol_id), /*passive=*/true);
        flatten_on_drawdown();   // 成交后评估: 回撤破第二档则平仓
    }

    // 订单回报分发（'A'/'E'/'C'/'J'）：按 type 驱动 OMS 状态机 + 风控。
    void on_order_report(const Fill& f) {
        std::lock_guard<std::mutex> lk(mtx_);
        const Order* o = order_manager_.order(f.order_id);
        if (!o) return;
        switch (f.type) {
            case 'A':   // Accepted: 进入活态, 记录交易所分配的 ref
                order_manager_.on_accept(f.order_id, f.exchange_ref);
                break;
            case 'E':   // Executed: 成交(可多次, 累积到 FILLED)
                order_manager_.on_fill(f.order_id, f.filled_qty, f.fill_price);
                { Order risk_order = *o; risk_order.price = f.fill_price;
                  risk_manager_.on_fill(risk_order, f.filled_qty); }   // 真实成交价
                record_trade(f.order_id, o->symbol_id, o->side, f.filled_qty,
                             f.fill_price, mid_of(o->symbol_id), /*passive=*/true);
                flatten_on_drawdown();   // 成交后评估: 回撤破第二档则平仓
                break;
            case 'C':   // Canceled: 撤单
                order_manager_.on_cancel(f.order_id);
                break;
            case 'J':   // Rejected: 拒单
                order_manager_.on_reject(f.order_id);
                break;
            default: break;
        }
    }

    // 撤单：OMS 标记 PENDING_CANCEL + 内存交易所撤单。
    bool cancel_order(uint64_t order_id) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!exchange_) return false;
        const Order* o = order_manager_.order(order_id);
        if (!o) return false;
        if (!order_manager_.request_cancel(order_id)) return false;   // 非法状态不可撤
        exchange_->cancel(order_id, o->symbol_id, kStrategyUserId);  // 引擎撤单 + 回报 'C'
        return true;
    }

    // ── 盘口查询 + 市价→限价转换(V5, 柜台行为) ──

    // 盘口查询(同步)：直接从内存交易所权威簿取 BBO，无网络等待。
    bool query_book(uint64_t symbol_id, BookQuote& out) {
        if (!exchange_) return false;
        return exchange_->book_quote(symbol_id, out);
    }
    // 下单前实时权威簿中间价（滑点基准）：无有效盘口返回 -1。
    int64_t live_mid(uint64_t symbol_id) {
        BookQuote q;
        if (query_book(symbol_id, q) && q.bid > 0 && q.ask > 0)
            return (q.bid + q.ask) / 2;
        return -1;
    }

    // 市价→限价转换(柜台行为)：查盘口 → 按盘口价转限价单(TIF='D')发出。
    // BUY 用 ask 价(买吃卖一), SELL 用 bid 价(卖打买一)。查盘口失败/无价 → 0(不下单)。
    uint64_t submit_market_as_limit(const Signal& sig, uint64_t strategy_id) {
        if (sig.side == OrderSide::NONE) return 0;
        BookQuote q;
        if (!query_book(sig.locate, q)) return 0;
        int64_t limit_price = (sig.side == OrderSide::BUY) ? q.ask : q.bid;
        if (limit_price <= 0) return 0;   // 无对手盘价, 不下单
        Signal limit_sig = sig;
        limit_sig.price = limit_price;
        return submit_signal(limit_sig, strategy_id, OrderType::LIMIT);
    }

    // 回撤平仓触发(V5)：回撤破第二档 → 撤全部活态订单。成交后评估调用。
    // 防重复: flatten_issued_ 标志, 触发一次后不再重复撤。
    void flatten_on_drawdown() {
        if (flatten_issued_) return;
        if (!risk_manager_.drawdown_flatten()) return;   // 未破第二档
        flatten_issued_ = true;
        order_manager_.iterate([&](uint64_t id, const OrderManager::Entry& e) {
            if (e.status == OrderStatus::SUBMITTED ||
                e.status == OrderStatus::PARTIAL_FILL) {
                if (exchange_) {
                    order_manager_.request_cancel(id);
                    exchange_->cancel(id, e.order.symbol_id, kStrategyUserId);
                } else {
                    order_manager_.request_cancel(id);
                }
            }
        });
    }

private:
    int64_t mid_of(uint64_t symbol_id) const {
        auto it = mid_map_.find(symbol_id);
        return (it != mid_map_.end()) ? it->second : -1;
    }
    void record_trade(uint64_t order_id, uint64_t symbol_id, OrderSide side,
                      uint64_t qty, int64_t price, int64_t mid, bool passive) {
        if (!trade_cb_) return;
        // 防御：mid 无效（<0）时用成交价（滑点=0），不污染 trades.csv
        if (mid < 0) mid = price;
        trade_cb_(TradeRecord{order_id, symbol_id, side, qty, price,
                              mid, passive, segment_, seq_});
    }
    OrderManager& order_manager_;
    RiskManager&  risk_manager_;
    IExchangeSimulator* exchange_ = nullptr;   // 内存交易所（本仿真）
    uint64_t base_qty_ = 100;   // 满强度基准下单量(股)
    bool flatten_issued_ = false;   // 回撤平仓是否已触发(防重复)
    std::function<void(const TradeRecord&)> trade_cb_;  // 成交记录回调
    std::map<uint64_t, int64_t> mid_map_;  // symbol → 最近中间价（滑点基准）
    uint64_t segment_ = 0;    // 触发段号
    uint64_t seq_ = 0;        // 触发事件序号
    std::mutex mtx_;
};
