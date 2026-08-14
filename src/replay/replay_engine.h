#pragma once

// ── 回放引擎（★ 本仿真新增：分段驱动循环）──
// 从历史 ITCH 文件逐帧读委托事件，每条先进交易所撮合（ExchangeSimulator），
// 交易所广播给交易系统；每 segment_size 条调用一次段边界回调（策略插入窗口）。
//
// 时序不变量：单线程，一条委托"撮合 → 广播 → 订阅方应用"全部完成才处理下一条。

#include "exchange/exchange_simulator.h"
#include "exchange/itch_parser.h"
#include "replay/itch_replay_source.h"

#include <cstddef>
#include <cstdint>
#include <functional>

class ReplayEngine {
public:
    struct Config {
        std::string file;          // 历史 ITCH 二进制（2B 大端长度前缀帧）
        size_t segment_size = 1000;   // ★ 每段回放多少条 ITCH（策略插入节奏）
        size_t max_messages = 0;      // 0 = 全部
    };

    ReplayEngine(ItchReplaySource& src, nxex::ItchParser& parser, ExchangeSimulator& ex)
        : src_(src), parser_(parser), ex_(ex) {}

    // run：逐帧委托进交易所撮合→广播；每 segment_size 条调用一次 on_segment_boundary(seg)。
    void run(const Config& cfg, const std::function<void(size_t seg)>& on_segment_boundary);

    size_t messages_processed() const { return total_; }   // 解析的消息总数
    size_t order_events() const { return order_events_; }  // 订单事件数（进交易所）
    size_t segments() const { return segments_; }

private:
    ItchReplaySource& src_;
    nxex::ItchParser& parser_;
    ExchangeSimulator& ex_;
    size_t total_ = 0;
    size_t order_events_ = 0;
    size_t segments_ = 0;
};
