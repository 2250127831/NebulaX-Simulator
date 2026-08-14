# NebulaX-Simulator 设计文档

> 面向高频交易策略与低延迟交易系统的市场微观结构仿真平台。
> 基于历史逐笔行情（ITCH）回放，模拟订单簿演化、策略决策、订单撮合与交易结果分析。
> **单机 + 纯内存通信**：全链路同进程直接函数调用，无任何网络传输。

## 1. 定位与目标

本项目是"撮合引擎（NebulaX）+ 交易框架（NebulaX-Trader）"的仿真拼装：

- **交易所侧** = 真实撮合：价格优先 + 时间优先、部分成交、撤单、改单、市价（IOC）/限价（DAY）语义，来自 **NebulaX** 的 `MatchingEngine`。
- **交易侧** = 策略决策：市场簿重建、信号计算、OMS 订单生命周期、风控、执行，来自 **NebulaX-Trader** 的策略框架。
- **回放引擎** = 把历史 ITCH 文件切段回放，**每段回放 N 条，段边界给策略留出"插入窗口"**（下新单 / 撤挂单），形成可复现的确定性仿真。

### 核心数据流（对齐现实量化流程：委托 → 交易所撮合 → 广播 → 订阅）

现实中，委托先到交易所，交易所撮合后把市场状态**广播**给所有订阅者，交易系统**订阅广播**做决策，再把自己的订单发回交易所。本仿真严格按此流程：

```
历史委托流 ──► 撮合引擎（交易所）──► 广播（Broadcast）──► 交易系统（订阅）
策略订单   ────► 撮合引擎（交易所）──► 成交回报 ─────────► 交易系统（回报）
```

- **委托流是交易所的输入**（历史背景委托 + 策略订单），不是交易系统的直接输入。
- **撮合引擎就是交易所**：唯一入口，先撮合、后广播。
- **交易系统订阅广播**：广播内容 = 交易所撮合后的市场——委托事件 + 成交事件 + 盘口。注意 ITCH 广播协议本身就广播委托事件（Add/Delete/Cancel/Replace），交易系统正是从这份广播重建订单簿——与真实量化系统订阅逐笔行情一致。
- **时序由交易所驱动**：单线程、严格按序，杜绝乱序。

### 两个源项目的能力边界

| 能力 | NebulaX | NebulaX-Trader | 本仿真用哪个 |
|---|---|---|---|
| 撮合（价格/时间优先、部分成交、撤改单、IOC/FOK） | ✅ MatchingEngine | ❌ 只重建簿不撮合 | **NebulaX** |
| 订单簿/池/索引（OrderPool/OrderMap） | ✅ 交易所簿 | ✅ 策略侧簿（独立实现） | 两侧各用各的 |
| ITCH 解析（A/F/D/X/U/P/E/C） | ✅（只输出订单事件） | ✅（含成交事件 P/E/C） | **NebulaX-Trader**（唯一入口，转成引擎输入） |
| 策略框架（OFI/OBI/momentum…） | ❌ | ✅ | **NebulaX-Trader** |
| OMS / 风控 / 执行 | ❌ | ✅ | **NebulaX-Trader**（执行侧改接内存撮合） |
| WAL / 共享内存 / TCP / io_uring / OUCH | ✅ | ✅ | **全部丢弃**（单机内存通信不需要） |

## 2. 总体架构

