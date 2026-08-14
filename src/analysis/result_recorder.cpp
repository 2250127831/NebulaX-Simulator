#include "analysis/result_recorder.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

namespace {
const char* side_name(OrderSide s) {
    return s == OrderSide::BUY ? "BUY" : (s == OrderSide::SELL ? "SELL" : "NONE");
}
const char* status_name(OrderStatus st) {
    switch (st) {
        case OrderStatus::PENDING: return "PENDING";
        case OrderStatus::SUBMITTED: return "SUBMITTED";
        case OrderStatus::PARTIAL_FILL: return "PARTIAL_FILL";
        case OrderStatus::PENDING_CANCEL: return "PENDING_CANCEL";
        case OrderStatus::FILLED: return "FILLED";
        case OrderStatus::CANCELLED: return "CANCELLED";
        case OrderStatus::REJECTED: return "REJECTED";
        default: return "?";
    }
}
}  // namespace

bool ResultRecorder::write(const std::string& out_dir,
                           size_t messages, size_t order_events, size_t segments,
                           uint64_t engine_trades) const {
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);

    // ── equity.csv（含策略累计成交指标）──
    {
        FILE* f = std::fopen((out_dir + "/equity.csv").c_str(), "w");
        if (!f) return false;
        std::fprintf(f, "segment,equity,drawdown,realized_pnl,net_position,"
                        "engine_trades,strategy_resting,strategy_trades,strategy_filled,strategy_pnl\n");
        for (const auto& s : snapshots_) {
            std::fprintf(f, "%zu,%lld,%lld,%lld,%lld,%llu,%zu,%llu,%llu,%lld\n",
                         s.segment, (long long)s.equity, (long long)s.drawdown,
                         (long long)s.realized_pnl, (long long)s.net_position,
                         (unsigned long long)s.engine_trades, s.strategy_resting,
                         (unsigned long long)s.strategy_trades,
                         (unsigned long long)s.strategy_filled,
                         (long long)s.strategy_pnl);
        }
        std::fclose(f);
    }

    // ── trades.csv（策略逐笔成交明细）──
    {
        FILE* f = std::fopen((out_dir + "/trades.csv").c_str(), "w");
        if (!f) return false;
        std::fprintf(f, "segment,seq,order_id,symbol,side,qty,price,mid,slip,passive\n");
        for (const auto& t : trades_) {
            // slip：成交价 vs 中间价（方向感知：BUY 正滑点=买贵，SELL 负滑点=卖低）
            const int64_t slip = (t.side == OrderSide::BUY) ? (t.price - t.mid)
                                                            : (t.mid - t.price);
            std::fprintf(f, "%llu,%llu,%llu,%llu,%s,%llu,%lld,%lld,%lld,%d\n",
                         (unsigned long long)t.segment, (unsigned long long)t.seq,
                         (unsigned long long)t.order_id, (unsigned long long)t.symbol_id,
                         side_name(t.side), (unsigned long long)t.qty,
                         (long long)t.price, (long long)t.mid, (long long)slip,
                         t.passive ? 1 : 0);
        }
        std::fclose(f);
    }

    // ── orders.csv ──
    {
        FILE* f = std::fopen((out_dir + "/orders.csv").c_str(), "w");
        if (!f) return false;
        std::fprintf(f, "order_id,symbol,side,qty,type,status,filled,remaining,avg_fill_price\n");
        for (const auto& [id, e] : orders_) {
            std::fprintf(f, "%llu,%llu,%s,%llu,%d,%s,%llu,%llu,%lld\n",
                         (unsigned long long)id, (unsigned long long)e.order.symbol_id,
                         side_name(e.order.side), (unsigned long long)e.order.quantity,
                         (int)e.order.type, status_name(e.status),
                         (unsigned long long)e.filled, (unsigned long long)e.remaining,
                         (long long)e.avg_fill_price);
        }
        std::fclose(f);
    }

    // ── summary.txt（含绩效指标，从净值序列派生）──
    {
        FILE* f = std::fopen((out_dir + "/summary.txt").c_str(), "w");
        if (!f) return false;

        // 策略汇总（从 trades/orders 派生）
        uint64_t strat_trades = 0, strat_filled = 0, passive = 0, active = 0;
        int64_t slip_sum = 0, slip_cnt = 0;
        for (const auto& t : trades_) {
            ++strat_trades; strat_filled += t.qty;
            if (t.passive) ++passive; else ++active;
            if (t.mid > 0) { slip_sum += (t.side == OrderSide::BUY) ? (t.price - t.mid)
                                                                    : (t.mid - t.price);
                             ++slip_cnt; }
        }
        const double avg_slip = slip_cnt ? (double)slip_sum / slip_cnt : 0.0;
        const int64_t realized = snapshots_.empty() ? 0 : snapshots_.back().realized_pnl;

        // 订单统计
        size_t n_orders = orders_.size(), n_filled = 0, n_rejected = 0, n_cancelled = 0;
        for (const auto& [id, e] : orders_) {
            if (e.status == OrderStatus::FILLED) ++n_filled;
            else if (e.status == OrderStatus::REJECTED) ++n_rejected;
            else if (e.status == OrderStatus::CANCELLED) ++n_cancelled;
        }
        const double fill_rate = n_orders ? 100.0 * n_filled / n_orders : 0.0;

        // ── 绩效指标（从净值序列派生）──
        // 段收益率 r_t = (equity_t - equity_{t-1}) / equity_{t-1}；仅取 equity>0 且非 0 序列。
        std::vector<double> rets;
        rets.reserve(snapshots_.size());
        for (size_t i = 1; i < snapshots_.size(); ++i) {
            const int64_t prev = snapshots_[i-1].equity, cur = snapshots_[i].equity;
            if (prev > 0) rets.push_back((double)(cur - prev) / (double)prev);
        }
        double mean = 0.0, sd = 0.0, downside = 0.0;
        double sum_pos = 0.0, sum_neg = 0.0; int64_t n_pos = 0, n_neg = 0;
        if (!rets.empty()) {
            for (double r : rets) mean += r;
            mean /= rets.size();
            for (double r : rets) { sd += (r - mean) * (r - mean); }
            sd = std::sqrt(sd / rets.size());
            for (double r : rets) {
                if (r > 0) { sum_pos += r; ++n_pos; }
                else if (r < 0) { sum_neg += -r; ++n_neg; }
                if (r < 0) downside += r * r;
            }
            downside = std::sqrt(downside / rets.size());
        }
        const double sharpe = (sd > 0) ? mean / sd : 0.0;                 // 段级夏普（无风险 0）
        const double sortino = (downside > 0) ? mean / downside : 0.0;
        // 卡玛：累计收益 / 最大回撤（净值级）
        const int64_t init = snapshots_.empty() ? 0 : snapshots_.front().equity;
        const int64_t final_eq = snapshots_.empty() ? 0 : snapshots_.back().equity;
        int64_t peak = 0, max_dd = 0;
        for (const auto& s : snapshots_) {
            if (s.equity > peak) peak = s.equity;
            const int64_t dd = peak - s.equity;
            if (dd > max_dd) max_dd = dd;
        }
        const double total_ret = (init > 0) ? (double)(final_eq - init) / (double)init : 0.0;
        const double calmar = (max_dd > 0) ? total_ret / (double)max_dd : 0.0;
        const double win_rate = (n_pos + n_neg) ? 100.0 * n_pos / (n_pos + n_neg) : 0.0;
        const double profit_factor = (sum_neg > 0) ? sum_pos / sum_neg : (sum_pos > 0 ? 999.0 : 0.0);

        std::fprintf(f, "=== NebulaX-Simulator Summary ===\n");
        std::fprintf(f, "messages_processed : %zu\n", messages);
        std::fprintf(f, "order_events       : %zu\n", order_events);
        std::fprintf(f, "segments           : %zu\n", segments);
        std::fprintf(f, "engine_trades      : %llu\n", (unsigned long long)engine_trades);
        std::fprintf(f, "orders_total       : %zu (filled %zu, rejected %zu, cancelled %zu)\n",
                     n_orders, n_filled, n_rejected, n_cancelled);
        std::fprintf(f, "fill_rate          : %.2f%%\n", fill_rate);
        std::fprintf(f, "strategy_trades    : %llu (active %llu, passive %llu)\n",
                     (unsigned long long)strat_trades, (unsigned long long)active,
                     (unsigned long long)passive);
        std::fprintf(f, "strategy_filled    : %llu\n", (unsigned long long)strat_filled);
        std::fprintf(f, "avg_slip           : %.4f ticks\n", avg_slip);
        std::fprintf(f, "realized_pnl       : %lld\n", (long long)realized);
        std::fprintf(f, "final_equity       : %lld\n", (long long)final_eq);
        std::fprintf(f, "max_drawdown       : %lld\n", (long long)max_dd);
        std::fprintf(f, "\n-- 绩效指标（净值序列派生，段级，无年化）--\n");
        std::fprintf(f, "total_return       : %.4f%%\n", 100.0 * total_ret);
        std::fprintf(f, "sharpe(segment)    : %.4f\n", sharpe);
        std::fprintf(f, "sortino(segment)   : %.4f\n", sortino);
        std::fprintf(f, "calmar             : %.4f\n", calmar);
        std::fprintf(f, "win_rate(segments) : %.2f%%\n", win_rate);
        std::fprintf(f, "profit_factor      : %.4f\n", profit_factor);
        std::fprintf(f, "ret_mean/std       : %.6f / %.6f\n", mean, sd);
        std::fprintf(f, "\n说明: 段级指标未年化；segment=N 条消息非固定时间。用户可用 equity.csv 自行 Python 分析。\n");
        std::fclose(f);
    }
    return true;
}
