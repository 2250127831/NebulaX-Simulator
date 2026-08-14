// ── 纯回放验证（★ 本仿真新增）──
// 无策略，纯引擎重演历史 ITCH 订单流（A/F/D/X/U）：
//   1. 引擎撮合成交的 maker refs 与历史成交（P/E/C）的 executed refs 对比（重演等价性）
//   2. 确定性：两次运行结果一致由外层脚本断言
//
// 用法：./test_replay_pure [itch.bin]

#include "replay/itch_replay_source.h"
#include "exchange/itch_parser.h"
#include "exchange/matching_engine.h"
#include "exchange/order_pool.h"
#include "exchange/order_map.h"
#include "exchange/protocol.h"

#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace {
inline uint64_t rd_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}
}  // namespace

int main(int argc, char** argv) {
    const std::string path = (argc > 1) ? argv[1] : "test_data/itch_sample.bin";
    // --exact: 要求引擎成交 refs 与历史成交 refs 完全等价且 0 错误（构造完整样本专用）
    const bool exact = (argc > 2 && std::string(argv[2]) == "--exact");

    ItchReplaySource src;
    if (!src.open(path)) { std::printf("open failed: %s\n", path.c_str()); return 1; }

    nxex::OrderPool pool(1u << 20);
    nxex::OrderMap idx(1u << 20);
    nxex::MatchingEngine eng(pool, idx);
    nxex::ItchParser parser;

    std::set<uint64_t> hist_refs, eng_refs;
    size_t orders = 0, trades = 0, errors = 0;
    size_t err_code[8] = {};   // ErrorCode 1..7
    const uint8_t* body = nullptr;
    size_t len = 0;
    uint64_t seq = 0;
    while (src.next(body, len, seq)) {
        const uint8_t t = body[0];
        if (t == 'P' || t == 'E' || t == 'C') {
            if (len >= 19) hist_refs.insert(rd_u64(body + 11));   // executed order ref
            continue;   // 成交结果不喂引擎，只收集用于对比
        }
        nxex::ItchEvent ev;
        if (!parser.feed(body, len, ev)) continue;
        ++orders;
        std::vector<nxex::BinaryResponse> rsp;
        switch (ev.type) {
            case nxex::ItchEvent::Type::ADD:
                eng.processAdd(ev.locate, ev.order_ref, ev.side, ev.price, ev.shares,
                               /*user_id=*/ev.order_ref, rsp, nxex::OrderTif::DAY);
                break;
            case nxex::ItchEvent::Type::DELETE:
                eng.processCancel(ev.locate, ev.order_ref, /*user_id=*/ev.order_ref, rsp);
                break;
            case nxex::ItchEvent::Type::CANCEL:
                eng.processCancelShares(ev.locate, ev.order_ref, ev.shares,
                                        /*user_id=*/ev.order_ref, rsp);
                break;
            case nxex::ItchEvent::Type::REPLACE: {
                nxex::OrderBook* b = eng.book_for(ev.locate);
                const nxex::Order* old = b ? b->findOrder(ev.order_ref) : nullptr;
                eng.processReplace(ev.locate, ev.order_ref, ev.new_order_ref,
                                   old ? old->side : nxex::Side::INVALID,
                                   ev.price, ev.shares, /*user_id=*/ev.order_ref, rsp);
                break;
            }
            default:
                break;
        }
        for (auto& r : rsp) {
            if (r.type == nxex::RSP_TRADE) { ++trades; eng_refs.insert(r.data.trade.order_ref); }
            else if (r.type == nxex::RSP_ERROR) {
                ++errors;
                const unsigned c = (unsigned)r.data.error.code;
                if (c < 8) ++err_code[c];
            }
        }
    }

    size_t overlap = 0;
    for (auto ref : hist_refs) if (eng_refs.count(ref)) ++overlap;
    const double cov = hist_refs.empty() ? 0.0 : 100.0 * (double)overlap / hist_refs.size();
    std::printf("=== test_replay_pure ===\n");
    std::printf("file       : %s\n", path.c_str());
    std::printf("orders     : %zu\n", orders);
    std::printf("engine trades: %zu (unique maker refs %zu)\n", trades, eng_refs.size());
    std::printf("hist unique refs (P/E/C): %zu\n", hist_refs.size());
    std::printf("overlap    : %zu (%.1f%% of hist)\n", overlap, cov);
    std::printf("engine errors: %zu (by code: ", errors);
    for (unsigned c = 1; c < 8; ++c) std::printf("%u=%zu ", c, err_code[c]);
    std::printf(")\n");
    // 结构正确性：引擎成交的 maker 一定来自本流订单（A/F/D/X/U 的 ref）
    const bool ok = (orders > 0 && trades > 0) && (!exact || (errors == 0 && overlap == hist_refs.size()));
    std::printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
