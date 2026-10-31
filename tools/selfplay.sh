#!/usr/bin/env bash
# selfplay.sh —— 自对弈：裁判 0 陪练，我方同构建镜像占满 A–D 四席位互打（10/28 晚新增）
#
# 动机（用户 10/28 晚拍板"换一种测试方式"）：裁判陪练跑的是朴素参考策略，
#   只适合协议回归与绝对分基线，测不出对抗强度。自对弈让对手拥有相同感知与
#   规则知识，把资源争夺/集火/撤退时机等对抗维度打出来。
# 局限：同代码 = 盲区同源，故只看相对表现与死亡线，不当作外部对手的替身。
#
# 用法: ./tools/selfplay.sh [场数] [tick_ms] [端口]
#   默认: 5 场、100ms×500 Tick、端口 8080
#   LLM 网关: SELFPLAY_LLM_GATEWAY=http://127.0.0.1:8899/v1/chat/completions \
#               ./tools/selfplay.sh ...（配 tools/mock_llm_gateway.py 联调回血路线；
#             未设置时用裁判 practice-mock，event_plan 恒 null）
#   席位: A=BLACKOUT队（主视角），B/C/D=BLACKOUT-B/C/D（镜像）
#   前置: 裁判未启动时自动拉起 ~/Desktop/dist/rmtc_practice/start_referee.sh --stage 2
#         （不带 --bots = 0 陪练；stage2 模板本身含 A–D 四席位）
#   产物: docs/evidence/selfplay_<场次>_<时间>.json（裁判状态快照）
#         docs/evidence/selfplay_<场次>_bot_<席位>_<时间>.log（各席位程序日志）
#         docs/evidence/selfplay_summary_<时间>.txt（汇总表）
#   收尾: 脚本不停止裁判；全部结束后照 runbook §5 手动清理。
# 注意：不能加 -u —— source /opt/ros/humble/setup.bash 会引用未定义变量
set -eo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$HERE/.."
COUNT="${1:-5}"
TICK_MS="${2:-500}"
PORT="${3:-8080}"
BASE="http://127.0.0.1:$PORT"
STAMP="$(date +%Y%m%d-%H%M%S)"
EVI="$ROOT/docs/evidence"
SEATS=(A B C D)
NAME_A="BLACKOUT队"
# 可选 LLM 网关（R2-B 回血路线联调）：SELFPLAY_LLM_GATEWAY=... ./tools/selfplay.sh
#   未设置时沿用 run_contestant.sh 默认（裁判 practice-mock，event_plan 恒 null）。
LLM_GW="${SELFPLAY_LLM_GATEWAY:-}"
GW_ARGS=()
if [ -n "$LLM_GW" ]; then GW_ARGS=("$LLM_GW"); fi

mkdir -p "$EVI"

# ---------- 0. 裁判在线检查/拉起（0 陪练） ----------
echo "==> [0/3] 裁判在线检查"
if ! curl -s -o /dev/null -m 2 "$BASE/api/state" 2>/dev/null; then
  echo "    裁判未运行 → 自动拉起 stage2 裁判（0 陪练，日志 $EVI/referee_selfplay_${STAMP}.log）"
  setsid bash -c "cd $HOME/Desktop/dist/rmtc_practice && ./start_referee.sh --stage 2" \
    >"$EVI/referee_selfplay_${STAMP}.log" 2>&1 &
  for i in $(seq 1 90); do
    sleep 1
    curl -s -o /dev/null -m 2 "$BASE/api/state" 2>/dev/null && break
  done
  if ! curl -s -o /dev/null -m 2 "$BASE/api/state" 2>/dev/null; then
    echo "    [中止] 裁判 90 秒内未上线，见 $EVI/referee_selfplay_${STAMP}.log" >&2
    exit 1
  fi
fi
echo "    裁判在线"

