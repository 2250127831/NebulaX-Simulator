#include "replay/replay_engine.h"

void ReplayEngine::run(const Config& cfg,
                       const std::function<void(size_t seg)>& on_segment_boundary) {
    if (!src_.open(cfg.file)) {
        // 调用方负责检查，此处直接跳过（避免空跑）；sim_main 已提前校验文件存在
        return;
    }
    const uint8_t* body = nullptr;
    size_t len = 0;
    uint64_t seq = 0;
    size_t seg_msg = 0;
    while (src_.next(body, len, seq)) {
        if (cfg.max_messages && total_ >= cfg.max_messages) break;
        nxex::ItchEvent ev;
        if (parser_.feed(body, len, ev)) {
            ex_.on_market_order(ev);   // 委托进交易所撮合 → 广播
            ++order_events_;
        }
        ++total_;
        ++seg_msg;
        if (seg_msg >= cfg.segment_size) {
            if (on_segment_boundary) on_segment_boundary(segments_);
            ++segments_;
            seg_msg = 0;
        }
    }
    if (seg_msg > 0) {   // 收尾段（不足一段的尾部）
        if (on_segment_boundary) on_segment_boundary(segments_);
        ++segments_;
    }
}
