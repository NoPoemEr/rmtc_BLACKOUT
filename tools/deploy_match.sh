#!/usr/bin/env bash
# deploy_match.sh —— 正式赛 3 分钟部署脚本（2026-10-30 官方公告）
#
# 公告要点（docs/规则变更追踪.md 2026-10-30 条目）：
#   - 每场之间准备时间严格 3 分钟，非官方问题导致无法按时部署视为 0 分
#   - 上场前向工作人员检查 git 状态
#   - 双赛场：各赛场令牌/分组按当天公共文档填写
#   - 轮间 10 分钟整备可改决策类型（本队拍板：全程单档 combat，轮间不改档）
#
# 用法: ./tools/deploy_match.sh [--go|--check] <席位A-D> <赛场1|2> <令牌> <域编号> <LLM网关> [队名]
#   --check（默认）：只校验 + 打印参数卡与 git 状态，不启动程序（出示给工作人员）
#   --go            ：校验全部通过后立即启动程序（3 分钟窗口最后一步）
#   例: ./tools/deploy_match.sh --check A 1 'T001-xxxxxxxx' 42 https://gw/v1/chat/completions
#   例: ./tools/deploy_match.sh --go    A 2 'T001-yyyyyyyy' 43 https://gw2/v1/chat/completions
#
# 设计约束：
#   - 全部接入参数只从命令行进入，不写任何文件 → 现场 git 零改动，检查秒过
#   - 档位校验：decision_config.json 必须恰好是 {"decision_type":"combat"}
#     单字段（规则 §2.3：config 只允许改 decision_type 且不得夹带其他内容）
#   - git 工作区不干净（除 decision_config.json 外有任何改动/未跟踪文件）→ 中止，
#     防止把未封存改动带上场（10/31 封存后工作区必须全绿）
set -eo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$HERE/.."
CFG="$ROOT/contestant_ws/src/rmtc_bot/config/decision_config.json"
EVI="$ROOT/docs/evidence"
STAMP="$(date +%Y%m%d-%H%M%S)"

MODE=check
while [ $# -gt 0 ]; do
  case "$1" in
    --check) MODE=check; shift ;;
    --go) MODE=go; shift ;;
    *) break ;;
  esac
done

SEAT="${1:-}"
VENUE="${2:-}"
TOKEN="${3:-}"
DOMAIN="${4:-}"
GATEWAY="${5:-}"
NAME="${6:-BLACKOUT队}"

fail=0
warn()  { echo "  [警告] $*" >&2; }
bad()   { echo "  [中止] $*" >&2; fail=1; }
need()  { [ -n "$2" ] || bad "$1"; }

echo "===== 正式赛部署校验（$STAMP，模式=$MODE）====="

