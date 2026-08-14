#!/usr/bin/env bash
# ── 性能压测 ──
# 用大样本（itch_sample.bin / itch_100mb.bin，本地软链）跑完整仿真，测耗时与吞吐。
# 用法：./scripts/run_benchmark.sh [itch_file] [config.yaml]
set -euo pipefail

cd "$(dirname "$0")/.."
FILE="${1:-test_data/itch_sample.bin}"
CONFIG="${2:-config/simulator.yaml}"
OUT="/tmp/nebulaX_bench"

[ -f "$FILE" ] || { echo "样本不存在: $FILE (软链到 NebulaX-Trader/test_data?)"; exit 1; }

# 指定回放文件 + 独立输出目录
BENCH_CFG=/tmp/nebulaX_bench.yaml
sed "s|file:.*itch.*|file: $FILE|" "$CONFIG" | sed "s|out_dir:.*|out_dir: $OUT|" > "$BENCH_CFG"

echo "=== NebulaX-Simulator Benchmark ==="
echo "file: $FILE"
START=$(date +%s.%N)
./build/nebulaX_simulator "$BENCH_CFG" > /tmp/nebulaX_bench.out 2>&1
END=$(date +%s.%N)

ELAPSED=$(echo "$END - $START" | bc 2>/dev/null || echo "?")
grep -E "messages|order_events|segments|engine trades|strategy orders" /tmp/nebulaX_bench.out
echo "---"
echo "elapsed: ${ELAPSED}s"
# 吞吐 = 委托数 / 秒（从 stdout 解析）
MSGS=$(grep -oP 'messages\s+: \K[0-9]+' /tmp/nebulaX_bench.out)
if [ -n "$MSGS" ] && [ "$ELAPSED" != "?" ]; then
  TPS=$(echo "$MSGS $ELAPSED" | awk '{printf "%.0f", $1/$2}')
  echo "throughput: ~${TPS} msgs/s"
fi
echo "output: $OUT/{equity,trades,orders}.csv summary.txt"
