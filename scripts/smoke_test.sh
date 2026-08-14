#!/usr/bin/env bash
# ── 端到端冒烟测试 ──
# 跑一次仿真（合成样本，不依赖本地大软链），校验输出文件存在 + 仿真正常结束。
# 用法：./scripts/smoke_test.sh [config.yaml] [out_dir]
set -euo pipefail

cd "$(dirname "$0")/.."
CONFIG="${1:-config/simulator.yaml}"
OUT="${2:-/tmp/nebulaX_smoke}"

# 生成测试数据（若小样本不存在）
[ -f test_data/itch_synth_sample.bin ] || python3 scripts/gen_test_data.py test_data

# 改用合成样本 + 指定输出目录（避免依赖本地大软链）
SMOKE_CFG=/tmp/nebulaX_smoke.yaml
sed "s|file:.*itch.*|file: test_data/itch_synth_sample.bin|" "$CONFIG" | \
  sed "s|out_dir:.*|out_dir: $OUT|" > "$SMOKE_CFG"

rm -rf "$OUT"
./build/nebulaX_simulator "$SMOKE_CFG" > /tmp/nebulaX_smoke.out 2>&1

# 断言输出文件存在且非空
for f in equity.csv trades.csv orders.csv summary.txt; do
  [ -s "$OUT/$f" ] || { echo "FAIL: $OUT/$f 缺失或为空"; cat /tmp/nebulaX_smoke.out; exit 1; }
done

# 断言仿真正常结束（有 segments 统计）
grep -q "segments.*:" "$OUT/summary.txt" || { echo "FAIL: summary.txt 缺 segments"; exit 1; }

echo "SMOKE TEST PASSED"
echo "  output: $OUT/{equity,trades,orders}.csv summary.txt"
grep -E "segments|strategy orders|realized pnl" "$OUT/summary.txt"
