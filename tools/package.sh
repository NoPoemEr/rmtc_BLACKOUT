#!/usr/bin/env bash
# 提交打包流水线：git 干净导出 → 干净全量构建 → 九套自检 → 提交压缩包 + sha256
#
# 用法: ./tools/package.sh [输出目录]
#   默认输出到 <仓库>/dist/
#
# 流水线（每次提交前跑一遍，10/30–31 提交当天必跑）：
#   1. git archive 导出 HEAD 已跟踪内容（自动排除 build/install/log 等忽略项）
#   2. 在临时目录全量 colcon build（不带 --symlink-install，验证源码自足）
#   3. 跑九套自检：九套必须全绿（collect/combat/decision 已于 10/25、10/27、10/28 实现，红灯即中止）
#   4. 生成 <dist>/rmtc_bot_submission_<时间戳>.tar.gz + .sha256
#   5. 全程不碰工作区 build/install/log，不影响正在联调的进程
#
# 注意：不能加 -u —— source /opt/ros/humble/setup.bash 会引用未定义变量
set -eo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$HERE/.."
OUT_DIR="${1:-$ROOT/dist}"
STAMP="$(date +%Y%m%d-%H%M%S)"
NAME="rmtc_bot_submission_${STAMP}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# 守门：工作区有未提交改动时中止——git archive 只导出 HEAD，
# 不提交就打包会静默产出落后于源码的 dist（10/21 实测踩过）
if [ -n "$(git -C "$ROOT" status --porcelain)" ]; then
  echo "==> 中止：工作区有未提交改动，先 commit 再打包（避免 dist 落后于 HEAD）" >&2
  git -C "$ROOT" status --short >&2
  exit 1
fi

echo "==> [1/4] git archive 导出 HEAD（干净导出，排除忽略项）"
cd "$ROOT"
git archive --format=tar -o "$WORK/src.tar" HEAD
mkdir -p "$WORK/pkg"
tar -xf "$WORK/src.tar" -C "$WORK/pkg"
git rev-parse --short=12 HEAD > "$WORK/pkg/.git_commit"

echo "==> [2/4] 临时目录全量构建（验证导出内容自足）"
source /opt/ros/humble/setup.bash
(cd "$WORK/pkg/contestant_ws" && colcon build)

echo "==> [3/4] 九套自检（在导出的工作区上跑）"
source "$WORK/pkg/contestant_ws/install/setup.bash"
MUST_FAIL=0
check() {  # check <可执行名> <标签>
  local bin="$1" label="$2"
  set +e
  local out; out="$(ros2 run rmtc_bot "$bin" 2>&1)"; local rc=$?
  set -e
  if [ $rc -eq 0 ]; then
    printf '  [绿] %s\n' "$label"
  else
    printf '  [红] %s（必须绿，红灯则中止打包）\n' "$label"
    # M7 修复：grep 无匹配退出 1 会中断脚本，先取回再展示
    fail_lines="$(printf '%s\n' "$out" | grep -E '\[FAIL\]' || true)"
    if [ -n "$fail_lines" ]; then
      printf '%s\n' "$fail_lines" | head -3 | sed 's/^/       /'
    fi
    MUST_FAIL=$((MUST_FAIL+1))
  fi
}
check perception_selftest "perception 感知解析"
check strategy_selftest   "strategy 策略接口/配置"
check llm_selftest        "llm LLM 客户端"
check llm_conclusion_selftest "llm_conclusion LLM 结论解析/校验"
check llm_brain_selftest   "llm_brain LLM 触发/提示词"
check combat_llm_guide_selftest "combat_llm_guide LLM 意图翻译层"
# M11 修复：collect/combat 已实现（10/25、10/27），提交日回归成红必须中止出包
check collect_selftest    "collect 第一阶段策略"
check combat_selftest     "combat 第二阶段规则层"
check decision_selftest   "decision 决策内核+LLM顾问"

if [ $MUST_FAIL -gt 0 ]; then
  echo "==> 中止：必须绿的自检出现红灯，提交包未生成" >&2
  exit 1
fi

# 删除临时构建产物：提交包只含源码（build/install/log 夹带临时绝对路径，
# 且每次构建字节都变 → sha256 不可复现；10/21 实测踩过）
rm -rf "$WORK/pkg/contestant_ws/build" "$WORK/pkg/contestant_ws/install" \
       "$WORK/pkg/contestant_ws/log"

echo "==> [4/4] 生成提交压缩包 + sha256"
mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"
tar -C "$WORK/pkg" --null -T <(cd "$WORK/pkg" && find . -maxdepth 1 ! -name . ! -name .git_commit -print0) \
    --transform='s,^\./,,' -czf "$OUT_DIR/$NAME.tar.gz"
(cd "$OUT_DIR" && sha256sum "$NAME.tar.gz" > "$NAME.tar.gz.sha256")

echo
echo "==> 打包完成"
echo "    压缩包: $OUT_DIR/$NAME.tar.gz"
echo "    校验和: $OUT_DIR/$NAME.tar.gz.sha256"
echo "    源码 commit: $(cat "$WORK/pkg/.git_commit")"
sha256sum "$OUT_DIR/$NAME.tar.gz"
