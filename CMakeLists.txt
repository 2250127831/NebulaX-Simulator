cmake_minimum_required(VERSION 3.20)
project(NebulaX-Simulator VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── 外部依赖：仅 yaml-cpp + Threads（不依赖 io_uring/libbpf/dpdk，纯内存仿真）──
find_package(yaml-cpp REQUIRED)
find_package(Threads REQUIRED)

add_compile_options(-Wall -Wextra -Wpedantic -O2 -march=native)

# ── 交易所侧（迁移自 NebulaX 撮合核心）──
# 纯撮合引擎，不依赖交易侧。ExchangeSimulator（桥，引用策略市场簿）放 sim 库。
add_library(exchange STATIC
    src/exchange/matching_engine.cpp
    src/exchange/order_book.cpp
    src/exchange/itch_parser.cpp
    src/exchange/wal.cpp
)
target_include_directories(exchange PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
                                           ${CMAKE_CURRENT_SOURCE_DIR}/src/exchange)

# ── 交易侧 + 交易所适配器 + 回放 + 分析（迁移自 NebulaX-Trader 框架 + 本仿真新增）──
add_library(sim STATIC
    src/core/config_loader.cpp
    src/market/book/order_book_consumer.cpp
    src/exchange/exchange_simulator.cpp
    src/replay/itch_replay_source.cpp
    src/replay/replay_engine.cpp
    src/analysis/result_recorder.cpp
)
target_include_directories(sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
                                      ${CMAKE_CURRENT_SOURCE_DIR}/src/exchange)
target_link_libraries(sim PUBLIC exchange yaml-cpp Threads::Threads)

# ── 主程序 ──
add_executable(nebulaX_simulator src/sim_main.cpp)
target_link_libraries(nebulaX_simulator PRIVATE sim exchange yaml-cpp Threads::Threads)

# ── 测试 ──
add_executable(test_replay_pure tests/test_replay_pure.cpp)
target_link_libraries(test_replay_pure PRIVATE sim exchange Threads::Threads)

enable_testing()
# 测试样本：优先用生成的小样本（CI 无本地大样本，gen_test_data.py 生成）；
# 本地有 itch_sample.bin（软链到 Trader test_data）时用大样本做量级对比。
set(SYNTH_SAMPLE ${CMAKE_CURRENT_SOURCE_DIR}/test_data/itch_synth_sample.bin)
set(FULL_SAMPLE ${CMAKE_CURRENT_SOURCE_DIR}/test_data/itch_full_sample.bin)

# 重演等价强断言：构造的完整样本（订单流+成交流一一对应）必须 100% 等价 + 0 错误
add_test(NAME replay_full_exact COMMAND test_replay_pure ${FULL_SAMPLE} --exact)

# 纯回放结构正确性：多 symbol 合成样本（100% 重演等价）——CI 可用
add_test(NAME replay_pure_synth COMMAND test_replay_pure ${SYNTH_SAMPLE})

# 大样本量级对比（本地有 itch_sample.bin 才启用；CI 上不存在则跳过）
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_data/itch_sample.bin)
  add_test(NAME replay_pure COMMAND test_replay_pure
           ${CMAKE_CURRENT_SOURCE_DIR}/test_data/itch_sample.bin)
endif()

# 价格单位 + PnL 数值正确性（全链路统一 1/10000 tick）
add_executable(test_price_units tests/test_price_units.cpp)
target_link_libraries(test_price_units PRIVATE sim exchange Threads::Threads)
add_test(NAME price_units COMMAND test_price_units)

# 加权净投票仲裁（吸收自 Trader arbitrate.h）
add_executable(test_arbitrate tests/test_arbitrate.cpp)
target_link_libraries(test_arbitrate PRIVATE sim exchange Threads::Threads)
add_test(NAME arbitrate COMMAND test_arbitrate)
