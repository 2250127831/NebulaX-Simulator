# NebulaX 部分撤单 Bug 修复建议

> 本仓库（NebulaX-Simulator）迁移 NebulaX 撮合引擎时发现，供 NebulaX 维护者参考。
> 已在本仓库按建议修复并验证（构造的完整 ITCH 样本重演 100% 等价、0 错误）。

## 问题概述

**部分撤单（X / `processCancelShares`）会把订单剩余量扣两次**，导致：

- 订单剩余量 `remaining_qty` 被错误减 2× → 订单提前"消失"（后续 D/X 撤单报 `ORDER_NOT_FOUND`）。
- 撤单被错误计入 `filled_qty`（已成交量）→ 成交量统计虚高。
- 剩余量虚高/为 0 的订单滞留在盘口，后续撮合行为错乱 → 成交虚增。

实测（`itch_100mb.bin`，845 万委托）：
- 修复前：引擎成交 111,775 笔、`ORDER_NOT_FOUND` 312,194 次。
- 修复后：引擎成交 25,086 笔（≈ 历史成交）、`ORDER_NOT_FOUND` 19,929 次。

## 根因

`processCancelShares` 部分撤单分支**手动减了一次 remaining，又调用了 `reduceOrderQty`**，而 `reduceOrderQty` 内部**再减一次 remaining 且错误地增加 filled_qty**（`reduceOrderQty` 的语义是"撮合成交减量"，不是"撤单减量"）。

## 受影响位置（2 处）

### 位置 1：`src/matching_engine.cpp:280-281`（`processCancelShares` 部分撤分支）

```cpp
} else {
    // 部分撤：减少剩余量 + 档量
    o->remaining_qty -= shares;            // ← 减 1
    book->reduceOrderQty(o, shares);       // ← 内部再减 1，且 filled_qty += shares（错误）
    auto& rsp = out.emplace_back();
    rsp.type = RSP_OK;
    rsp.data.ack.order_id = order_ref;
}
```

### 位置 2：`src/matching_engine.cpp:438`（`recoverFromWal` WAL 回放的部分撤单分支）

```cpp
else { o->remaining_qty -= entry->quantity; book->reduceOrderQty(o, entry->quantity); }
```

同一双扣模式。WAL 回放含部分撤单时同样复现（崩溃恢复后状态错乱）。

## 建议修复

**新增一个"撤单减量"方法**（只减 remaining + 档量，不加 filled_qty），撮合成交仍走 `reduceOrderQty`：

`include/order_book.h`（在 `reduceOrderQty` 声明旁）:

```cpp
// 部分撤单：只减剩余量 + 档量，不增加已成交量（撤单不是成交）。
void reduceQtyCancel(Order* order, uint32_t amount);
```

`src/order_book.cpp`（在 `reduceOrderQty` 实现后）:

```cpp
void OrderBook::reduceQtyCancel(Order* order, uint32_t amount)
{
    order->remaining_qty -= amount;   // 只减剩余，不加 filled_qty

    PriceLevel& level = (order->side == Side::BUY)
        ? bids_[order->price] : asks_[order->price];
    level.total_qty -= amount;
}
```

`src/matching_engine.cpp:280`（`processCancelShares` 部分撤分支）改为:

```cpp
} else {
    // 部分撤：只减剩余量 + 档量（撤单不是成交，不加 filled_qty）
    book->reduceQtyCancel(o, shares);
    auto& rsp = out.emplace_back();
    rsp.type = RSP_OK;
    rsp.data.ack.order_id = order_ref;
}
```

`src/matching_engine.cpp:438`（`recoverFromWal` 部分撤分支）改为:

```cpp
else { book->reduceQtyCancel(o, entry->quantity); }
```

## 验证建议

修完后建议跑一个**部分撤单单测**覆盖：撤单后 `remaining_qty`、`filled_qty`、`level.total_qty` 三者一致，且后续整笔撤/删能正常找到订单。本仓库的复现用例：

```
买 40 @ P → X 撤 15（剩余应 25）→ 卖 10 @ P 吃 10（剩余应 15）→ D 删（应成功）
修复前 D 报 ORDER_NOT_FOUND（剩余被双扣成 0）；修复后 D 成功。
```

## 附：类似位置的核对

- `reduceOrderQty` 在撮合路径（`matching_engine.cpp:563/623`，maker 部分成交）的用法**正确**（成交应减 remaining + 加 filled），勿动。
- 全量撤单分支（`shares >= o->remaining_qty` → `book->removeOrder(o)`）无此问题。
- 其余 `reduceOrderQty` 调用点已核对无同类问题。
