# NebulaX-Simulator

> **V1**（2026-08）：单机、纯内存、确定性的市场微观结构仿真平台。

面向高频交易策略与低延迟交易系统的**市场微观结构仿真平台**。基于历史逐笔行情（ITCH）回放，模拟订单簿演化、策略决策、订单撮合与交易结果分析。

**单机 + 纯内存通信**：全链路同进程直接函数调用，无任何网络传输，速度更快、无网络复杂度。

版本计划见 [docs/VERSION_PLAN.md](docs/VERSION_PLAN.md)。

## 核心数据流（对齐现实量化流程）

```
委托流 ──► 交易所(撮合引擎) ──► 广播(Broadcast) ──► 交易系统(策略/OMS/风控 订阅)
策略订单 ────► 交易所 ──► 成交回报 ──► 交易系统
```

- **委托先到交易所撮合**：撮合引擎是唯一入口，是市场权威。
- **撮合后广播**：广播内容 = 委托事件（A/D/X/U）+ 成交事件（EXECUTE）+ 盘口。
- **交易系统订阅广播**：策略市场簿重建（剔自单）、信号计算、下单决策。
- **时序不变量**：单线程，一条委托"撮合 → 广播 → 订阅方应用"全部完成才处理下一条，杜绝乱序。

## 分段回放 + 策略插入窗口

- **`segment_size`** 控制策略交易节奏：每段回放 N 条 ITCH 委托进交易所撮合并广播，**段边界是策略的插入窗口**（下单 / 撤单）。
- `segment_size=1` 退化为逐事件决策。

## 两个源项目的组合

| 能力 | 来源 | 本仿真用途 |
|---|---|---|
| 撮合引擎（价格/时间优先、部分成交、撤改单、IOC/DAY） | **NebulaX**（迁移至 `src/exchange/`） | 交易所权威簿 |
| ITCH 解析（A/F/D/X/U） | **NebulaX** | 历史委托输入 |
| 策略框架（OFI/OBI）、订单簿消费者 | **NebulaX-Trader**（迁移至 `src/`） | 交易系统 |
| OMS / 风控 / 执行引擎 | **NebulaX-Trader**（ExecutionEngine 改接内存撮合） | 交易系统 |
| 网络层（io_uring/TCP/OUCH/WAL/共享内存） | — | **不迁移**（纯内存通信不需要） |

约束：两个源项目**本体只读**，所需组件**拷贝到本仓库后**修改。

## 构建

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

依赖：仅 `yaml-cpp` + Threads（C++20，Linux）。

## 运行

```bash
# 默认配置（回放 itch_sample.bin，segment_size=1000）
./build/nebulaX_simulator config/simulator.yaml
# 自定义配置
./build/nebulaX_simulator my_config.yaml
```

输出到 `results/`：`equity.csv`（净值/回撤/持仓曲线）、`orders.csv`（策略订单终态）、`summary.txt`。

### 配置要点

```yaml
replay:
  file: test_data/itch_sample.bin   # 历史 ITCH 二进制（2B 大端长度前缀帧）
  segment_size: 1000                # ★ 每段回放多少条 ITCH（策略插入节奏）
  order_id_base: 100000000000       # 策略订单 ref 基址（> 历史最大 ref）
strategy:
  strategies: [ofi, obi]            # 白名单策略组合（编译期绑定）：ofi / obi；[] = 仅收行情不交易
  primary: ofi                      # 主策略（须在 strategies 中；决定下单标的/seq，不单独定方向）
  weights: {ofi: 1.0, obi: 0.6}     # 投票权重（未列缺省 1.0；加权净投票定方向）
  vote_threshold: 0.0               # 净投票阈值（0 = 任一策略有信号即按净信号下；1.5 ≈ 要求全同向）
execution:
  base_qty: 100
```

### 可插拔策略（CRTP 编译期多态，吸收自 NebulaX-Trader）

策略继承 `StrategyT<Self>`，实现 `on_market(ev, ctx)` + `signal()`，白名单 dispatch 到 `run_sim<...>` 模板——无虚调用、无容器遍历，加策略不改框架：

```cpp
// src/strategy/base/strategy.h
struct BookContext {   // 框架每条事件算一次，传各策略（避免每策略查簿）
    const OrderBook* book; OrderSide side; int64_t mid, bid, ask; uint64_t bid_vol, ask_vol, seq;
};
template <class Impl>
class StrategyT {
public:
    void on_market(const MarketEvent& ev, const BookContext& ctx) { static_cast<Impl*>(this)->on_market(ev, ctx); }
    Signal signal() const { return static_cast<const Impl*>(this)->signal(); }
    void reset() {}                     // 可选覆写（OFI 清窗口）
    void on_segment(uint64_t) {}        // 段边界钩子（可选）
};
```

