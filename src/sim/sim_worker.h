#pragma once

// ── SimWorker（★ 本仿真新增，模板化吸收自 Trader BookWorker<Strategies...>）──
// 单线程仿真：每个 worker 负责一组 symbol（locate % nworkers == id），
// 订阅交易所广播的市场事件（策略市场簿已剔自单），框架算一次 BookContext 传各策略，
// 段边界 arbitrate() 用加权净投票（arbitrate_decide 纯函数）在插入窗口下单。
//
// 编译期绑定：策略经 CRTP StrategyT 内联，std::get<I> 常量索引，无虚调用、无容器遍历。
// 加策略 = 模板参数加类型 + config 白名单，不改本类。

#include "core/config.h"
#include "core/market_event.h"
#include "core/types.h"
#include "market/book/order_book_consumer.h"
#include "execution/execution_engine.h"
#include "risk/risk_manager.h"
#include "strategy/base/arbitrate.h"
#include "strategy/base/signal.h"
#include "strategy/base/strategy.h"

#include <array>
#include <cstdint>
#include <tuple>
#include <utility>

// 下单节奏（迁移自 Trader main.cpp：方向翻转必下；方向不变仅强度跳变 ≥ 阈值才再下）
static constexpr int64_t kStrengthStep = 500;   // 千分比定点(500 = 5% 满强度)

template <class... Strategies>
class SimWorker {
public:
    SimWorker() = default;

    void init(size_t id, size_t nworkers, uint64_t strategy_id,
              uint64_t max_position, ExecutionEngine* ex, RiskManager* rm,
              const StrategyConfig& sc) {
        id_ = id; nworkers_ = nworkers; strategy_id_ = strategy_id;
        max_position_ = max_position; ex_ = ex; rm_ = rm;
        // 仲裁运行时配置：权重按策略名对应槽（未列缺省 1.0 = 10000），primary 索引，净投票阈值
        const auto& st = sc.strategies;
        constexpr size_t N = sizeof...(Strategies);
        for (size_t i = 0; i < N && i < st.size(); ++i) {
            auto it = sc.weights_bp.find(st[i]);
            weight_bp_[i] = (it != sc.weights_bp.end()) ? it->second : 10000;
            if (st[i] == sc.primary) primary_idx_ = i;
        }
        threshold_bp_ = sc.vote_threshold_bp;
    }

    // 消费一条交易所广播的市场事件（仅处理归属本 worker 的 symbol）
    void on_market_event(const MarketEvent& ev, const OrderBookConsumer& market_view) {
        if (nworkers_ > 1 && (ev.locate % nworkers_) != id_) return;
        const OrderBook* book = market_view.book(ev.locate);

        // 盯市：盘口有效时喂中间价给风控（回撤按盯市净值计算）
        if (book && book->best_bid() >= 0 && book->best_ask() >= 0)
            rm_->mark(ev.locate, (book->best_bid() + book->best_ask()) / 2);

        // 框架算一次 BookContext（方向 + BBO + 现价），各策略共享，避免每策略查簿。
        BookContext ctx;
        ctx.book = book;
        ctx.seq = ev.timestamp;
        if (book && book->best_bid() >= 0 && book->best_ask() >= 0) {
            ctx.bid = book->best_bid(); ctx.bid_vol = book->best_bid_volume();
            ctx.ask = book->best_ask(); ctx.ask_vol = book->best_ask_volume();
            ctx.mid = (ctx.bid + ctx.ask) / 2;
        }
        // 方向：A/U 自带 side；D/X/E 查簿
        if (ev.type == MarketEvent::Type::ADD || ev.type == MarketEvent::Type::REPLACE) {
            ctx.side = ev.order.side;
        } else if (book) {
            if (ev.type == MarketEvent::Type::TRADE || ev.type == MarketEvent::Type::EXECUTE)
                ctx.side = book->side_of(ev.trade.order_ref);
            else
                ctx.side = book->side_of(ev.order.order_ref);
        }
        // 喂触发上下文（被动成交记录用：段号 + 事件序号）
        ex_->set_segment_ctx(cur_segment_, ev.timestamp);
        // 逐策略 on_market（方向/盘口门控由策略内部保证）
        feed(ev, ctx, std::index_sequence_for<Strategies...>{});
    }