# ---------- 1. 接入参数校验（当天公共文档填写） ----------
need "席位必填（A-D）" "$SEAT"
need "赛场编号必填（1 或 2）" "$VENUE"
need "本场令牌必填（当天公共文档）" "$TOKEN"
need "ROS 域编号必填（当天公共文档）" "$DOMAIN"
need "LLM 网关完整地址必填（当天公共文档）" "$GATEWAY"
[ -n "$SEAT" ] && [[ ! "$SEAT" =~ ^[A-D]$ ]] && bad "席位必须是 A-D 大写单字符，当前='$SEAT'"
[ -n "$VENUE" ] && [[ ! "$VENUE" =~ ^[12]$ ]] && bad "赛场编号必须是 1 或 2，当前='$VENUE'"
[ -n "$DOMAIN" ] && [[ ! "$DOMAIN" =~ ^[0-9]+$ ]] && bad "域编号必须是纯数字，当前='$DOMAIN'"
[ -n "$GATEWAY" ] && [[ ! "$GATEWAY" =~ ^https?:// ]] && bad "LLM 网关必须是完整 http(s) 地址，当前='$GATEWAY'"

# ---------- 2. 程序与档位校验 ----------
if [ ! -f "$ROOT/contestant_ws/install/setup.bash" ]; then
  bad "构建产物不存在——比赛日前必须完成一次 colcon build（手册 §1）"
fi
if [ ! -f "$CFG" ]; then
  bad "档位配置缺失：$CFG"
else
  # 恰好单字段 decision_type=combat（规则 §2.3 单字段封装；本队单档策略）
  python3 - "$CFG" <<'PYEOF'
import json, sys
p = sys.argv[1]
try:
    d = json.load(open(p))
except Exception as e:
    print(f"    [中止] 档位配置不是合法 JSON：{e}"); sys.exit(1)
if not isinstance(d, dict) or set(d.keys()) != {"decision_type"}:
    print(f"    [中止] 档位配置必须恰好只有 decision_type 单字段，实际键={sorted(d.keys())}"); sys.exit(1)
if d["decision_type"] != "combat":
    print(f"    [中止] decision_type 必须为 combat（本队单档策略），实际={d['decision_type']!r}"); sys.exit(1)
print("    decision_config.json 恰好单字段且为 combat（本队单档策略）✓")
PYEOF
  [ $? -ne 0 ] && fail=1
fi

# ---------- 3. git 状态（出示给工作人员） ----------
echo "  -- git 状态（上场前工作人员检查用）--"
DIRTY=0
if git -C "$ROOT" status --short | grep -v '^.. contestant_ws/src/rmtc_bot/config/decision_config.json$' | grep -q .; then
  DIRTY=1
fi
git -C "$ROOT" status --short || true
if [ "$DIRTY" = 1 ]; then
  bad "git 工作区不干净（除 decision_config.json 外有改动/未跟踪文件）——封存后工作区必须全绿；当前改动见上"
else
  echo "  git 工作区干净（或仅 decision_config.json 改动）✓"
fi

# ---------- 4. 参数卡（打印对照当天公共文档） ----------
echo "  -- 本场参数卡（对照当天公共文档逐项核对）--"
echo "    赛场   = $VENUE"
echo "    席位   = $SEAT"
echo "    队名   = $NAME"
echo "    令牌   = $TOKEN"
echo "    域编号 = $DOMAIN"
echo "    LLM网关= $GATEWAY"
echo "    决策档 = combat（单档策略，轮间不改档）"

if [ "$fail" = 1 ]; then
  echo "===== 校验未通过，禁止部署 =====" >&2
  exit 1
fi

if [ "$MODE" = check ]; then
  echo "===== 校验通过（--check 模式，未启动程序）====="
  echo "  3 分钟部署流程：本脚本 --check 给工作人员看 git 状态 → 确认参数卡"
  echo "  → 裁判宣布可以部署 → 本脚本 --go 启动 → 观察回执闭环与 LLM 计数"
  exit 0
fi

# ---------- 5. 启动（--go） ----------
# 单实例保证（10/30 练习赛教训：3 个未杀的孤儿 bot 进程同时打一场——
# 每 tick 3 条动作、LLM 调用 84 次 = 3×28，配额翻三倍且动作来源不明）。
# 注意 [r]mtc_bot 写法：防止 bash -c 命令行里出现同串时 pgrep 自匹配误杀。
OLD=$(pgrep -f 'rmtc_bot/lib/[r]mtc_bot' 2>/dev/null || true)
if [ -n "$OLD" ]; then
  echo "  清理残留 bot 进程: $(echo $OLD | tr '\n' ' ')"
  kill $OLD 2>/dev/null || true
  sleep 2
fi
LOG="$EVI/deploy_match_${STAMP}.log"
echo "  启动程序（日志 $LOG）..."
export ROS_DOMAIN_ID="$DOMAIN"
export RMTC_TEAM_TOKEN="$TOKEN"
export RMTC_LLM_URL="$GATEWAY"
export RMTC_DECISION_CONFIG="$CFG"
setsid "$ROOT/run_contestant.sh" "$SEAT" "$NAME" "$GATEWAY" >"$LOG" 2>&1 &
BOT_PID=$!
sleep 3
if ! kill -0 "$BOT_PID" 2>/dev/null; then
  echo "  [中止] 程序 3 秒内退出，见日志 $LOG" >&2
  tail -20 "$LOG" >&2
  exit 1
fi
echo "===== 已启动（PID $BOT_PID）====="
echo "  观察要点（30 秒内确认，超时立即找台下工作学长）：
    1. 日志 $LOG 出现 [bot_node] 节点启动：席位=$SEAT ... 策略=combat
    2. 裁判页本席位回执闭环（invalid_actions=0）
    3. 裁判页 LLM 成功 N 次随公告增长（预算 30 次/场，上限 100）
    4. 异常处置：程序崩 → 重跑本命令；LLM 报错 → 本地兜底照常跑，不影响资格判定
    5. 比赛期间严禁接近裁判（0 分并取消面试/纳新资格）——任何问题找台下工作学长转达"