# ---------- 1. 连跑 COUNT 场 ----------
SUMMARY="$EVI/selfplay_summary_${STAMP}.txt"
: > "$SUMMARY"
echo "==> [1/3] 连跑 $COUNT 场（${TICK_MS}ms×500，我方 A–D 镜像互打，0 陪练）"
if [ -n "$LLM_GW" ]; then echo "    LLM 网关: $LLM_GW（mock 事件理解）"; fi
for n in $(seq 1 "$COUNT"); do
  echo "    -- 场 $n/$COUNT 部署并 resume --"
  if [ -n "${RMTC_REF_CONFIG:-}" ] && [ -f "$RMTC_REF_CONFIG" ]; then
    BODY=$(python3 -c "import json,sys; d={'stage':2,'tick_ms':${TICK_MS},'max_ticks':500,'bots':[]}; d['config']=json.load(open(sys.argv[1])); print(json.dumps(d))" "$RMTC_REF_CONFIG")
  else
    BODY="{\"stage\":2,\"tick_ms\":${TICK_MS},\"max_ticks\":500,\"bots\":[]}"
  fi
  curl -sf -X POST "$BASE/api/start" -H 'Content-Type: application/json' \
    -d "$BODY" >/dev/null
  sleep 1
  curl -sf -X POST "$BASE/api/resume" -d '{}' >/dev/null

  PIDS=()
  trap 'for p in "${PIDS[@]:-}"; do kill -- -"$p" 2>/dev/null || true; done' EXIT INT TERM
  for s in "${SEATS[@]}"; do
    if [ "$s" = "A" ]; then NAME="$NAME_A"; else NAME="BLACKOUT-$s"; fi
    LOG="$EVI/selfplay_${n}_bot_${s}_${STAMP}.log"
    setsid "$ROOT/run_contestant.sh" "$s" "$NAME" "${GW_ARGS[@]}" >"$LOG" 2>&1 &
    PIDS+=($!)
  done

  FINISHED=false
  for i in $(seq 1 150); do
    sleep 2
    if curl -sf "$BASE/api/state" 2>/dev/null | \
        python3 -c 'import json,sys; sys.exit(0 if json.load(sys.stdin)["status"]["finished"] else 1)' 2>/dev/null; then
      FINISHED=true; break
    fi
  done
  if [ "$FINISHED" != true ]; then
    echo "    [中止] 场 $n 5 分钟内未结束；日志见 $EVI/selfplay_${n}_bot_*_${STAMP}.log" >&2
    for p in "${PIDS[@]}"; do kill -- -"$p" 2>/dev/null || true; done
    exit 1
  fi
  sleep 1
  for p in "${PIDS[@]}"; do kill -- -"$p" 2>/dev/null || true; done
  for p in "${PIDS[@]}"; do wait "$p" 2>/dev/null || true; done
  PIDS=()

  SNAP="$EVI/selfplay_${n}_${STAMP}.json"
  curl -sf "$BASE/api/state" > "$SNAP"
  python3 - "$SNAP" "$n" >> "$SUMMARY" <<'PYEOF'
import json, sys
snap, n = sys.argv[1], sys.argv[2]
d = json.load(open(snap))
teams = d["board"]["teams"]
rank = sorted(teams, key=lambda t: -t.get("score", -1))
def line(t, rk):
    return ("%-14s 总分=%.2f 排名=%d/%d 资源=%s 伤害=%.1f 任务=%s 死亡=%s invalid=%s hp=%s" % (
        t.get("team_name", t["team"]), t.get("score", -1), rk, len(teams),
        t.get("resource_value", "?"), t.get("effective_damage", -1),
        t.get("mission_completed", "?"), t.get("deaths", "?"),
        t.get("invalid_actions", "?"), t.get("hp", "?")))
print("场%s（A 为主视角）" % n)
for t in rank:
    rk = next(i + 1 for i, x in enumerate(rank) if x["team"] == t["team"])
    mark = "  <== A" if t["team"] == "A" else ""
    print("    " + line(t, rk) + mark)
PYEOF
  echo "    场 $n 完成 → $SNAP"
done

# ---------- 2. 汇总 ----------
echo "==> [2/3] 汇总"
cat "$SUMMARY"
echo "==> [3/3] 产物"
echo "    汇总: $SUMMARY"
echo "    快照: $EVI/selfplay_*_${STAMP}.json"
echo "    日志: $EVI/selfplay_*_bot_*_${STAMP}.log"
