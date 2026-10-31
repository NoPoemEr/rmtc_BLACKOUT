#!/usr/bin/env bash
# map_test.sh —— 对抗地图实测编排：每张地图 3 场陪练（A vs B/C/D）+ 3 场自对弈
#
# 用法: ./tools/map_test.sh <地图配置json> <标签> [端口]
#   例:  ./tools/map_test.sh maps/map_walls.json walls
#   前置: 裁判未运行时自动拉起（默认 stage2；会话配置经 /api/start body 的
#         config 字段按地图下发——CLI --config 只作用于进程级默认值）
#   产物: docs/evidence/<标签>_batch_*.json / *_bot_*.log
#         docs/evidence/<标签>_selfplay_*.json / *_bot_*.log
#   注意: 脚本结束后不停止裁判（可复用）；换图前手动停裁判。
#         pkill -f 会自匹配本脚本命令行，停裁判请用
#         pgrep -f 'practice.py --' | xargs kill 这类带引号分隔的写法。
set -eo pipefail
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$HERE/.."
CFG="$(cd -- "$(dirname -- "$1")" && pwd)/$(basename "$1")"
TAG="${2:?用法: map_test.sh <config.json> <标签>}"
PORT="${3:-8080}"
BASE="http://127.0.0.1:$PORT"
EVI="$ROOT/docs/evidence"
mkdir -p "$EVI"
REF_DIR="$HOME/Desktop/dist/rmtc_practice"

stop_referee() {
  # 只杀 practice.py 主进程（children 是独立 ROS 节点，一并按模式杀）
  for pid in $(pgrep -f "practice.py --" 2>/dev/null); do kill "$pid" 2>/dev/null || true; done
  for pid in $(pgrep -f "$HOME/Desktop/dist/rmtc_practice/bin/" 2>/dev/null); do kill "$pid" 2>/dev/null || true; done
  sleep 3
}

start_referee() {  # $1: --bots 参数（可空）
  setsid bash -c "cd $REF_DIR && ./start_referee.sh --stage 2 $1 --config $CFG" \
    >"$EVI/referee_${TAG}_$(date +%H%M%S).log" 2>&1 &
  for i in $(seq 1 90); do
    if curl -sf -m 2 "$BASE/api/state" >/dev/null 2>&1; then return 0; fi
    sleep 1
  done
  echo "    [中止] 裁判 90 秒未上线（$CFG）" >&2
  return 1
}

echo "===== 地图实测: $TAG ($CFG) ====="
stop_referee
export RMTC_REF_CONFIG="$CFG"  # 会话配置经 /api/start body 下发（CLI --config 不会进会话）
echo "-- 陪练场（3 场）--"
start_referee "--bots B,C,D"
bash "$HERE/match_batch.sh" 3 100 A BLACKOUT队 "$PORT"
echo "-- 自对弈（3 场）--"
bash "$HERE/selfplay.sh" 3 100 "$PORT"
stop_referee
echo "===== $TAG 完成 ====="
