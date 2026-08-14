// ── NebulaX-Simulator 主入口（★ 本仿真新增，白名单 dispatch 吸收自 Trader）──
// 组装：交易所（撮合+广播）← 历史 ITCH 回放；交易系统（策略市场簿 + OMS/风控/执行）订阅广播。
// 分段回放：每 segment_size 条 ITCH 委托进交易所撮合并广播，段边界是策略插入窗口（下单/撤单）。
// 策略：编译期绑定（CRTP），config.strategy.strategies 白名单 dispatch 到 run_sim<...> 模板。
//
// 用法：./nebulaX_simulator [config.yaml]

#include "core/config.h"
#include "core/market_event.h"
#include "core/memory/order_map.h"
#include "core/memory/order_pool.h"
#include "market/book/order_book_consumer.h"
#include "oms/order_manager.h"
#include "risk/risk_manager.h"
#include "execution/execution_engine.h"
#include "exchange/exchange_simulator.h"
#include "replay/itch_replay_source.h"
#include "replay/replay_engine.h"
#include "sim/sim_worker.h"
#include "analysis/result_recorder.h"
#include "strategy/tick/order_book_imbalance_strategy.h"
#include "strategy/tick/order_flow_imbalance_strategy.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using OFI = OrderFlowImbalanceStrategy;
using OBI = OrderBookImbalanceStrategy;