```
                        ┌───────────────────────────────────────────────┐
                        │             NebulaX-Simulator                  │
                        │               （单进程、单线程、确定性）            │
                        │                                               │
  itch_*.bin            │   ┌───────────────────────────────────────┐  │
  (2B 大端长度前缀+body) │   │            ReplayEngine                │  │
 ────────────────▶ 逐帧  │   │   分段循环: 每 segment_size 条一段        │  │
                        │   │   每条 → 交易所撮合 → 广播 → 段边界插入    │  │
                        │   └───────────────────────────────────────┘  │
                        │                        │                      │
                        │           ItchEvent (A/F/D/X/U) 委托进交易所   │
                        │                        ▼                      │
                        │   ┌────────────────────────────────────────┐ │
                        │   │ ExchangeSimulator（★ 交易所+广播通道）     │ │
                        │   │  委托进 · 撮合 · 归属路由 · 市场广播       │ │
                        │   └───┬──────────┬────────────┬────────────┘ │
                        │       │①撮合      │②策略成交回报 │③市场广播      │
                        │       ▼          │            ▼              │
                        │  ┌────────────┐  │   ┌─────────────────────┐ │
                        │  │ Matching   │  │   │  策略行情视图(订阅)   │ │
                        │  │ Engine     │  │   │ (市场簿, 剔自单)      │ │
                        │  │ [NebulaX]  │  │   │ OrderBookConsumer    │ │
                        │  │ 权威簿      │  │   │ [NebulaX-Trader]    │ │
                        │  └────────────┘  │   └──────────┬──────────┘ │
                        │       │RSP_TRADE │              │Tick/BBO    │
                        │       │(maker)   │              ▼            │
                        │       └────┬─────┘   ┌────────────────────┐  │
                        │            │         │ 策略 (OFI/OBI/…)    │  │
                        │            │         │ 段边界 arbitrate    │  │
                        │            │         └────────┬───────────┘  │
                        │            │ 被动成交回报      │Signal        │
                        │            │  maker=策略单     ▼             │
                        │   ┌────────▼───────────┐  ExecutionEngine   │
                        │   │ ExecutionEngine    │  submit_signal     │
                        │   │ [T, 改造:接内存撮合] │                    │
                        │   │ OrderManager+Risk  │◄── 主动成交回报 ────│
                        │   └────────┬───────────┘                    │
                        │            │ 策略订单发回交易所               │
                        │            ▼                                │
                        │   ExchangeSimulator::submit → MatchingEngine │
                        │            │                                │
                        │   ┌────────▼───────────┐                    │
                        │   │ ResultRecorder      │ 成交/订单/净值/统计  │
                        │   └────────────────────┘                    │
                        └───────────────────────────────────────────────┘
```

### 端到端数据流（分段循环，单线程严格按序）

```
for each ITCH 委托消息（按文件序）:
    解析 → ItchEvent (A/F/D/X/U)         # P/E/C 不喂：引擎重演等价，喂了双计数
    ExchangeSimulator::on_market_order(ev)   # ── ① 委托进交易所撮合 ──
      MatchingEngine.processAdd/Cancel/CancelShares/Replace
        → [RSP_TRADE × N + 最终状态帧]   # RSP_TRADE 只指 maker（被动方）
      taker_fill = ev.shares - 最终残留     # taker 自身成交量，调用方追踪
      按 maker 归属：
        maker = 策略单 → ExecutionEngine::on_order_report（策略被动成交回报）
        maker = 市场单 → 只计数（交给 ② 广播的 EXECUTE）
    ExchangeSimulator::broadcast(ev, taker_fill)  # ── ② 交易所广播：委托事件+成交 ──
      ① 委托事件本身（A/D/X/U）→ 策略簿 + 信号
         # 广播含完整委托事件：即使 taker 立即成交也先入簿，OFI/簿重建需要完整委托流
      ② 撮合成交 → 策略簿 EXECUTE：
         市场 maker 被吃   → 按 maker order_ref 减量
         策略 maker 被吃   → 已在①回报 OMS，此处跳过（自单不在策略簿）
         taker 自身成交    → 按 ev.order_ref 减量 taker_fill
         # 净效果：策略簿残留 = 交易所权威簿中市场单残留，且不含策略单
      ③ 盘口/BEV 变化 → 策略簿 BBO（供 OBI）
    策略簿/信号更新完成后 → 下一条委托（时序严格）
    统计段内条数；段边界:                   # ── ③ 段边界插入窗口 ──
      每 symbol worker: arbitrate() → submit_signal
        ExecutionEngine: om.new_order → rm.check_order → ExchangeSimulator::submit
          MatchingEngine.processAdd(IOC=市价 / DAY=限价, user_id=策略ID)
          RSP_TRADE 全部 → om.on_fill / rm.on_fill + 策略簿 EXECUTE 市场 maker
          残留挂簿 → 注册策略订单表（等后续市场订单被动吃）
    记录本段快照（盘口/持仓/净值）
文件耗尽: 收尾统计 + 汇总报告
```

