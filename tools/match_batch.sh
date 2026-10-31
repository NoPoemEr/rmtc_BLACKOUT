#!/usr/bin/env bash
# match_batch.sh —— 连跑 N 场 stage2 练习赛并汇总战绩（10/28 策略优化验证用）
#
# 用法: ./tools/match_batch.sh [场数] [tick_ms] [席位] [队名] [端口]
#   默认: 10 场、500ms×500 Tick（裁判 stage2 默认节奏，约 4 分钟/场）、席位 A、BLACKOUT队、端口 8080
#   前置: 裁判未启动时脚本自动拉起
#         ~/Desktop/dist/rmtc_practice/start_referee.sh --stage 2 --bots B,C,D
#   产物: docs/evidence/match_batch_<场次>_<时间>.json（裁判状态快照）
#         docs/evidence/match_batch_<场次>_bot_<时间>.log（程序日志）
#         docs/evidence/match_batch_summary_<时间>.txt（汇总表）
#   收尾: 脚本不停止裁判（可复用连跑）；全部结束后照 runbook §5 手动清理。
#   调试口子: RMTC_BOT_CMD 覆盖启动命令（同 stage2_accept.sh）。
# 注意：不能加 -u —— source /opt/ros/humble/setup.bash 会引用未定义变量
set -eo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$HERE/.."
COUNT="${1:-10}"
TICK_MS="${2:-500}"
SEAT="${3:-A}"
NAME="${4:-BLACKOUT队}"
PORT="${5:-8080}"
BASE="http://127.0.0.1:$PORT"
STAMP="$(date +%Y%m%d-%H%M%S)"
EVI="$ROOT/docs/evidence"
mkdir -p "$EVI"

if [ "$SEAT" != "A" ]; then
  echo "    [中止] 练习场参赛席位固定为 A（B/C/D 是陪练车位），当前='$SEAT'" >&2
  exit 2
fi

# ---------- 0. 裁判在线检查/拉起 ----------
echo "==> [0/3] 裁判在线检查"
if ! curl -s -o /dev/null -m 2 "$BASE/api/state" 2>/dev/null; then
  echo "    裁判未运行 → 自动拉起 stage2 裁判（--bots B,C,D，日志 $EVI/referee_batch_${STAMP}.log）"
  setsid bash -c "cd $HOME/Desktop/dist/rmtc_practice && ./start_referee.sh --stage 2 --bots B,C,D" \
    >"$EVI/referee_batch_${STAMP}.log" 2>&1 &
  for i in $(seq 1 90); do
    sleep 1
    curl -s -o /dev/null -m 2 "$BASE/api/state" 2>/dev/null && break
  done
  if ! curl -s -o /dev/null -m 2 "$BASE/api/state" 2>/dev/null; then
    echo "    [中止] 裁判 90 秒内未上线，见 $EVI/referee_batch_${STAMP}.log" >&2
    exit 1
  fi
fi
echo "    裁判在线"

# ---------- 1. 连跑 COUNT 场 ----------
SUMMARY="$EVI/match_batch_summary_${STAMP}.txt"
: > "$SUMMARY"
echo "==> [1/3] 连跑 $COUNT 场（${TICK_MS}ms×500，陪练 B/C/D）"
for n in $(seq 1 "$COUNT"); do
  echo "    -- 场 $n/$COUNT 部署并 resume --"
  if [ -n "${RMTC_REF_CONFIG:-}" ] && [ -f "$RMTC_REF_CONFIG" ]; then
    BODY=$(python3 -c "import json,sys; d={'stage':2,'tick_ms':${TICK_MS},'max_ticks':500,'bots':['B','C','D']}; d['config']=json.load(open(sys.argv[1])); print(json.dumps(d))" "$RMTC_REF_CONFIG")
  else
    BODY="{\"stage\":2,\"tick_ms\":${TICK_MS},\"max_ticks\":500,\"bots\":[\"B\",\"C\",\"D\"]}"
  fi
  curl -sf -X POST "$BASE/api/start" -H 'Content-Type: application/json' \
    -d "$BODY" >/dev/null
  sleep 1
  curl -sf -X POST "$BASE/api/resume" -d '{}' >/dev/null

  LOG="$EVI/match_batch_${n}_bot_${STAMP}.log"
  BOT_PID=""
  # M10 同款：EXIT/INT/TERM 统一回收进程组，防孤儿 bot 替本队代打
  trap 'if [ -n "$BOT_PID" ]; then kill -- -"$BOT_PID" 2>/dev/null || true; fi' EXIT INT TERM
  if [ -n "${RMTC_BOT_CMD:-}" ]; then
    setsid bash -c "$RMTC_BOT_CMD" >"$LOG" 2>&1 &
  else
    setsid "$ROOT/run_contestant.sh" "$SEAT" "$NAME" >"$LOG" 2>&1 &
  fi
  BOT_PID=$!

  FINISHED=false
  for i in $(seq 1 150); do
    sleep 2
    if curl -sf "$BASE/api/state" 2>/dev/null | \
        python3 -c 'import json,sys; sys.exit(0 if json.load(sys.stdin)["status"]["finished"] else 1)' 2>/dev/null; then
      FINISHED=true; break
    fi
  done
  if [ "$FINISHED" != true ]; then
    echo "    [中止] 场 $n 5 分钟内未结束；程序日志 $LOG" >&2
    kill -- -"$BOT_PID" 2>/dev/null || true
    exit 1
  fi
  sleep 1
  kill -- -"$BOT_PID" 2>/dev/null || true
  wait "$BOT_PID" 2>/dev/null || true

  SNAP="$EVI/match_batch_${n}_${STAMP}.json"
  curl -sf "$BASE/api/state" > "$SNAP"
  python3 - "$SNAP" "$SEAT" "$n" >> "$SUMMARY" <<'PYEOF'
import json, sys
snap, seat, n = sys.argv[1], sys.argv[2].upper(), sys.argv[3]
d = json.load(open(snap))
teams = d["board"]["teams"]
rank = sorted(teams, key=lambda t: -t.get("score", -1))
ours = next(t for t in teams if t["team"] == seat)
rank_n = next(i + 1 for i, t in enumerate(rank) if t["team"] == seat)
print("场%s 总分=%.2f 排名=%d/%d 资源=%s 伤害=%.1f 任务=%s invalid=%s hp=%s alive=%s deaths=%s" % (
    n, ours.get("score", -1), rank_n, len(teams), ours.get("resource_value", "?"),
    ours.get("effective_damage", -1), ours.get("mission_completed", "?"),
    ours.get("invalid_actions", "?"), ours.get("hp", "?"), ours.get("alive", "?"),
    ours.get("deaths", "?")))
for t in rank:
    print("    %s: %.2f (资源%s 伤害%.1f 任务%s)" % (
        t["team"], t.get("score", -1), t.get("resource_value", "?"),
        t.get("effective_damage", -1), t.get("mission_completed", "?")))
PYEOF
  echo "    场 $n 完成 → $SNAP"
done

# ---------- 2. 汇总 ----------
echo "==> [2/3] 汇总"
cat "$SUMMARY"
echo "==> [3/3] 产物"
echo "    汇总: $SUMMARY"
echo "    快照: $EVI/match_batch_*_${STAMP}.json"
echo "    日志: $EVI/match_batch_*_bot_${STAMP}.log"
