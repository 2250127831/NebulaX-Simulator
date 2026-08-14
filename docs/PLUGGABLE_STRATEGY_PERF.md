# 可插拔策略重构 —— 性能与可行性评估材料

> 目的：NebulaX-Simulator 做了一次"可插拔策略"重构，整理如下，供 NebulaX-Trader（低延迟真实交易系统）的负责方评估**性能影响**与**移植可行性**。
> 结论先行：**在单线程仿真上下文中，本次重构的开销可忽略（实测 <2%）；但重构引入的通用化模式（虚函数、STL 容器、按需查簿）与 Trader 生产热路径的"硬编码 + 无虚 + 原子信号槽"风格相悖，若照搬会损失性能。** 本文给出分点分析和替代方案。

> **2026-08 更新（已按 Trader 升级对齐）**：本仓库已吸收 Trader 的 **CRTP 编译期多态** 方案，替代本文评估的虚函数+注册版：
> - `StrategyT<Impl>` + `BookContext`（框架算一次 side/BBO/mid 传各策略，无虚调用、无容器遍历）
> - `arbitrate.h` 加权净投票纯函数（**原样复用 Trader**）
> - `SimWorker<Strategies...>` 模板化 + `std::index_sequence` 编译期展开
> - config `primary`/`weights`/`vote_threshold` 仲裁配置化
> 详见 [DESIGN.md 决策 8](DESIGN.md)。本文其余部分为"虚函数版"的历史评估，保留供对照。

---

## 1. 重构内容摘要

把策略从"硬编码两个实例"改为"统一接口 + 注册"：

- **`Strategy` 基类统一接口**（[src/strategy/base/strategy.h](src/strategy/base/strategy.h)）：

```cpp
class Strategy {
public:
    virtual ~Strategy() = default;
    virtual void on_market(const MarketEvent& ev, const OrderBook* book) = 0;  // 行情事件
    virtual Signal signal() const = 0;                                        // 当前信号
    virtual void on_segment(uint64_t segment) {}                              // 段边界钩子（可选）
};
```

- **SimWorker 持策略列表**（[src/sim/sim_worker.h](src/sim/sim_worker.h)）：`std::vector<std::unique_ptr<Strategy>>` + `add_strategy()`；每条广播事件遍历调 `on_market`；段边界 `arbitrate(seg)` 遍历所有策略 `signal()` 聚合（全部同向才下单，以第一个注册策略为准）。
- **内置策略工厂**（[src/sim_main.cpp](src/sim_main.cpp) `make_builtin_strategy`）：config `strategies: [ofi, obi]` 按名注册。加新策略 = 写派生类 + 工厂加一个名字分支。
- **OFI / OBI 适配**：方向推导（D/X/E 查簿 side）、BBO 取用从框架挪进策略内部。

---

## 2. 引入的性能特征（逐点）

### 2.1 每条行情事件 = 遍历策略列表 + 虚函数分派

热路径（每条广播事件）：
```
ExchangeSimulator::broadcast(ev)
  → view_->on_event(ev)              // OrderBookConsumer 重建簿（不变）
  → sink_(ev)                        // std::function
     → for (worker) worker->on_market_event(ev)
        → for (strategy) strategy->on_market(ev, book)   // ★ 虚函数 × N
```

- 虚调用本身 ~1-2ns（多数处理器分支预测友好），`std::vector` 遍历是连续内存，cache 友好。
- **代价随策略数 N 线性增长**；每次虚调用还要传 `book` 指针、重取 BBO。
- 原 Trader `BookWorker` 是**硬编码两个具体策略**（OFI/OBI 各一个成员），编译期绑定，无虚调用、无遍历。

### 2.2 按需查簿重复化（重要差异）

原 Trader `BookWorker::process` 对**每条事件只查一次方向**（`side_of`），算出 `side` 后传给 OFI；OBI 则直接喂 BBO。

重构后 `on_market(ev, book)` 让**每个策略各自查簿**：
- OFI 对 D/X/E 查 `book->side_of(ref)`；OBI 每次查 `best_bid()/best_ask()/best_bid_volume()/best_ask_volume()`。
- **N 个策略 = N 次查簿**（每次含 OrderMap 查找或 BBO 极值缓存读）。
- 单线程下查簿是 O(1)+cache 命中，可忽略；多线程生产下这是重复的 cache 访问。

### 2.3 聚合仲裁：遍历 signal() 虚调用

原 Trader 用**原子信号槽**（`SignalSlot`：side/locate/strength/seq 四个 `alignas(64) std::atomic`）跨线程传信号，arbitrate 只读两个原子。

重构后 `arbitrate` 遍历所有策略 `signal()`（虚调用 + 读策略状态，可能跨 cache 行）。单线程无并发问题；多线程下不如原子槽 cache 友好（原子是单条 cache 行，遍历状态是散布的多条）。

### 2.4 新增 STL 组件

