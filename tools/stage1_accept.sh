#!/usr/bin/env bash
# stage1 一键验收：部署新场 → 拉起本程序 → 等整场结束 → 快照 + 自动判定
#
# 用法: ./tools/stage1_accept.sh [席位A] [队名] [端口8080]
#   前置（照 docs/练习联调runbook.md）：
#     1. 裁判已启动（runbook §1，HTTP 127.0.0.1:8080）
#     2. decision_config.json 的 decision_type=collect（未实现 CollectStrategy 时跑会判不达标，属预期）
#   通过标准（runbook §3）：status.finished、invalid_actions=0、收集分 >= 35（冲 38+）、
#     回执闭环（last_action.tick=99）。产物：docs/evidence/stage1_accept_<时间>.json + 程序日志。
#   调试口子：RMTC_BOT_CMD 覆盖启动命令（如用参考车验脚本判定逻辑），默认跑本仓库程序。
# 注意：不能加 -u —— source /opt/ros/humble/setup.bash 会引用未定义变量
set -eo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$HERE/.."
SEAT="${1:-A}"   # 练习场参赛席位固定为 A（stage1 只有 A 车位），此处默认值安全
NAME="${2:-BLACKOUT队}"
PORT="${3:-8080}"

if [ "$SEAT" != "A" ]; then
  echo "    [中止] 练习场参赛席位固定为 A（stage1 无陪练、只有 A 车位），当前='$SEAT'" >&2
  exit 2
fi
BASE="http://127.0.0.1:$PORT"
STAMP="$(date +%Y%m%d-%H%M%S)"
EVI="$ROOT/docs/evidence"
MIN_SCORE=35      # 通过线（runbook §3）
TARGET_SCORE=38   # 冲刺线
MAX_TICKS=100     # stage1 默认 100 Tick

# ---------- 0. 前置检查 ----------
echo "==> [0/5] 前置检查"
if ! curl -s -o /dev/null -m 2 "$BASE/api/state" 2>/dev/null; then
  echo "    [中止] 裁判未响应 $BASE —— 先按 runbook §1 启动裁判: ~/Desktop/dist/rmtc_practice/start_referee.sh --stage 1" >&2
  exit 1
fi

CFG="${RMTC_DECISION_CONFIG:-$ROOT/contestant_ws/src/rmtc_bot/config/decision_config.json}"
DTYPE="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("decision_type",""))' "$CFG" 2>/dev/null || true)"
if [ "$DTYPE" != "collect" ]; then
  echo "    [中止] decision_config.json 的 decision_type=$DTYPE（应为 collect）—— 先改 $CFG" >&2
  exit 1
fi
echo "    裁判在线、decision_type=collect 就绪"

# ---------- 1. 部署新场并推进 ----------
echo "==> [1/5] 部署 stage1 新场（100ms×100）并 resume"
curl -sf -X POST "$BASE/api/start" -H 'Content-Type: application/json' \
  -d '{"stage":1,"tick_ms":100,"max_ticks":100,"bots":[]}' >/dev/null
sleep 1
curl -sf -X POST "$BASE/api/resume" -d '{}' >/dev/null

# ---------- 2. 拉起程序 ----------
echo "==> [2/5] 启动程序（席位 $SEAT，队名 $NAME）"
LOG="$EVI/stage1_accept_bot_${STAMP}.log"
BOT_PID=""
# M10 修复：EXIT/INT/TERM 统一回收进程组，中途 Ctrl+C 也不留孤儿 bot 替本队代打
trap 'if [ -n "$BOT_PID" ]; then kill -- -"$BOT_PID" 2>/dev/null || true; fi' EXIT INT TERM
if [ -n "${RMTC_BOT_CMD:-}" ]; then
  # 调试口子（仅本地调试用，内容按 shell 解析）
  echo "    命令(调试口子): $RMTC_BOT_CMD"
  setsid bash -c "$RMTC_BOT_CMD" >"$LOG" 2>&1 &
else
  # L24 修复：默认路径不再拼 shell 字符串——路径含空格不失效、队名不可注入
  echo "    命令: $ROOT/run_contestant.sh $SEAT \"$NAME\""
  setsid "$ROOT/run_contestant.sh" "$SEAT" "$NAME" >"$LOG" 2>&1 &