## 3. 关键设计决策

### 决策 1：委托 → 交易所撮合 → 广播 → 交易系统订阅（现实量化流程）

对齐现实：**委托先到交易所，撮合引擎处理，然后广播市场状态，交易系统订阅广播做决策**。委托流是交易所的**输入**（背景委托 + 策略订单），不是交易系统的直接输入；交易系统看到的是交易所**广播**的市场。对每条 ITCH 委托事件，处理顺序是：

1. **委托进交易所**（processAdd/Cancel/…）→ 引擎是唯一入口，撮合 + 更新权威簿；
2. **交易所广播**：把该委托的撮合结果广播给订阅者——广播含**委托事件**（A/D/X/U 本身，即便立即成交也广播）与**成交事件**（撮合成交）。策略簿重建与 OFI 订单流分析需要完整委托事件，所以广播全量；
3. **订阅方应用**：交易系统从广播重建簿（委托事件入簿、成交事件减 maker、taker 自身扣减）、更新信号；OMS 从回报通道收到策略订单的成交回报；
4. **策略信号更新**发生在该委托的全部效果（委托 + 成交）应用之后；
5. 下一条委托才开始。

不变量：**策略在任何时刻看到的，都是交易所已经撮合到该时刻的广播市场**——不存在"先看到原始委托、后看到成交"或乱序。策略订单在段边界发回交易所后立即撮合、立即回报 + 进广播，也遵守同一顺序。全链路单线程，无锁、无异步、无迟到事件。

一句话：**委托流进交易所，交易所广播市场，交易系统订阅广播**。

### 决策 2：权威簿（引擎）+ 策略市场簿（镜像，剔自单）

- **引擎权威簿**（NebulaX `MatchingEngine`）：**历史市场订单 + 策略订单**，撮合产生全部成交。策略挂单真实存在于盘中，会被后续历史订单被动成交——这正是 what-if 仿真的语义。
- **策略市场簿**（NebulaX-Trader `OrderBookConsumer`）：订阅交易所广播，接收**委托事件全量**（A/D/X/U 无条件入簿）+ **成交事件扣减**（市场 maker 减量 + taker 自身扣减），重建**市场单视图**，**剔除策略自身订单**。净效果 = 交易所权威簿中市场单的残留视图。理由：真实交易者的行情盘口不含自己的挂单（自己的订单状态从回报通道得知），信号不该受自己订单影响（避免自反馈）。
- 成交/仓位/PnL 以 OMS/Risk 为准。两套簿用**各自独立的 OrderPool/OrderMap**（NebulaX 版 `Order` 槽 vs Trader 版 `OrderSlot` 槽，目录隔离防头文件同名冲突）。

### 决策 3：历史成交 P/E/C 不喂，成交全部来自引擎撮合

ITCH 的 `P/E/C` 是**成交结果**，不是撮合输入；其 `executed_order_ref` 指向**被动方（maker）**，语义与引擎 `RSP_TRADE` 完全一致（`order_ref=maker、side=maker 方向、price=maker 价格`）。

- 完整 ITCH 文件（同一本书的订单+成交流）下，**引擎重演 A/F/D/X/U 产生的成交 ≡ 历史 P/E/C**——所以 P/E/C 直接跳过，交易系统看到的成交**全部来自引擎撮合**（策略簿 EXECUTE 减量 + OMS 成交回报），时序天然正确，无双计数。
- 若未来接入不完整的 L2 数据（如 `l2_replay_20260617.csv` 那种合成流，无对手盘），引擎重演成交会少于历史——此时策略看到的成交即"可撮合的部分"，文档记录该局限。

### 决策 4：成交归属路由（maker/taker 四象限）

引擎每次调用返回 `[RSP_TRADE × N + 最终状态帧]`，每笔 RSP_TRADE 的 `order_ref` 是 **maker**：