    // 段边界：策略插入窗口。先触发各策略 on_segment 钩子，再加权净投票仲裁下单。
    void arbitrate(size_t segment) {
        if (!ex_ || !rm_ || sizeof...(Strategies) == 0) return;
        cur_segment_ = segment;
        on_segment_all(segment, std::index_sequence_for<Strategies...>{});
        constexpr size_t N = sizeof...(Strategies);
        ArbSignal sigs[N];
        build_sigs(sigs, std::index_sequence_for<Strategies...>{});
        ArbDecision d = arbitrate_decide(sigs, N, weight_bp_.data(), primary_idx_, threshold_bp_);
        if (!d.act) return;

        const uint64_t pos = rm_->position(d.locate);
        const bool blocked = (d.dir == OrderSide::BUY && pos >= max_position_) ||
                             (d.dir == OrderSide::SELL && pos == 0);
        const bool fresh_dir = d.dir != last_side_;
        const bool strength_ge = d.strength >= last_str_ + kStrengthStep;
        if (blocked || !(fresh_dir || strength_ge)) return;

        // 下单前喂该 symbol 的实时权威簿 mid（滑点基准：市价单吃当前 mid）+ 触发上下文
        const int64_t live = ex_->live_mid(d.locate);
        if (live > 0) ex_->set_mid(d.locate, live);
        ex_->set_segment_ctx(segment, 0);
        Signal decision{/*side=*/d.dir, /*locate=*/d.locate, /*price=*/0,
                        /*timestamp=*/0, /*strength=*/d.strength};
        const uint64_t oid = ex_->submit_signal(decision, strategy_id_);
        if (oid != 0) {
            last_side_ = d.dir;
            last_str_ = d.strength;
        }
    }

    size_t strategy_count() const { return sizeof...(Strategies); }

private:
    // 编译期展开：每策略 on_market（CRTP 内联，无虚调用）
    template <size_t... Is>
    void feed(const MarketEvent& ev, const BookContext& ctx, std::index_sequence<Is...>) {
        (std::get<Is>(strategies_).on_market(ev, ctx), ...);
    }
    template <size_t... Is>
    void on_segment_all(size_t seg, std::index_sequence<Is...>) {
        (std::get<Is>(strategies_).on_segment(seg), ...);
    }
    // 编译期展开：各策略 signal → ArbSignal 数组
    template <size_t... Is>
    void build_sigs(ArbSignal* sigs, std::index_sequence<Is...>) {
        ((sigs[Is] = from_signal(std::get<Is>(strategies_).signal())), ...);
    }
    static ArbSignal from_signal(const Signal& s) {
        return ArbSignal{s.side, s.locate, s.strength, s.timestamp};
    }

    std::tuple<Strategies...> strategies_;                    // 策略列表（编译期绑定）
    std::array<int64_t, sizeof...(Strategies)> weight_bp_{};  // 各策略投票权重（万分比）
    size_t primary_idx_ = 0;                                  // 主策略索引
    int64_t threshold_bp_ = 0;                                // 净投票阈值（万分比）
    ExecutionEngine* ex_ = nullptr;
    RiskManager* rm_ = nullptr;
    size_t id_ = 0;
    size_t nworkers_ = 1;
    uint64_t strategy_id_ = 1;
    uint64_t max_position_ = 10000;
    OrderSide last_side_ = OrderSide::NONE;
    int64_t last_str_ = -1;
    size_t cur_segment_ = 0;   // 当前段号（被动成交记录用）
};