fi
BOT_PID=$!

# ---------- 3. 等整场结束 ----------
echo "==> [3/5] 等待整场结束（上限 90s）"
FINISHED=false
for i in $(seq 1 45); do
  sleep 2
  if curl -sf "$BASE/api/state" | python3 -c 'import json,sys; sys.exit(0 if json.load(sys.stdin)["status"]["finished"] else 1)' 2>/dev/null; then
    FINISHED=true; break
  fi
done
if [ "$FINISHED" != true ]; then
  echo "    [中止] 90s 内未结束（referee 可能卡住）；程序日志: $LOG" >&2
  kill -- -"$BOT_PID" 2>/dev/null || true
  exit 1
fi
sleep 1

# ---------- 4. 停程序 + 快照 ----------
echo "==> [4/5] 停程序并快照"
kill -- -"$BOT_PID" 2>/dev/null || true
wait "$BOT_PID" 2>/dev/null || true
# 兜底：setsid 组杀漏网时告警（防上一场孤儿替本队代打，10/21 实测坑）
# SIGTERM 优雅关闭有 ~1s 时延，两轮复检只报真残留
LEFTOVER=""
for _ in 1 2; do
  sleep 2
  LEFTOVER="$(pgrep -f 'rmtc_bot.*rmtc_bot' | tr '\n' ' ' || true)"
  [ -z "$LEFTOVER" ] && break
done
if [ -n "$LEFTOVER" ]; then
  echo "    [警告] 仍有 rmtc_bot 进程: $LEFTOVER —— 检查是否上一场孤儿（pgrep -af rmtc_bot）"
fi
SNAP="$EVI/stage1_accept_${STAMP}.json"
curl -sf "$BASE/api/state" > "$SNAP"
echo "    快照: $SNAP"

# ---------- 5. 判定 ----------
echo "==> [5/5] 判定"
VERDICT="$(python3 - "$SNAP" "$SEAT" "$MIN_SCORE" "$MAX_TICKS" <<'PYEOF'
import json, sys
snap, seat, min_score, max_ticks = sys.argv[1], sys.argv[2].upper(), float(sys.argv[3]), int(sys.argv[4])
d = json.load(open(snap))
team = next((t for t in d["board"]["teams"] if t["team"] == seat), None)
la = d.get("checks", {}).get(seat, {}).get("last_action", {})
pt = d.get("checks", {}).get(seat, {}).get("perception_tick", 0)
ok_finished = bool(d["status"]["finished"])
score = team["score"] if team else -1.0
invalid = team["invalid_actions"] if team else -1
rows = [
    ("整场结束 finished", ok_finished),
    ("invalid_actions=0", invalid == 0),
    (f"收集分 >= {int(min_score)}（当前 {score:.1f}，冲刺 38+）", score >= min_score),
    (f"回执闭环 last_action.tick={max_ticks-1}（当前 {la.get('tick')}）", la.get("tick") == max_ticks-1),
]
print(f"    席位 {seat} | tick={d['status']['tick']}/{d['status']['max_ticks']} "
      f"| 收集分={score} | invalid_actions={invalid} | perception_tick={pt}")
print(f"    裁判侧 last_action: {json.dumps(la, ensure_ascii=False)}")
for label, ok in rows:
    print(f"    [{'绿' if ok else '红'}] {label}")
print("RESULT=" + ("PASS" if all(ok for _, ok in rows) else "FAIL"))
PYEOF
)"
echo "$VERDICT"

echo
echo "    程序日志: $LOG"
echo "    回执闭环核对（并排截图素材 3）"
echo "    -- 程序侧日志尾部 --"
tail -5 "$LOG" | sed 's/^/      /'
echo
if echo "$VERDICT" | grep -q 'RESULT=PASS'; then
  echo ">>> stage1 验收通过（素材 1–4 截图后照 runbook §5 收尾清理）"
  # M9 修复：FAIL 必须让脚本退出码非 0，自动化验收不再误判
  exit 0
else
  echo ">>> stage1 验收未达标：红项见上。实现 CollectStrategy 后再跑（docs/collect_design.md）。"
  exit 1
fi