| taker | maker | 处理 |
|---|---|---|
| 市场单 | 市场单 | 市场内部成交 → 策略簿 EXECUTE（市场 maker 减量 + taker 自身扣减），只记录 |
| 策略单（主动下单） | 市场单 | 策略簿 EXECUTE（市场 maker 减量）+ **OMS 主动成交回报** |
| 市场单（后续历史订单） | 策略单 | **OMS 被动成交回报**（策略挂单被历史吃）→ 更新持仓/PnL，注销策略单；策略簿跳过（自单不在簿） |
| 策略单 | 策略单 | 被 `user_id` 自成交排除禁止（引擎按 taker 的 user_id 排除同属挂单） |

- **市场 taker**（回放驱动 `on_market_order`）：遍历 RSP_TRADE，`maker ∈ 策略订单表` → `on_order_report` 回灌 OMS/Risk（被动成交）；否则策略簿 EXECUTE（maker 减量）+ 记录；委托全量入簿 + taker 自身扣减（见 on_market_order 伪代码）。
- **策略 taker**（`ExchangeSimulator::submit`）：全部 RSP_TRADE → OMS 主动成交 + 策略簿 EXECUTE（市场 maker 减量）；残留挂簿 → 注册策略订单表。
- **最终状态帧**：`RSP_FILLED` 完成；`RSP_OK`（残留挂簿）→ 注册策略订单表；`RSP_ERROR` → `om.on_reject`。
- 策略订单语义映射：`OrderType::MARKET → tif=IOC`（全吃或作废），`OrderType::LIMIT → tif=DAY`（挂簿）。

### 决策 5：身份与自成交排除

| 订单来源 | 引擎 user_id | 引擎 order_ref |
|---|---|---|
| 历史市场订单 | `order_ref`（沿用原 ITCH server 约定） | 历史 ref |
| 策略订单 | `kStrategyUserId`（配置常量） | OMS `order_id`，OrderManager 起始 id = `order_id_base`（配置，> 历史最大 ref） |

- 自成交排除：策略作为 taker 时引擎按 `user_id=kStrategyUserId` 排除策略自身挂单；历史订单作为 taker 时按其自身 ref 排除，**不**排除策略挂单 → 历史订单可正常命中策略挂单（决策 4 的第三象限）。
- `order_id_base` 保证策略 ref 与历史 ref 不冲突，fill 归属可判定。

### 决策 6：价格单位统一 1/10000（tick）

- **全链路统一** ITCH 原始精度 1/10000 元（tick）：引擎权威簿 / 策略市场簿 / OMS 均价 / 风控盯市与 PnL / 配置资金值。
- 100 ticks = 1 分，故原"分"制值 ×100 即 ticks，完全兼容（用户要求）。
- PnL/净值单位 = tick × 股（1/10000 元 × 股）；配置 `initial_equity`/`max_daily_loss`/`max_drawdown_*` 同单位。
- 验证：`tests/test_price_units.cpp` 断言 PnL/均价/equity 精确整数（买@100000 卖@100500 ×100 股 → realized = 50000）。
- 附带修复：市价单 Risk 用 `order.price`（=0/信号价）会算错 avg_cost/PnL → ExecutionEngine 成交回报用**真实成交价** `fill_price` 回填后喂 Risk。

### 决策 7：分段插入语义

- `segment_size` 控制策略交易节奏：段内只处理行情（委托进交易所撮合 + 广播 + 信号更新），**段边界是策略的插入点**（下单 / 撤单）。
- 段边界 SimWorker 调 `arbitrate(seg)`：先触发各策略的 `on_segment(seg)` 钩子（插入窗口显式下单/撤单），再聚合信号下单。
- `segment_size=1` 退化为逐事件决策（仍遵守"委托→撮合→广播"顺序）。

### 决策 8：可插拔策略（CRTP 编译期多态 + 加权净投票仲裁，吸收自 Trader）