// ── 运行一种策略组合（编译期绑定）──
// 策略在 tuple 中按 config.strategies 顺序（槽序 = 仲裁优先级，primary 在其中）。
template <class... Strategies>
int run_sim(const Config& cfg) {
    // 启动校验：非空策略组合时 primary 须在 strategies 白名单中（否则仲裁主策略槽缺失）。
    // 空列表 = 仅收行情不交易，跳过校验。
    if (!cfg.strategy.strategies.empty() && !cfg.strategy.primary.empty()) {
        const auto& st = cfg.strategy.strategies;
        if (std::find(st.begin(), st.end(), cfg.strategy.primary) == st.end()) {
            std::fprintf(stderr, "[error] strategy.primary '%s' 不在 strategies 白名单中\n",
                         cfg.strategy.primary.c_str());
            return 1;
        }
    }

    // ── 策略市场簿（广播订阅方，剔自单）──
    OrderPool market_pool(cfg.order_book.pool_slots);
    OrderMap market_index(cfg.order_book.pool_slots);
    OrderBookConsumer market_view(market_pool, market_index);

    // ── 交易所（委托进撮合 → 广播）──
    ExchangeSimulator exchange(cfg.order_book.pool_slots);
    exchange.set_market_view(&market_view);

    // ── 交易侧 OMS / 风控 / 执行 ──
    OrderManager om;
    om.set_next_id(cfg.replay.order_id_base);   // 策略订单 ref 高于历史最大 ref
    RiskManager rm;
    rm.set_max_position(cfg.risk.max_position);
    rm.set_max_daily_loss(cfg.risk.max_daily_loss);
    rm.set_initial_equity(cfg.risk.initial_equity);
    rm.set_max_drawdown_pause(cfg.risk.max_drawdown_pause);
    rm.set_max_drawdown_flatten(cfg.risk.max_drawdown_flatten);
    // 结果记录器（先于 ExecutionEngine 声明，供成交回调捕获）
    ResultRecorder recorder;
    ExecutionEngine ex(om, rm);
    ex.set_base_qty(cfg.execution.base_qty);
    ex.set_exchange(&exchange);
    exchange.set_execution(&ex);
    // 成交记录回调 → ResultRecorder（trades.csv）
    ex.set_trade_recorder([&](const TradeRecord& t) { recorder.add_trade(t); });

    // ── 策略 workers（每 worker 同一种策略组合，locate 分片）──
    const size_t nworkers = std::max<size_t>(1, cfg.order_book.workers);
    std::vector<std::unique_ptr<SimWorker<Strategies...>>> workers;
    workers.reserve(nworkers);
    for (size_t i = 0; i < nworkers; ++i) {
        auto w = std::make_unique<SimWorker<Strategies...>>();
        w->init(i, nworkers, /*strategy_id=*/1, cfg.risk.max_position, &ex, &rm,
                cfg.strategy);
        workers.push_back(std::move(w));
    }
    // 订阅交易所广播 → 每个 worker 更新信号（策略市场簿已剔自单）
    exchange.set_market_sink([&](const MarketEvent& ev) {
        for (auto& w : workers) w->on_market_event(ev, market_view);
    });

    // ── 分段回放：委托进交易所撮合 → 广播；段边界为策略插入窗口 ──
    ItchReplaySource src;
    nxex::ItchParser parser;
    ReplayEngine replay(src, parser, exchange);

    ReplayEngine::Config rcfg{cfg.replay.file, cfg.replay.segment_size, cfg.replay.max_messages};
    std::printf("[sim] replaying %s (segment_size=%zu, strategies=[",
                rcfg.file.c_str(), rcfg.segment_size);
    for (const auto& s : cfg.strategy.strategies) std::printf("%s,", s.c_str());
    std::printf("])...\n");
    // 段边界：策略插入窗口（下单/撤单），触发段号由 SimWorker 喂给 ExecutionEngine
    replay.run(rcfg, [&](size_t seg) {
        for (auto& w : workers) w->arbitrate(seg);
        // 段快照：策略累计成交（从 trades 计数）+ 净持仓
        uint64_t s_trades = recorder.trades().size();
        uint64_t s_filled = 0;
        int64_t net_pos = 0;
        om.iterate([&](uint64_t, const OrderManager::Entry& e) {
            s_filled += e.filled;
            const int64_t s = (e.order.side == OrderSide::BUY) ? 1 : -1;
            net_pos += s * (int64_t)e.filled;
        });
        recorder.add_snapshot({seg, rm.equity(), rm.drawdown(), rm.realized_pnl(),
                               net_pos, exchange.trade_count(),
                               exchange.strategy_resting(), s_trades, s_filled,
                               rm.realized_pnl()});
    });

    // ── 收尾：订单终态 ──
    om.iterate([&](uint64_t id, const OrderManager::Entry& e) { recorder.add_order_final(id, e); });

    recorder.write(cfg.analysis.out_dir, replay.messages_processed(), replay.order_events(),
                   replay.segments(), exchange.trade_count());

    // ── stdout 汇总（从 recorder 派生，与 summary.txt 一致）──
    size_t n_traded = 0;
    uint64_t qty_traded = 0;
    for (const auto& t : recorder.trades()) { ++n_traded; qty_traded += t.qty; }
    const auto& snaps = recorder.snapshots();
    int64_t max_dd = 0;
    for (const auto& s : snaps) if (s.drawdown > max_dd) max_dd = s.drawdown;
    const int64_t final_eq = snaps.empty() ? rm.equity() : snaps.back().equity;

    std::printf("=== NebulaX-Simulator ===\n");
    std::printf("file           : %s\n", cfg.replay.file.c_str());
    std::printf("messages       : %zu\n", replay.messages_processed());
    std::printf("order_events   : %zu\n", replay.order_events());
    std::printf("segments       : %zu\n", replay.segments());
    std::printf("engine trades  : %llu\n", (unsigned long long)exchange.trade_count());
    std::printf("strategy orders: %zu (traded %zu, qty %llu)\n",
                om.order_count(), n_traded, (unsigned long long)qty_traded);
    std::printf("strategy rest  : %zu\n", exchange.strategy_resting());
    std::printf("realized pnl   : %lld\n", (long long)rm.realized_pnl());
    std::printf("final equity   : %lld\n", (long long)final_eq);
    std::printf("max drawdown   : %lld\n", (long long)max_dd);
    std::printf("output         : %s/{equity,trades,orders}.csv summary.txt\n",
                cfg.analysis.out_dir.c_str());
    return 0;
}

int main(int argc, char** argv) {
    const std::string config_path = (argc > 1) ? argv[1] : "config/simulator.yaml";
    Config cfg = ConfigLoader::load(config_path);

    // 白名单组合（编译期绑定，不运行时插拔；槽序 = 仲裁优先级，首个为 primary）。
    // 空列表 = 仅收行情不交易。
    const auto& st = cfg.strategy.strategies;
    if (st == std::vector<std::string>{"ofi", "obi"}) return run_sim<OFI, OBI>(cfg);
    if (st == std::vector<std::string>{"ofi"})        return run_sim<OFI>(cfg);
    if (st == std::vector<std::string>{"obi"})        return run_sim<OBI>(cfg);
    if (st.empty()) {
        std::fprintf(stderr, "策略列表为空，仅收行情不交易\n");
        return run_sim<>(cfg);
    }
    std::fprintf(stderr, "不支持的策略组合(支持 [ofi,obi]/[ofi]/[obi]/[])\n");
    return 1;
}
