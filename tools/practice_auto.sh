#!/usr/bin/env bash
# practice_auto.sh —— 无人值守跑一场练习赛（全自动：部署→起 bot→注册→开始→等完→存证→统计）
#
# 用法: ./tools/practice_auto.sh <标签> [max_ticks]
#   例: ./tools/practice_auto.sh v85 500
#
# 前置: 练习服务器已在 127.0.0.1:8080 运行（start_referee.sh --stage 2 --bots B,C,D）
# 产出: docs/evidence/practice_<标签>_final_state.json
#       docs/evidence/practice_<标签>_bot_<时间戳>.log
# 退出码: 0 完成；2 服务器未就绪；3 注册超时；4 对局超时
set -o pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
LABEL="${1:?用法: practice_auto.sh <标签> [max_ticks]}"
TICKS="${2:-500}"
CONFIG="${3:-}"
API=http://127.0.0.1:8080
EVI="$ROOT/docs/evidence"
STAMP="$(date +%Y%m%d-%H%M%S)"
LOG="$EVI/practice_${LABEL}_bot_${STAMP}.log"
OUT="$EVI/practice_${LABEL}_final_state.json"
mkdir -p "$EVI"

state() { curl -s -m 3 "$API/api/state"; }
field() { python3 -c "import json,sys;d=json.load(sys.stdin);print($1)" 2>/dev/null; }

if [ -z "$(state | field "d['status']['ready']")" ]; then
  echo "ERROR: 练习服务器未就绪（127.0.0.1:8080）" >&2; exit 2
fi

# 1) 部署（新 session，暂停在 tick 0）；给了 CONFIG 就用自定义地图/资源布局
if [ -n "$CONFIG" ]; then
  BODY="$(python3 - "$CONFIG" "$TICKS" <<'PY'
import json, sys
cfg = json.load(open(sys.argv[1]))
print(json.dumps({'stage': 2, 'tick_ms': 500, 'max_ticks': int(sys.argv[2]),
                  'bots': ['B', 'C', 'D'], 'config': cfg}, ensure_ascii=False))
PY
)"
else
  BODY="{\"stage\":2,\"tick_ms\":500,\"max_ticks\":${TICKS},\"bots\":[\"B\",\"C\",\"D\"]}"
fi
curl -s -m 30 -X POST "$API/api/start" -d "$BODY" \
  | field "d.get('session_id','?')" | sed 's/^/deploy: /'
SID="$(state | field "d['session_id']")"
echo "session: $SID"

# 2) 起 bot（每场新进程，确保注册被新裁判收到）
pkill -f 'lib/rmtc_bot/rmtc_bot' 2>/dev/null
sleep 1
(
  cd "$ROOT/contestant_ws/src/rmtc_bot" || exit 1
  set +u
  # shellcheck disable=SC1091
  source /opt/ros/humble/setup.bash
  # shellcheck disable=SC1091
  source ../../install/setup.bash
  export ROS_DOMAIN_ID=27
  export RMTC_DECISION_CONFIG="$PWD/config/decision_config.json"
  export RMTC_LLM_URL=http://127.0.0.1:8080/v1/chat/completions
  export RMTC_TEAM_TOKEN=practice-A
  exec ros2 run rmtc_bot rmtc_bot A BLACKOUT队
) > "$LOG" 2>&1 &
BOT=$!

# 3) 等注册（以裁判日志为准：board 的队名要等开赛后才刷新）
REFLOG="$HOME/.rmtc-practice/$SID/referee.log"
ok=0
for _ in $(seq 1 40); do
  if grep -q 'registered team A as' "$REFLOG" 2>/dev/null; then ok=1; break; fi
  sleep 0.5
done
if [ "$ok" != "1" ]; then
  echo "ERROR: 注册超时（裁判日志无 team A）" >&2; kill "$BOT" 2>/dev/null; exit 3
fi
echo "registered: $(grep 'registered team A as' "$REFLOG" | tail -1)"

# 4) 开始
curl -s -m 5 -X POST "$API/api/resume" -d '{}'; echo

# 5) 等结束
for _ in $(seq 1 900); do
  st="$(state)"
  fin="$(printf '%s' "$st" | field "d['status']['finished']")"
  if [ "$fin" = "True" ]; then
    printf '%s' "$st" > "$OUT"
    echo "finished: tick=$(printf '%s' "$st" | field "d['status']['tick']")"
    break
  fi
  sleep 1
done
if [ "$fin" != "True" ]; then
  echo "ERROR: 对局超时（未收到 finished）" >&2; kill "$BOT" 2>/dev/null; exit 4
fi

# 6) 统计
sleep 1
kill "$BOT" 2>/dev/null
pkill -f 'lib/rmtc_bot/rmtc_bot' 2>/dev/null
python3 "$ROOT/tools/match_stats.py" "$HOME/.rmtc-practice/$SID" "$LOG" 2>&1 | head -60
echo "state: $OUT"
echo "log:   $LOG"