- **CRTP 契约**（[strategy.h](src/strategy/base/strategy.h)）：策略继承 `StrategyT<Impl>`，`on_market(ev, ctx)`/`signal()` 编译期绑定（可内联、无虚调用、无容器遍历）。`BookContext` 由框架每条事件算一次（side + BBO + mid）传各策略，避免每策略重复查簿。`on_segment(seg)` 段边界钩子。
- **门控契约搬进策略内部**：方向查不到的 D/X/E（`ctx.side==NONE`）需要方向的策略必须跳过（否则 DELETE 被误当卖侧撤污染窗口）；盘口无效（`ctx.book==null`）需要盘口的策略必须跳过。
- **加权净投票仲裁**（[arbitrate.h](src/strategy/base/arbitrate.h)，纯函数，原样复用 Trader）：`net_bp = Σ(sign(side)×weight_bp)` 超阈值定方向，强度 = 同向策略加权平均，标的/seq = primary 槽。config：`strategies` 白名单 + `primary` + `weights`（yaml double → 万分比定点）+ `vote_threshold`。
- **SimWorker 模板化**（[sim_worker.h](src/sim/sim_worker.h)）：`SimWorker<Strategies...>` 持 tuple + `std::index_sequence` 编译期展开（feed / on_segment / 构造 ArbSignal），仲裁调 `arbitrate_decide` 纯函数。单线程无需原子信号槽（直接读 signal()）。
- **白名单 dispatch**（`sim_main.cpp`）：`[ofi,obi]/[ofi]/[obi]/[]` → `run_sim<...>` 模板实例化。空列表 = 仅收行情不交易（修复原 `use_*:false` 的"全停" bug：旧 AND 仲裁无条件要求两槽同向，关一个 = 永不成交）。启动校验 primary ∈ strategies。
- **语义变化（对齐 Trader）**：仲裁从"AND 全同向"改为"加权净投票"——策略可部分同意，按权重定方向。实测 `itch_sample`：净投票触发更多交易（16 vs 14 订单），策略整体亏损 -2079 万 ticks（-2079 元，逐笔核对一致，非单位 bug）。

## 4. 目录结构与文件清单（来源标注）

```
NebulaX-Simulator/
├── CMakeLists.txt              # 仅依赖 yaml-cpp + Threads（不依赖 io_uring/libbpf/dpdk）
├── config/simulator.yaml       # 扩展自 Trader Config，新增 replay/analysis 段
├── docs/DESIGN.md
├── src/
│   ├── core/                   # [T] NebulaX-Trader
│   │   ├── types.h, market_event.h
│   │   ├── config.h, config_loader.cpp
│   │   └── memory/order_pool.h, order_map.h        # 策略市场簿的池
│   ├── market/
│   │   ├── parser/itch_parser.h/.cpp               # [T] MarketEvent 版，含 P/E/C
│   │   └── book/order_book.h, order_book_consumer.h/.cpp  # [T] 策略市场簿
│   ├── strategy/               # [T] base(strategy.h,signal.h) + tick/ 具体策略
│   ├── oms/order.h, order_manager.h                # [T]
│   ├── risk/risk_manager.h                         # [T]
│   ├── execution/execution_engine.h                # [T] 修改：sender/codec → IExchangeSimulator
│   ├── exchange/               # [N] NebulaX 撮合核心
│   │   ├── order.h, order_pool.h, order_map.h, trade_pool.h
│   │   ├── order_book.h/.cpp, matching_engine.h/.cpp
│   │   ├── protocol.h, metrics.h
│   │   ├── wal.h/.cpp          # 仅满足编译；运行时 wal_=nullptr 不启用
│   │   └── exchange_simulator.h/.cpp   # ★ 新增适配器：先撮合后推送 + 成交归属路由
│   ├── replay/
│   │   ├── itch_replay_source.h        # ★ 新增：2B 大端长度前缀帧迭代器
│   │   └── replay_engine.h/.cpp        # ★ 新增：分段驱动循环
│   ├── analysis/result_recorder.h/.cpp # ★ 新增：成交/订单/净值/统计
│   └── sim_main.cpp                    # ★ 新增：组装 + 运行 + 汇总
├── test_data/                  # 符号链接或拷贝自 NebulaX-Trader/test_data/
└── tests/
    ├── test_replay_source.cpp  # ★ 帧迭代 + 消息计数
    ├── test_replay_pure.cpp    # ★ 无策略纯回放：簿演化可复现
    └── test_sim_strategy.cpp   # ★ 策略下单→撮合→OMS 全链路
```

