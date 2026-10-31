#!/usr/bin/env bash
# stage2 一键验收：部署新场（带陪练 B/C/D）→ 拉起本程序 → 等整场结束 → 快照 + 自动判定
#
# 用法: ./tools/stage2_accept.sh [席位A] [队名] [端口8080]
#   前置（照 docs/练习联调runbook.md，stage2 版）：
#     1. 裁判已启动: ~/Desktop/dist/rmtc_practice/start_referee.sh --stage 2 --bots B,C,D
#     2. decision_config.json 的 decision_type=combat
#   通过标准（docs/combat_design.md §0 + M3 里程碑）：
#     finished、invalid_actions=0、任务分>0、资源分>0、AI 资格满足（ai.<seat>.success_count>0）、存活
#   产物：docs/evidence/stage2_accept_<时间>.json + 程序日志。
#   调试口子：RMTC_BOT_CMD 覆盖启动命令（如用参考车验脚本判定逻辑），默认跑本仓库程序。
#   时长：stage2 默认 500ms×500 Tick ≈ 4 分 10 秒，请耐心等待（脚本上限 8 分钟）。
# 注意：不能加 -u —— source /opt/ros/humble/setup.bash 会引用未定义变量
set -eo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$HERE/.."
SEAT="${1:-A}"   # 练习场参赛席位固定为 A（stage2 的 B/C/D 是陪练车位），此处默认值安全
NAME="${2:-BLACKOUT队}"
PORT="${3:-8080}"

if [ "$SEAT" != "A" ]; then
  echo "    [中止] 练习场参赛席位固定为 A（stage2 的 B/C/D 是陪练车位），当前='$SEAT'" >&2
  exit 2
fi
BASE="http://127.0.0.1:$PORT"
STAMP="$(date +%Y%m%d-%H%M%S)"
EVI="$ROOT/docs/evidence"
mkdir -p "$EVI"

# ---------- 0. 前置检查 ----------
echo "==> [0/5] 前置检查"
if ! curl -s -o /dev/null -m 2 "$BASE/api/state" 2>/dev/null; then
  echo "    [中止] 裁判未响应 $BASE —— 先启动 stage2 裁判: ~/Desktop/dist/rmtc_practice/start_referee.sh --stage 2 --bots B,C,D" >&2
  exit 1
fi

CFG="${RMTC_DECISION_CONFIG:-$ROOT/contestant_ws/src/rmtc_bot/config/decision_config.json}"
DTYPE="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("decision_type",""))' "$CFG" 2>/dev/null || true)"
if [ "$DTYPE" != "combat" ]; then
  echo "    [中止] decision_config.json 的 decision_type=$DTYPE（应为 combat）—— 先改 $CFG" >&2
  exit 1
fi
echo "    裁判在线、decision_type=combat 就绪"

# ---------- 1. 部署新场并推进 ----------
echo "==> [1/5] 部署 stage2 新场（500ms×500，陪练 B/C/D）并 resume"
curl -sf -X POST "$BASE/api/start" -H 'Content-Type: application/json' \
  -d '{"stage":2,"tick_ms":500,"max_ticks":500,"bots":["B","C","D"]}' >/dev/null
sleep 1
curl -sf -X POST "$BASE/api/resume" -d '{}' >/dev/null

# ---------- 2. 拉起程序 ----------
echo "==> [2/5] 启动程序（席位 $SEAT，队名 $NAME）"
LOG="$EVI/stage2_accept_bot_${STAMP}.log"
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

# ---------- 3. 等整场结束（250s 赛程 + 余量，上限 480s） ----------
echo "==> [3/5] 等待整场结束（上限 8 分钟，赛程约 4 分 10 秒）"
FINISHED=false
for i in $(seq 1 240); do
  sleep 2
  if curl -sf "$BASE/api/state" | python3 -c 'import json,sys; sys.exit(0 if json.load(sys.stdin)["status"]["finished"] else 1)' 2>/dev/null; then
    FINISHED=true; break
  fi
done
if [ "$FINISHED" != true ]; then
  echo "    [中止] 8 分钟内未结束（referee 可能卡住）；程序日志: $LOG" >&2
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
SNAP="$EVI/stage2_accept_${STAMP}.json"
curl -sf "$BASE/api/state" > "$SNAP"
echo "    快照: $SNAP"

# ---------- 5. 判定 ----------
echo "==> [5/5] 判定"
VERDICT="$(python3 - "$SNAP" "$SEAT" <<'PYEOF'
import json, sys
snap, seat = sys.argv[1], sys.argv[2].upper()
d = json.load(open(snap))
team = next((t for t in d["board"]["teams"] if t["team"] == seat), None)
la = d.get("checks", {}).get(seat, {}).get("last_action", {})
ai = d.get("ai", {}).get(seat, {})
score = team["score"] if team else -1.0
rv = team["resource_value"] if team else -1.0
mc = team["mission_completed"] if team else -1
invalid = team["invalid_actions"] if team else -1
alive = bool(team and team.get("alive"))
rows = [
    ("整场结束 finished", bool(d["status"]["finished"])),
    ("invalid_actions=0", invalid == 0),
    ("任务分>0（mission_completed=%s）" % mc, mc > 0),
    ("资源分>0（resource_value=%s）" % rv, rv > 0),
    ("AI 资格（ai.%s.success_count=%s）" % (seat, ai.get("success_count")), ai.get("success_count", 0) > 0),
    ("存活（hp=%s alive=%s）" % (team.get("hp") if team else "?", alive), alive),
]
print(f"    席位 {seat} | tick={d['status']['tick']}/{d['status']['max_ticks']} "
      f"| 总分={score} | 资源分={rv} | 任务完成={mc} | invalid={invalid}")
print(f"    ai 状态: {json.dumps({k: ai.get(k) for k in ('mode','success','status','success_count','attempt_count')}, ensure_ascii=False)}")
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
  echo ">>> stage2 验收通过（素材 1–4、6 截图后照 runbook §5 收尾清理）"
  # M9 修复：FAIL 必须让脚本退出码非 0，自动化验收不再误判
  exit 0
else
  echo ">>> stage2 验收未达标：红项见上。对照 docs/combat_design.md 排查后再跑。"
  exit 1
fi
