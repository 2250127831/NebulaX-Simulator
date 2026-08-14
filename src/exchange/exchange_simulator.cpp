#include "exchange/exchange_simulator.h"
#include "execution/execution_engine.h"   // 完整定义（回报回调 on_order_report）

// ══ 构造：多簿模式，共享池 + 索引；metrics/WAL/TradePool 置空（仿真不需要）══

ExchangeSimulator::ExchangeSimulator(size_t pool_slots)
    : pool_(pool_slots)
    , index_(pool_slots)
    , engine_(pool_, index_ /*metrics=*/, nullptr) {}

// ── 市场侧：历史委托进交易所 ──

void ExchangeSimulator::on_market_order(const nxex::ItchEvent& ev) {
    ++clock_;
    std::vector<nxex::BinaryResponse> rsp;
    switch (ev.type) {
        case nxex::ItchEvent::Type::ADD:
            engine_.processAdd(ev.locate, ev.order_ref, ev.side, ev.price, ev.shares,
                               /*user_id=*/ev.order_ref, rsp, nxex::OrderTif::DAY);
            break;
        case nxex::ItchEvent::Type::DELETE:
            engine_.processCancel(ev.locate, ev.order_ref, /*user_id=*/ev.order_ref, rsp);
            break;
        case nxex::ItchEvent::Type::CANCEL:
            engine_.processCancelShares(ev.locate, ev.order_ref, ev.shares,
                                        /*user_id=*/ev.order_ref, rsp);
            break;
        case nxex::ItchEvent::Type::REPLACE: {
            // U 消息无方向，查旧单补 side（查不到 → INVALID，引擎会拒）
            nxex::OrderBook* b = engine_.book_for(ev.locate);
            const nxex::Order* old = b ? b->findOrder(ev.order_ref) : nullptr;
            engine_.processReplace(ev.locate, ev.order_ref, ev.new_order_ref,
                                   old ? old->side : nxex::Side::INVALID,
                                   ev.price, ev.shares, /*user_id=*/ev.order_ref, rsp);
            break;
        }
        default:
            return;  // 非订单事件不进交易所
    }

    // ── 广播委托事件全量（含立即成交的 taker 也先入簿，与真实 ITCH 广播一致）──
    broadcast(to_market_event(ev));

    // ── 广播/回报撮合成交（nxex::RSP_TRADE 只指 maker）──
    uint64_t taker_fill = 0;
    for (auto& r : rsp) {
        if (r.type != nxex::RSP_TRADE) continue;
        ++trades_;
        taker_fill += r.data.trade.quantity;
        const uint64_t maker_ref = r.data.trade.order_ref;
        if (is_strategy_order(maker_ref)) {
            // 策略挂单被历史委托被动成交 → 回报 OMS/Risk
            on_strategy_maker_fill(ev.locate, maker_ref,
                                   r.data.trade.quantity, r.data.trade.price);
        } else {
            // 市场 maker 被吃 → 广播 EXECUTE 扣减策略簿
            broadcast(make_execute(ev.locate, maker_ref, r.data.trade.quantity));
        }
    }
    // taker 自身成交（市场 taker）→ 广播 EXECUTE 扣减策略簿里刚入簿的委托
    if (taker_fill > 0) {
        const uint64_t taker_ref =
            (ev.type == nxex::ItchEvent::Type::REPLACE) ? ev.new_order_ref : ev.order_ref;
        broadcast(make_execute(ev.locate, taker_ref, taker_fill));
    }
}

// ── 策略侧：策略订单进交易所 ──

void ExchangeSimulator::submit(const Order& order, uint64_t order_id, nxex::OrderTif tif,
                               ExchangeSubmitResult& out) {
    ++clock_;
    std::vector<nxex::BinaryResponse> rsp;
    const nxex::Side side =
        (order.side == OrderSide::BUY) ? nxex::Side::BUY : nxex::Side::SELL;
    engine_.processAdd(order.symbol_id, order_id, side, (uint32_t)order.price,
                       (uint32_t)order.quantity, kStrategyUserId, rsp, tif);

    out.accepted = false;
    out.fully_filled = false;
    uint64_t filled_total = 0;
    for (auto& r : rsp) {
        switch (r.type) {
            case nxex::RSP_TRADE:
                ++trades_;
                out.accepted = true;
                filled_total += r.data.trade.quantity;
                out.fills.push_back(Fill{/*type=*/'E', /*order_id=*/order_id,
                                         /*exchange_ref=*/0,
                                         /*filled_qty=*/r.data.trade.quantity,
                                         /*fill_price=*/(int64_t)r.data.trade.price});
                // 市场 maker 被策略吃 → 广播 EXECUTE 扣减策略簿
                broadcast(make_execute(order.symbol_id, r.data.trade.order_ref,
                                       r.data.trade.quantity));
                break;
            case nxex::RSP_OK:   // 剩余挂簿
                out.accepted = true;
                strategy_refs_.insert(order_id);   // 注册，等后续历史委托被动吃
                break;
            case nxex::RSP_FILLED:
                out.accepted = true;
                break;
            case nxex::RSP_ERROR:   // 拒单（无流动性 / 参数非法 / 池满）
                break;
            default:
                break;
        }
    }
    out.fully_filled = (filled_total == (uint64_t)order.quantity);
}