标注：`[T]`=NebulaX-Trader，`[N]`=NebulaX，`★`=本项目新写。

### 迁移约束（用户明确要求）

- **只改本仓库**：NebulaX / NebulaX-Trader 本体**只读**，不直接修改。
- **迁移再改**：所需组件**拷贝到本仓库 `src/` 后**，在本仓库内修改。
- **不迁网络层**：io_uring / TCP / OUCH / MoldUDP / AF_XDP / DPDK / WAL / 共享内存 / SPSC 环 / eventfd 等一律不迁移——本仿真走**纯内存函数调用**，更快且无网络复杂度。

## 5. ExchangeSimulator（★ 新增适配器，核心）

职责：交易所 + 广播通道。委托进、撮合、归属路由、市场广播。持有 `MatchingEngine` 权威簿、策略订单注册表、以及指向策略行情视图（订阅方）/ ExecutionEngine 的回调。

```cpp
class ExchangeSimulator {
public:
    ExchangeSimulator(size_t pool_capacity);          // 引擎自建池/索引（单线程无需共享内存）

    // ── 市场侧：每条历史 ITCH 委托进交易所（先撮合，后广播）──
    void on_market_order(const ItchEvent& ev);        // A/F/D/X/U → 引擎撮合 → 归属路由广播

    // ── 策略侧：策略订单发回交易所（段边界调用）──
    struct SubmitResult {
        uint64_t order_id; bool accepted;             // 是否挂簿/成交
        std::vector<Fill> fills;                      // 每笔成交（价/量）→ OMS
        bool fully_filled;
    };
    void submit(const Order& order, uint64_t order_id,
                OrderTif tif, SubmitResult& out);     // → 引擎 processAdd(IOC/DAY)
    void cancel(uint64_t order_id, uint64_t user_id); // → 引擎 processCancel

    // ── 广播目标（订阅方，组装时绑定）──
    void set_market_view(MarketView* view);           // 策略行情视图（订阅广播 ADD/…/EXECUTE + BBO）
    void set_execution(ExecutionEngine* ex);          // 收策略被动/主动成交回报

    // ── 只读：策略订单注册表（maker 归属判定）──
    bool is_strategy_order(uint64_t order_ref) const;
    const OrderBook* book(uint64_t locate) const;     // 引擎权威簿（供结果分析/盘口快照）
private:
    MatchingEngine engine_;
    std::unordered_set<uint64_t> strategy_refs_;      // 策略订单注册表
    // ... 归属路由逻辑
};
```

`on_market_order` 的完整逻辑（委托进交易所撮合 → 广播委托事件 + 成交事件）：