| 组件 | 位置 | 热路径? | 特征 |
|---|---|---|---|
| `std::vector<std::unique_ptr<Strategy>>` | SimWorker | 遍历时 | 连续内存，无 per-event 分配 |
| `std::function<void(const MarketEvent&)>` | ExchangeSimulator::sink_ | 每条事件 | 小函数对象/间接调用，一般可接受 |
| `std::unordered_set<uint64_t> strategy_refs_` | ExchangeSimulator | 每笔撮合 | 归属判定（被动成交路由），非逐委托热路径 |
| 虚函数 `on_market`/`signal` | 策略 | 每条事件 | 见 2.1 |

### 2.5 实测数据点（本仓库 `itch_sample.bin`，442k 消息 / 198k 委托，单线程）

| 配置 | 耗时 |
|---|---|
| 空策略 `[]` | 3.61s |
| `[ofi, obi]` | 3.67s |
| **策略层开销** | **~60ms ≈ 1.6%** |

**单线程仿真上下文：可忽略。** 主要耗时在撮合（`std::map` 价格档 + 内存池）与 415 个 symbol 各自的簿重建，策略层占比很小。

---

## 3. 与原 Trader 实现的对照

| 维度 | 原 Trader `BookWorker` | 本重构 |
|---|---|---|
| 策略绑定 | 硬编码两个具体策略（成员） | 列表 + 注册 |
| 行情入口 | `BookWorker::process(ev)` 直调专用方法 | 虚函数 `on_market(ev, book)` |
| 方向推导 | 框架算一次 `side` 传下去 | 每个策略自取（N 次） |
| 信号传递 | 原子 `SignalSlot`（跨线程） | `signal()` 虚调用遍历 |
| 加新策略 | 改 `BookWorker` 源码 | 写派生类 + 工厂加名字 |
| cache 特征 | 紧致成员，编译器可内联 | 容器+虚调用，内联受限 |

**本质权衡**：硬编码换性能，可插拔换扩展性。原实现的性能优势在于编译期绑定（内联、无间接跳转、信号原子槽）；可插拔的优势在于加策略不改框架。

---

## 4. 移植到生产（多线程低延迟）的评估与建议

本重构**不直接适合** Trader 生产热路径。若 Trader 想获得可插拔能力，建议：

### 4.1 关键路径保留硬编码/编译期绑定

- 高频热路径（每条委托的簿重建、方向推导、信号更新）**保持原 `BookWorker` 硬编码**，不要为可插拔而虚函数化。
- 可插拔能力放在**低频入口**：策略工厂在启动时把 `Strategy*` 绑定进 worker，热路径仍走专用方法（CRTP / 接口分派在编译期展开）。

### 4.2 若必须统一接口，用编译期多态替代虚函数

```cpp
// CRTP：on_market 编译期绑定，可内联，无虚调用
template <class Impl>
struct StrategyT {
    void on_market(const MarketEvent& ev, const OrderBook* b) { static_cast<Impl*>(this)->on_market(ev, b); }
};
```
代价：失去运行期异构容器（`vector<Strategy*>`），需要 `std::variant` + 访问器（`std::visit`），或用模板化 worker 逐一实例化。

### 4.3 信号槽复用原子槽（跨线程）

若策略在各自线程写、仲裁线程读，**沿用 Trader 的 `SignalSlot` 原子槽**（side/locate/strength/seq 各一原子），仲裁聚合 N 个槽（N 次原子读，仍是单 cache 行级）。比遍历 `signal()` 虚调用 + 读散布状态更 cache 友好。

### 4.4 避免逐事件分配

本重构已避免（`vector` 预分配、`strategy_refs_` 无 per-event 分配）。生产移植保持这一点，勿引入 `std::function`/`std::bind` 闭包分配进热路径。

### 4.5 依赖头文件

`on_market` 依赖 `OrderBook`（前向声明 + 具体策略头内引入），生产移植需注意 include 面与编译时，避免热路径文件膨胀。

---

## 5. 结论

- **仿真场景（本项目）**：重构可用，性能损失实测 <2%，可插拔收益（加策略不改框架、配置化）成立。
- **生产场景（Trader）**：重构的**通用化模式**（虚函数 + STL 容器遍历 + 按需查簿）不应直接进热路径；应把可插拔放在**启动期绑定**，热路径保留编译期绑定/原子槽。若 Trader 采纳"统一接口"方向，推荐 CRTP/`std::variant` 编译期多态 + 复用原子信号槽。
- **额外注意**：本重构把"方向推导/BBO 取用"挪进策略内部（每策略一次），是 N 策略 = N 查簿的来源；生产若策略数多，可考虑框架算一次缓存给各策略共享（原 `BookWorker` 即如此）。

---

*文档来源：NebulaX-Simulator（单机纯内存仿真）。配套代码：[strategy/base/strategy.h](src/strategy/base/strategy.h)、[sim/sim_worker.h](src/sim/sim_worker.h)、[sim_main.cpp](src/sim_main.cpp)。*