- 门控契约（策略实现必须遵守）：方向查不到的 D/X/E（`ctx.side==NONE`）需要方向的策略必须跳过；盘口无效（`ctx.book==null`）需要盘口的策略必须跳过。
- 仲裁：**加权净投票**（[arbitrate.h](src/strategy/base/arbitrate.h) 纯函数，原样复用 Trader）——`net = Σ(sign×weight)` 超阈值定方向，强度 = 同向策略加权平均，标的/seq = primary。
- 加新策略：写 `StrategyT<Self>` 派生类 + config 白名单 + `sim_main` 加一个 dispatch 分支。
- 支持组合：`[ofi,obi]` / `[ofi]` / `[obi]` / `[]`（空 = 仅收行情，修复了原 `use_*:false` 的"全停" bug）。

`test_data/` 下的样本是**软链**到 `~/NebulaX-Trader/test_data/`（本地路径，未提交）。可改用你自己的完整 ITCH 逐笔文件。

## 价格单位

全链路统一 **1/10000 元（tick）**——引擎撮合、策略簿、OMS 均价、风控 PnL/净值、配置资金值同单位。100 ticks = 1 分，故分制值 ×100 即 ticks（完全兼容）。PnL 单位 = tick × 股。策略/风控的资金配置（`initial_equity`、`max_daily_loss` 等）均按 tick 给值。

## 测试

```bash
# 单元/集成测试（ctest）
cd build && ctest --output-on-failure

# 端到端冒烟（合成样本，不依赖本地大样本）
./scripts/smoke_test.sh

# 性能压测（本地大样本：itch_sample.bin 软链）
./scripts/run_benchmark.sh test_data/itch_sample.bin

# 生成测试数据（CI 用；小样本已提交）
python3 scripts/gen_test_data.py test_data
```

- `replay_full_exact`：构造的完整样本（订单流+成交流一一对应）重演 **100% 等价、0 错误**——证明"完整 ITCH 下引擎重演 ≡ 历史成交"（设计决策 3）。
- `replay_pure_synth`：多 symbol 合成样本（`gen_test_data.py` 生成）纯回放结构正确性——CI 可用，不依赖本地大样本。
- `price_units`：价格单位 + PnL 数值正确性（买@100000 卖@100500 ×100 股 → realized 精确 = 50000）。
- `arbitrate`：加权净投票仲裁（同向/部分同意/权重反转/阈值观望/primary）。

实测结果：
- 确定性：同一配置两次运行逐字段一致。
- `itch_100mb.bin`（874 万条消息 / 845 万委托 / 25090 笔撮合成交）：单线程 2 分 45 秒。
- 迁移 bug 修复：NebulaX `processCancelShares` 部分撤单双扣 `remaining_qty`，已在本仓库 `OrderBook::reduceQtyCancel` 修复（原项目未动）。

## 目录结构

```
src/
├── exchange/    # 交易所侧（迁移自 NebulaX）：撮合引擎、订单簿/池/索引、ITCH 解析
│   ├── matching_engine.*  order_book.*  order_pool.h  order_map.h  itch_parser.*
│   ├── protocol.h  metrics.h  wal.*  logger.h（极简）
│   ├── i_exchange_simulator.h      # 内存交易所接口
│   └── exchange_simulator.*        # ★ 适配器：委托进撮合→广播→归属路由
├── core/        # [T] types.h、market_event.h、config.*、memory/order_pool.h、order_map.h
├── market/book/ # [T] 策略市场簿（OrderBookConsumer，剔自单）
├── strategy/    # [T] OFI / OBI 策略
├── oms/  risk/  execution/   # [T] 订单管理 / 风控 / 执行（改接内存撮合）
├── replay/      # ★ ItchReplaySource（帧迭代）+ ReplayEngine（分段驱动）
├── analysis/    # ★ ResultRecorder（净值/订单/汇总）
├── sim/         # ★ SimWorker（策略 worker，段边界仲裁下单）
└── sim_main.cpp # ★ 组装 + 运行 + 汇总
```

标注：`[T]`=NebulaX-Trader，`[N]`=NebulaX，`★`=本仿真新增。详细设计见 [docs/DESIGN.md](docs/DESIGN.md)。

## 开源仓库

- **License**: MIT（见 [LICENSE](LICENSE)）
- **CI**: GitHub Actions（[.github/workflows/ci.yml](.github/workflows/ci.yml)）—— 构建 + ctest（含生成测试数据），push/PR 触发
- **依赖**: C++20 + CMake + yaml-cpp（Linux）
- **关联项目**:
  - [NebulaX](https://github.com/2250127831/NebulaX) — 撮合引擎（本项目交易所侧来源，含撤单双扣减 bug 修复建议 [docs/NEBULAX_FIX_ADVICE.md](docs/NEBULAX_FIX_ADVICE.md)）
  - NebulaX-Trader — 交易框架（本项目策略/OMS/风控来源，CRTP 策略契约与加权净投票仲裁对齐）
- **测试数据**: 小样本（`test_data/itch_*.bin`）由 `scripts/gen_test_data.py` 生成并提交；大样本（`itch_sample.bin` 等）为本地软链，指向 `~/NebulaX-Trader/test_data/`，不提交。