```cpp
void ExchangeSimulator::on_market_order(const ItchEvent& ev) {
    // ── ① 委托进交易所撮合：引擎是唯一入口 ──
    std::vector<BinaryResponse> rsp;
    switch (ev.type) {
        case ItchEvent::ADD:
            engine_.processAdd(ev.locate, ev.order_ref, ev.side, ev.price,
                               ev.shares, /*user_id=*/ev.order_ref, rsp, OrderTif::DAY);
            break;
        case ItchEvent::DELETE:
            engine_.processCancel(ev.locate, ev.order_ref, /*user_id=*/ev.order_ref, rsp);
            break;
        case ItchEvent::CANCEL:
            engine_.processCancelShares(ev.locate, ev.order_ref, ev.shares,
                                        /*user_id=*/ev.order_ref, rsp);
            break;
        case ItchEvent::REPLACE:
            engine_.processReplace(ev.locate, ev.order_ref, ev.new_order_ref, /*side=*/查旧单,
                                   ev.price, ev.shares, /*user_id=*/ev.order_ref, rsp);
            break;
        default: return;
    }
    // taker 自身成交量 = 委托量 - 最终残留（引擎 RSP_TRADE 只报 maker，taker 由调用方追踪）
    uint64_t taker_fill = calc_taker_fill(ev, rsp);

    // ── ② 交易所广播：委托事件 + 成交事件（严格按序，应用完才处理下一条）──
    broadcast(to_market_event(ev));                  // ①委托事件全量：ADD/DELETE/CANCEL/REPLACE
    for (auto& r : rsp) if (r.type == RSP_TRADE) {   // ②撮合成交事件
        if (is_strategy_order(r.data.trade.order_ref)) {   // maker = 策略单 → OMS 被动回报
            ex_->on_order_report(Fill{/*type='E', order_id=maker_ref, qty, price*/});
        } else {                                     // maker = 市场单 → 广播 EXECUTE(maker)
            broadcast(make_execute(ev.locate, r.data.trade.order_ref,
                                   r.data.trade.quantity));
        }
    }
    if (taker_fill > 0)                              // ③taker 自身成交 → 广播 EXECUTE(taker)
        broadcast(make_execute(ev.locate, /*order_ref=*/taker_ref(ev), taker_fill));
}
```

- **广播含完整委托事件**：`to_market_event(ev)` 无条件广播——即使 taker 立即成交也先入簿，再由 ③ 扣减，与真实 ITCH 广播（A 先广播、P 随后广播成交扣减）一致，策略簿重建精确、OFI 拿到完整委托事件。
- **taker 自身成交**：`taker_fill = ev.shares − 最终残留`（对 ADD/REPLACE 是成交量；DELETE/CANCEL 恒为 0）。REPLACE 的 taker 是 `new_order_ref`。
- **自单自动剔除**：策略订单从未经 `broadcast` 入簿，其作为 maker 的成交走 ① 的 OMS 回报分支而不进广播减量 → 策略簿天然不含策略单。
- **MarketView** = Trader 的 `OrderBookConsumer`（复用 `handle_add/delete/cancel/replace/execute`），订阅方从解析器换成 ExchangeSimulator 的广播。

## 6. 执行引擎改造（唯一需要动旧代码的地方）

`execution/execution_engine.h` 的 `submit_signal` 中，原"无 sender → 进程内立即 accept + 全额成交"存根，替换为接内存撮合：

```cpp
} else {
    // 无网络：接 ExchangeSimulator（真实撮合），不走 OUCH/TCP
    order_manager_.on_accept(id);                    // PENDING → SUBMITTED
    ExchangeSimulator::SubmitResult r;
    exchange_->submit(order, id, tif, r);            // 纯撮合
    for (auto& f : r.fills) {                        // 主动成交 → OMS/Risk（锁内，同原存根）
        order_manager_.on_fill(id, f.qty, f.price);
        risk_manager_.on_fill(order, f.qty);
    }
    if (!r.accepted) order_manager_.on_reject(id);
}
```

新增成员 `IExchangeSimulator* exchange_`（`set_exchange(...)`）。`cancel_order` 同样从 OUCH 发送改为 `exchange_->cancel(order_id)` → `processCancel` → `om.on_cancel`。其余 OMS/Risk/成交回报逻辑原样复用（`on_order_report` 由 ExchangeSimulator 在被动/主动成交时直接调用）。

## 7. 配置（simulator.yaml，示意）

```yaml
replay:
  file: test_data/itch_sample.bin    # ITCH 二进制（2B 大端长度前缀帧）
  segment_size: 1000                 # ★ 每段回放多少条 ITCH（策略插入节奏）
  max_messages: 0                    # 0 = 全部
  order_id_base: 100000000000        # 策略订单 ref 基址（> 历史最大 ref）
order_book:
  enabled: true
  pool_slots: 1048576
  workers: 1                         # 单线程仿真
strategy:
  use_obi: true
  use_ofi: true
risk:
  max_position: 10000
  max_daily_loss: 100000000
  initial_equity: 100000000
execution:
  base_qty: 100
analysis:
  out_dir: results/                  # trades.csv / orders.csv / equity.csv / summary
```