void ExchangeSimulator::cancel(uint64_t order_id, uint64_t symbol_id, uint64_t user_id) {
    ++clock_;
    std::vector<nxex::BinaryResponse> rsp;
    engine_.processCancel(symbol_id, order_id, user_id, rsp);
    for (auto& r : rsp) {
        if (r.type == nxex::RSP_CANCELLED || r.type == nxex::RSP_OK) {
            // 撤单成功：若订单仍在注册表则移除（可能已被吃，被动成交时已移除）
            strategy_refs_.erase(order_id);
            if (execution_) {
                execution_->on_order_report(
                    Fill{/*type=*/'C', /*order_id=*/order_id, 0, 0, 0});
            }
        }
    }
}

bool ExchangeSimulator::book_quote(uint64_t symbol_id, BookQuote& out) const {
    const nxex::OrderBook* b = engine_.book_for(symbol_id);
    if (!b) return false;
    const nxex::TopOfBook t = b->getTopOfBook();
    out.symbol_id = symbol_id;
    out.bid = t.bid_price ? (int64_t)t.bid_price : 0;
    out.bid_vol = t.bid_volume;
    out.ask = t.ask_price ? (int64_t)t.ask_price : 0;
    out.ask_vol = t.ask_volume;
    return out.bid > 0 && out.ask > 0;
}

// ── 私有：广播 / 事件转换 / 归属路由 ──

void ExchangeSimulator::broadcast(const MarketEvent& ev) {
    if (view_) view_->on_event(ev);   // 策略市场簿（重建盘口）
    if (sink_) sink_(ev);             // 订阅方（策略 worker 信号更新）
}

MarketEvent ExchangeSimulator::to_market_event(const nxex::ItchEvent& ev) const {
    MarketEvent m{};
    m.timestamp = clock_;
    m.locate = ev.locate;
    switch (ev.type) {
        case nxex::ItchEvent::Type::ADD:
            m.type = MarketEvent::Type::ADD;
            m.order.side = (ev.side == nxex::Side::BUY) ? OrderSide::BUY : OrderSide::SELL;
            m.order.price = ev.price;
            m.order.shares = ev.shares;
            m.order.order_ref = ev.order_ref;
            break;
        case nxex::ItchEvent::Type::DELETE:
            m.type = MarketEvent::Type::DELETE;
            m.order.order_ref = ev.order_ref;
            break;
        case nxex::ItchEvent::Type::CANCEL:
            m.type = MarketEvent::Type::CANCEL;
            m.order.order_ref = ev.order_ref;
            m.order.shares = ev.shares;
            break;
        case nxex::ItchEvent::Type::REPLACE:
            m.type = MarketEvent::Type::REPLACE;
            m.order.order_ref = ev.order_ref;
            m.order.new_order_ref = ev.new_order_ref;
            m.order.price = ev.price;
            m.order.shares = ev.shares;
            break;
        default:
            break;  // BOOK 等不进广播
    }
    return m;
}

MarketEvent ExchangeSimulator::make_execute(uint64_t locate, uint64_t order_ref, uint64_t qty) {
    MarketEvent m{};
    m.type = MarketEvent::Type::EXECUTE;
    m.locate = locate;
    m.trade.order_ref = order_ref;
    m.trade.volume = qty;
    m.trade.price = -1;   // E 无价，策略簿消费者查簿补全
    return m;
}

void ExchangeSimulator::on_strategy_maker_fill(uint64_t locate, uint64_t maker_ref,
                                               uint64_t qty, uint64_t price) {
    if (execution_) {
        execution_->on_order_report(Fill{/*type=*/'E', /*order_id=*/maker_ref,
                                         /*exchange_ref=*/0, /*filled_qty=*/qty,
                                         /*fill_price=*/(int64_t)price});
    }
    unregister_if_gone(locate, maker_ref);
}

void ExchangeSimulator::unregister_if_gone(uint64_t locate, uint64_t ref) {
    nxex::OrderBook* b = engine_.book_for(locate);
    if (!b || b->findOrder(ref) == nullptr) strategy_refs_.erase(ref);
}