## 8. 结果分析

- **成交录**：市场内部成交 + 策略成交（主动/被动），字段：时间/segment/order_ref/side/价格/数量/maker-taker 归属。
- **订单录**：每笔策略订单全生命周期（提交时间/价格/数量/状态/已成交/均价/触发段号）。
- **净值曲线**：每 segment 的持仓、已实现 + 浮动盈亏、回撤（复用 `RiskManager::equity/drawdown`）。
- **汇总**：总 PnL、成交笔数、胜率、成交率（提交→成交）、均价滑点 vs 盘中价、最大回撤、期末持仓。

## 9. 验证与测试计划

1. **委托→撮合→广播时序**：单线程 + 交易所单一入口，通过代码路径保证；测试断言"策略收到的每个广播事件，其撮合都已被引擎完成"。
2. **纯回放正确性**：无策略时，回放 `itch_test_small.bin` 应复现历史簿演化（有 `itch_test_small.expected.bin` 可对照）；且引擎撮合成交与历史 P/E/C 一致（抽样比对），无策略即无策略成交。
3. **确定性**：同一 `(文件, segment_size, 配置)` 两次运行结果逐字段一致。
4. **全链路**：策略下单（市价/限价）→ 撮合 → 主动/被动成交 → OMS 状态机 → 风控持仓/PnL 正确；`segment_size=1` 与 `N` 均能跑通。
5. **性能冒烟**：`itch_100mb.bin`（257MB）跑通并记录耗时（单线程回放吞吐基线）。

## 10. 实施步骤

1. 骨架：CMake + 拷贝最小文件集（先只编译交易所侧）→ 编译通过。
2. `ReplaySource` + `ExchangeSimulator`（委托进交易所撮合 + 广播 + 归属路由）→ 纯回放无策略可跑（验证簿演化 + 与历史成交比对）。
3. 交易侧接入：策略市场簿（`MarketView`）+ 策略 + 改造 `ExecutionEngine` + OMS/Risk → 策略可下单并撮合。
4. 段边界插入 + `ResultRecorder` + 汇总。
5. 用 `itch_test_small.bin` / `itch_sample.bin` 验证 + 性能冒烟。

## 11. 已知取舍

- **价格单位统一 1/10000**（决策 6）：全链路 tick 口径，资金配置同单位（×100 即原分制）。引擎/策略簿原本已 ticks，修复点在 OMS/Risk 用真实成交价 + 配置值口径。
- **历史 P/E/C 不喂**（决策 3）：基于"完整 ITCH 文件 + 引擎重演等价"的假设；不完整 L2 数据下策略成交为"可撮合部分"。
  - 验证：构造的完整样本（`test_data/itch_full_sample.bin`，订单流+成交流一一对应）重演 **100% 等价、0 错误**（`test_replay_pure --exact`）。
  - 真实样本（`itch_100mb.bin`）unique-ref 覆盖 ~55%：引擎成交笔数（修复后 ~2.5 万）与历史成交（4.5 万条 P/E/C ≈ 2.2 万笔）吻合，剩余差异为样例数据不完整（约 45% 历史成交订单在流中缺 taker 对手单或先被撤/删）。
- **迁移 bug 修复（本仓库）**：NebulaX `processCancelShares` 部分撤单双扣 `remaining_qty`（先手动减，再经 `reduceOrderQty` 内部再减且误加 `filled_qty`）。本仓库加 `OrderBook::reduceQtyCancel`（只减剩余+档量，不加 filled）修复。原项目本体未动。
- **策略市场簿剔自单**（决策 2）：策略看到的是不含自己挂单的市场盘口；若需要"全景盘口"（含自己订单）可在 MarketView 增加配置开关。
- 策略单 symbol 状态：`BookWorker` 每 worker 一个 OFI/OBI 实例，跟踪最近事件的 locate（继承原设计）；多 symbol 按 Dispatcher 分片拆多个 worker，单线程内顺序执行。
- WAL/崩溃恢复不做（仿真不需要持久化恢复）。
